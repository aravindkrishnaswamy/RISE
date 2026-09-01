# Object Modeling Recipes
> hook: Read when building an actual OBJECT (a mug, a table, a lamp) rather than blocking out a generic scene -- the geometry vocabulary, per-type gotchas, and CSG/SDF composition recipes.

FURRED OR HAIRED SUBJECT IN THE SCENE (cat, dog, any animal, a person
with hair)?  Read `hair-and-fur` BEFORE modelling its body -- an animal
is a groom plus a body, and the groom decides how the body should be
sculpted.  This skill covers only the solid parts.

This skill goes deep on modeling real objects.  For the observe loop
(cheap preview renders from multiple angles), the basic primitive
table, `csg_object`'s no-scale rule, and the cylinder-axis default,
read modeling-workflow-and-geometry FIRST -- this skill builds on it
rather than repeating it.

## The full geometry vocabulary

Everything registered in `src/Library/Parsers/ChunkParserRegistry.cpp`
as a `*_geometry` chunk (or the CSG combinator).  Do not invent kinds
not listed here.

| Chunk | What it's for | Cost | Gotcha |
|---|---|---|---|
| `sphere_geometry` | balls, lenses, blob primitives | cheapest analytic | `radius` only, centered at local origin |
| `box_geometry` | crates, tabletops, slabs, blockout | cheapest analytic | axis-aligned in LOCAL space; `width/height/depth` = X/Y/Z extents (full, not half) |
| `cylinder_geometry` | pipes, poles, mug bodies, legs | cheap analytic | default `axis` is `x` (lies on its side); `height` centers on the origin along that axis |
| `torus_geometry` | rings, handles, chain links | cheap analytic | ring axis is always Y (lies flat in XZ by default); `minorratio` is the tube radius as a FRACTION of `majorradius`, not an absolute size |
| `ellipsoid_geometry` | eggs, squashed balls, gems | cheap analytic | `radii` is per-axis semi-axes (like a scaled sphere baked into the geometry, not a `standard_object.scale`) |
| `infiniteplane_geometry` | floors, walls, backdrops | cheap analytic, unbounded | the CHUNK itself only takes `name`/`xtile`/`ytile` -- placement/tilt comes from the enclosing `standard_object`'s `position`/`orientation` (universal for every geometry chunk); default lies in XY facing +Z before that transform |
| `clippedplane_geometry` | bounded floors, area-light quads, framed backdrops | cheap analytic | four explicit corners; vertex WINDING picks which side renders/emits |
| `csg_object` | booleans of two already-declared objects | cost of both operands + one more test | **no `scale` parameter** -- size the operands, not the CSG result |
| `sdf_geometry` | melded/filleted/tapered organic shapes (fillets, cones, capsules, smooth unions) that no analytic primitive covers.  **Before composing several parts for one rounded MASS -- a cushion, a torso, a pebble, a soft-cornered slab -- try a single `superellipsoid` part**, the continuum primitive: `a` = radius, `b` = e1 (north-south exponent), `c` = e2 (east-west), proportions from the part's own `<sx sy sz>`, `round` unused.  `b`=`c`=1 is an ellipsoid, both toward 0 a box, `b` toward 0 with `c`=1 a cylinder, `b`=`c`=2 an octahedron, and **0.4-0.7 is the cushion/torso range** (both clamped to [0.1, 2]).  One part spans that whole family, so a roundbox-plus-blend stack, or a box intersected with a sphere, for the same shape is work you no longer have to do | sphere-traced -- more expensive per-hit than an analytic primitive, cost scales with `maxsteps` | inline `part` lines compose in order; the FIRST part must be `union` or `smin` (the field starts empty); see the lamp recipe below for the field layout |
| `lathe_geometry` | **SURFACES OF REVOLUTION -- the turned/lathe verb** (bottles, jars, vases, mortars, goblets, urns, turned legs, finials, lamp bases -- see "Turned forms" below) | mesh cost (tessellated once), no sphere-tracing | repeated `profile_point <r> <h>` lines ARE the silhouette (`r` = radius from the axis, `h` = height along it), spun about `axis` (default `y`); a point at `r 0` sits ON the axis and collapses to a single pole, so a profile that starts and ends there is closed and watertight with no caps; `sweep_degrees` under 360 cuts a capped section; the baked mesh is DOUBLE-SIDED, so a luminaire material on it radiates inward too |
| `skeleton_geometry` | **CREATURE BODIES authored as a JOINT GRAPH** (a hip branching into two legs and a tail, a hand's finger tree) -- `joint <name> <parent\|none> <x> <y> <z> <radius> [aspect]` lines, one per joint; expands at parse time into ONE `sdf_geometry` (a `roundcone` bone per parent->child pair, `smin`-blended) | sphere-traced, same cost model as `sdf_geometry`; `Map()` is O(joint count) per step with no acceleration over bones -- a skeleton is a render-time budget (a hand-authored SDF has a handful of parts, a skeleton invites 30-70) | a bone's own end caps ARE its two joints -- do not also add a `sphere_geometry`/extra `part` at a joint already covered by an incident bone, that just double-blends a redundant primitive; `blend` multiplies the SMALLER of the two joint radii, not either one alone; a bone's cross-section is a CIRCLE unless the joint line's optional 7th token `aspect` (width-to-depth ratio, 1 = round, ~0.6 a thigh) flattens it side-to-side -- and a body MASS that is really a cushion (a torso, an abdomen) is one `superellipsoid` part in a plain `sdf_geometry`, and a limb that FOLLOWS A CURVE is `sweep_geometry`; the manual `part` grammar stays the fallback for anything the joint graph can't express |
| `sweep_geometry` | tubes, rails, mouldings, cable runs, any TUBE THAT FOLLOWS A CURVE (a retort's neck, a spout, a handle, a bail), and -- via the per-station controls -- BODIES WITH A NON-CIRCULAR, CHANGING SECTION (a torso, a fin, a snout, a strap, a hull, a round-to-square leg) | mesh cost (tessellated once) | the cross-section is NOT fixed: `point_scale <sx> <sy>` scales the two profile axes independently per station, and a second profile (`profile2_*`) plus `point_morph <t>` changes the section's OUTLINE along the path -- but it still interpolates at most TWO sections, so it is NOT a lathe (see "Turned forms" below); open by default, `path_closed TRUE` sweeps a seamless loop instead (handles, wreaths, non-circular rings) -- `torus_geometry` is still cheaper for a plain circular ring; a NON-periodic (non-tiling) wrapping V texture shows a one-band rewind stripe at a closed loop's seam -- the geometry itself is seamless, but the texture content isn't unless it repeats at V=1==V=0 |
| `path_instances_geometry` | fence posts, rivets, beads, chain links along a path | one tessellation + N cheap instances | template +Y aligns with the path tangent -- orient the template accordingly before instancing; the template can be ANY first-class geometry INCLUDING a `displaced_geometry` (it is realized + tessellated through the same universal contract before stamping) |
| `displaced_geometry` | bumpy/organic surfaces (a `base_geometry` tessellated + offset by a painter) | tessellation + per-vertex offset | prefer FEWER bumps with LONGER wavelengths -- finer `detail` does not fix a too-busy displacement (SMS docs lesson); `base_geometry` composes over ANY geometry INCLUDING `sdf_geometry`, `lathe_geometry`, `sweep_geometry` and `skin_geometry` (Recipe 6 below), but a `lathe_geometry`/`sweep_geometry`/`skin_geometry` base goes visibly FACETED under any non-zero displacement (their bake re-emits unshared per-triangle-corner topology, so post-displacement shading has nothing to average across) -- `sdf_geometry` bases stay smooth |
| `circulardisk_geometry`, `cartesian_disk_geometry` | flat disks (dials, coins, disk-shaped bases) | cheap | `cartesian_disk_geometry` has uniform Cartesian UV density; the polar disk does not -- pick by what you're displacing/texturing onto it |
| `bezierpatch_geometry`, `bilinearpatch_geometry` | authored curved/patch surfaces | analytic (bezier) / cheap (bilinear) | `bezierpatch_geometry`'s old tessellation params (`detail`, `cache_size`, ...) are retired -- wrap it in `displaced_geometry` if you need that control |
| Mesh imports (`3dsmesh_geometry`, `rawmesh_geometry`/`rawmesh2_geometry`, `risemesh_geometry`, `plymesh_geometry`, `gltfmesh_geometry`) | authored/imported assets that are not primitive-shaped | mesh cost | `file` path resolved via the media-path search (see the reference-image skill for the exact resolution order); declare before the `standard_object` that references it |

## Turned forms are a PROFILE, never a stack of cylinders

**If the object's silhouette is a solid of revolution -- bottle, jar,
flask, retort, vase, cup, bowl, mortar, candlestick, goblet, urn,
barrel, turned table leg, finial, decanter -- author it as a PROFILE:
one continuous radius-versus-height curve.  Do NOT stack cylinders.**

This is the single change that most separates a render that reads as a
real object from one that reads as cartoonish and amateurish.  A stack
of `cylinder_geometry` chunks gives you a silhouette made of straight
vertical segments joined by hard right-angle steps, and every one of
those steps is a place the real object has a continuous curve.  The eye
reads the steps instantly, at any resolution, under any material.  No
amount of lighting or material work rescues it.  Three cylinders of
decreasing radius is not a bottle; it is three cylinders.

**The verb is `lathe_geometry`.**  One chunk, one profile: repeated
`profile_point <r> <h>` lines are the silhouette itself -- `r` the
radius from the axis, `h` the height along it -- revolved about `axis`
(default `y`, so the profile's `h` runs straight up the object).  Write
the (radius, height) pairs down the way you would read them off a
photograph, bottom to top; a point at `r = 0` sits ON the axis and
collapses to a single pole vertex, so a profile that starts and ends
there is a closed, watertight vessel needing no caps.  Bottles, jars,
vases, goblets, urns, turned table and chair legs, finials, lamp bases
and pedestals are all this one chunk.

**The FALLBACK verb is `sdf_geometry` with `roundcone` parts joined by
`smin`** -- reach for it when the turned form must also take part in
CSG (a drilled bore, a clipped taper), needs a non-circular
cross-section, or has to blend into a larger implicit body.
`roundcone` IS a profile segment: `<r1> <r2> <h>` is a frustum that runs
along local +Y from radius `r1` at `y=0` to radius `r2` at `y=h`, i.e.
exactly one (height, radius) span of your profile.  Chain them
end-to-end -- each part's `y` position is the previous part's top -- and
join them with `smin <k>` instead of `union`.  `k` is the blend radius
in world units, and it is the whole point: `smin` fillets the joint
between two segments into a continuous curve, so the silhouette flows
where a hard `union` (or a cylinder stack) would step.  Pick `k` around
a third of the local radius; larger `k` = softer shoulder.

**A FOUR-LEGGED creature body: start from
`insert_geometry_scaffold family:quadruped`, don't author the joint
graph from scratch.**  It
emits ONE `skeleton_geometry` chunk with a properly-proportioned generic
quadruped skeleton -- spine, neck, head, muzzle, two ears, a tapering
tail, and four legs (upper/lower/paw) -- ready to rename, reposition,
and re-radius joint by joint.  `build` (`lean` / `average` / `stocky`)
picks a stance; `size` scales the whole animal; `name` seeds
deterministic per-instance variety the same way `insert_material_scaffold`
does, so two calls with the same name+size+build are byte-identical and
a different name still reads as a DIFFERENT animal.  One call:
`insert_geometry_scaffold {"family":"quadruped","name":"fox","size":1.2,
"build":"lean"}`.  This is the fast, safe path for the common case (a
generic mammal body) -- it never needs a `detail`/`aspect` argument, and
its proportions (including the ear-to-head ratio) are already swept
clean against the SAME blend-scale law below.  Reach for hand-authored
`skeleton_geometry` (the rest of this section) when the scaffold's
generic anatomy is the wrong STARTING shape -- a non-mammal body plan,
an unusual limb count, or a graph the scaffold's fixed topology cannot
express -- and always reach for it to EDIT what the scaffold produced:
the emitted chunk is ordinary `skeleton_geometry` text, not a black box.

**POSING what the scaffold gave you is a PROCEDURE, not a shape to pick
off a list.** There is no `pose` parameter -- a curled-asleep animal,
a sitting dog, a rearing horse are all the SAME mechanism (joint-by-joint
edits to the standing skeleton), not a fixed enum of presets a scaffold
could enumerate for you. This is the actual loop, in order, every time:

1. **Start from the scaffold's standing output** (`insert_geometry_scaffold
   family:quadruped`), or your own hand-authored `skeleton_geometry` --
   either way you now have a `joint` graph you can read back with
   `read_document`.
2. **Edit joints ONE AT A TIME, or in small batches, with `propose_patch`'s
   `occurrence`.** `skeleton_geometry`'s `joint` lines are a REPEATABLE
   param -- `param:"joint"` alone always means occurrence 0 (the FIRST
   joint), no matter which one you mean. Pass `occurrence` (0-based,
   the same order `read_document` lists the `joint` lines in: the 5th
   line is `occurrence:4`) to address the specific joint you are posing --
   e.g. bending a hind leg means patching `BL_lower`'s occurrence with a
   new `y`/`z`, not re-authoring the whole chunk. Read the current line's
   text first (from `read_document`) so your replacement value keeps every
   token you are not changing.
3. **After each small batch, LOOK -- don't pose blind.** `reopen_element`
   (re-enter the element's window; free, never gated, legal from any
   phase) -> your `propose_patch` edits -> `finish_element` (closes the
   element and hands you an isolate render: a draft/form panel and a
   lit-material panel, side by side) -> if the silhouette is not yet
   right, `reopen_element` the SAME name again and keep going. Both verbs
   are free and ungated specifically so this cycle is cheap; use it after
   every batch, not once at the end.
4. **Where the pose puts two joint-graph-UNRELATED parts close together on
   purpose -- a curled tail against the flank, a tucked muzzle near a
   haunch -- give the bone(s) nearest that seam a tight per-joint `blend`
   override** (the `joint` line's optional 8th token: `<name> <parent>
   <x> <y> <z> <radius> <aspect> <blend>`; requires `aspect` to be spelled
   too, purely positional grammar). This is NOT the same knob as the
   chunk's own `blend` parameter -- it replaces that multiplier for JUST
   the one bone arriving at that joint, near-0 for a near-hard seam, left
   absent (inherits the chunk default) everywhere else. Why this works:
   `Map()`'s smin fold is sequential over EVERY part in the chunk, so
   any two parts that end up spatially close will blend UNLESS the LATER
   one's own k is small -- a pose-induced touch (nose near tail) is
   exactly the case a fixed chunk-wide `blend` cannot express, because
   turning it down everywhere flattens the spine/limb continuity you
   still want. Tighten only the bone(s) actually at the seam.
5. **`fix_blend_scale` is for GENUINE accidental over-blends, not for
   melds you meant.** An undirected call (`target` omitted) now SKIPS a
   chunk whose smin joints are MOSTLY flagged -- a curled pose or any
   other deliberately tight organic mass reads that way on purpose, and
   mass-clamping it would flatten exactly the overlap you built. If you
   genuinely do want that one chunk clamped anyway, pass `target:"<name>"`
   explicitly -- that always clamps, no matter how many joints it flags.

**Worked example -- a curled-asleep fox, expressed as the procedure above**
(not as a shape to copy verbatim; the numbers are illustrative, the STEPS
are the point):

- Step 1: `insert_geometry_scaffold {"family":"quadruped","name":"fox","size":1.2,"build":"lean"}`.
  `read_document` shows joints `hips`, `spine_mid`, `chest`, `neck`, `head`,
  `muzzle`, `ear_l`, `ear_r`, `tail1..3`, and the four legs
  (`FL/FR/BL/BR_upper/lower/paw`), all standing.
- Step 2, in small batches (`reopen_element`/`finish_element` around each,
  per step 3): (a) patch `hips`, `spine_mid`, `chest` by occurrence,
  curving their `x`/`z` into an arc so the spine reads as a C, not a
  straight line; (b) patch `neck`/`head`/`muzzle` so the head follows the
  SAME arc and comes to rest near (not on top of) the hip/tail region;
  (c) patch each leg's `upper`/`lower`/`paw` occurrences so the paws tuck
  in under the curled mass instead of standing; (d) patch `tail1..3` so
  the tail continues the spine's own curl outward, wrapping alongside the
  body toward the muzzle with a real gap, not touching it.
- Step 4: the muzzle and a tail segment now sit close together on
  purpose -- give ONE of that pair a tight `blend` override, and it
  matters WHICH: `skeleton_geometry`'s `joint` parameter description
  states the rule plainly -- two spatially-close, graph-unrelated joints
  bridge under the LATER-DECLARED one's own blend, so the override goes
  on whichever of the two comes LATER in the `joint` list, not
  necessarily the one that reads as "the seam" to your eye. In the usual
  scaffold declaration order (`hips`...`muzzle`, THEN `tail1..3`), that
  is the tail segment, not the muzzle: e.g. `tail3 tail2 <x> <y> <z>
  <radius> 1 0.02`. Overriding the earlier-declared joint of the pair
  does nothing -- read back the actual declaration order before picking
  which one to tighten, do not assume.
- Step 5: `fix_blend_scale` (no `target`) on the finished chunk -- with
  most of its own joints now legitimately close together, expect it to
  report the mass-clamp caveat and skip the chunk, which is the CORRECT
  outcome here (the tight overlap is the curl, not a mistake). Only pass
  `target` if a specific joint's blend genuinely looks wrong on the
  isolate render, not because the sweep flagged it.
- Verify by LOOKING (step 3, one more time) from a high 3/4 angle --
  curled poses read best from above-and-to-the-side, where the spine's
  own arc is visible.

Honesty note: this worked example has NOT been validated end to end as
a single finished render -- it illustrates the STEPS, not a proven
result. The two likeliest ways an attempt at it goes wrong, both
observed while writing it: executing it as a few large batches with a
look only at the end instead of the small-batch-then-look discipline
step 3 actually prescribes (a whole-body redesign between looks
converges far slower than many small, checked adjustments); and
tightening the earlier-declared joint of a close pair instead of the
later-declared one (step 4's rule) -- an override on the wrong side of
the pair silently does nothing, and the bridge you were trying to fix
is still there on the next render.

**A BRANCHING body (a creature, not a single profile) has its own
chunk now: `skeleton_geometry`.**  Hand-chaining `roundcone` parts this
way is still the right tool for a shape neither the lathe nor the joint
graph can express (a non-circular cross-section, a hollow interior, a
profile that has to blend into a larger implicit
body).  But a body with more than one limb meeting at a
shared joint -- a hip branching into two legs and a tail, a hand's
finger tree -- is a GRAPH, not a chain, and hand-authoring it as
`sdf_geometry` `part` lines means re-deriving each bone's position,
length, and orientation by hand.  `skeleton_geometry` takes the graph
directly: one `joint <name> <parent|none> <x> <y> <z> <radius> [aspect]
[blend]` line per joint (a bone is implied between every joint and its
parent; `blend` overrides the chunk's own `blend` for just that bone --
see "POSING" above for when to reach for it), and
it expands into the same `roundcone`-chain-joined-by-smin `sdf_geometry`
this section teaches, so everything below about profile-reading and
flat-bottom cuts still applies to how each individual bone is shaped --
`skeleton_geometry` only automates the graph assembly, not the
primitive vocabulary.  See the geometry vocabulary table above.

**Compose a creature from all three verbs, never one: a BODY is
overlapping MASSES (two or three `superellipsoid` parts `smin`-blended
in one `sdf_geometry`, exponents CLOSE TO 1.0 -- an organic torso or
head mass wants an ellipsoid, not the vocabulary table's 0.4-0.7 cushion
range, which is for a literal pillow/cushion shape and reads as a
rounded CUBE on a body), LIMBS are CHAINS (`skeleton_geometry`, with the
per-joint `aspect` token flattening any bone that should not read as a
pipe), and a NECK or TAIL is a SWEEP** -- steering a whole animal into a
single sweep or a single bone chain is what makes it silhouette as a
bent tube.

A minimal worked graph -- a hip branching into two legs, five joints in
one chunk: `joint hips none 0 0.5 0 0.22`, `joint hip_l hips -0.15 0.4 0
0.10`, `joint hip_r hips 0.15 0.4 0 0.10`, `joint paw_l hip_l -0.18 0.05
0.05 0.05`, `joint paw_r hip_r 0.18 0.05 0.05 0.05`.  The hand-authored
`sdf_geometry` equivalent is five `roundcone` parts whose position,
orientation and length you would otherwise compute from these same five
points by hand -- `skeleton_geometry` reads the points directly.

**A WING, FIN or web is a `skin_geometry` between two rails -- the bones
are the skeleton, the membrane stretched over them is the skin** (`rail_a`
the leading edge, `rail_b` the trailing edge, both traced root to tip, and
a small `billow` to fill it); the same chunk is a sail, a leaf, an awning
or a lampshade panel.

Reading a profile off a reference is mechanical.  Write down (radius,
height) pairs from the bottom up -- base, belly, shoulder, neck, lip --
and emit one `profile_point` per pair, in that order.  Recipe 4 below
does exactly this and renders.

Two things the profile approach needs:

- **A flat bottom is free on the lathe and needs a cut on the
  fallback.**  Start the profile at `0 0` (on the axis) and run straight
  out to the base radius at the same height: that horizontal first
  segment IS the flat base disc, no cap chunk and no boolean.  The
  `roundcone` fallback has no such move -- its `y=0` end carries a
  hemispherical cap of radius `r1`, so the bottom-most segment bulges
  below its own origin and a vessel authored naively sinks through the
  table.  End THAT part list with `part box subtract 0` positioned so
  the box's top face sits at the intended base plane.  (A round-bottomed
  florence flask genuinely wants the cap; leave it in that one case.)
- **`sweep_geometry` is still NOT the lathe verb -- but it is no longer
  a fixed-shape verb either.**  Three per-station controls, composed
  multiplicatively and applied in this order:
  - `point_morph <t>` with a second profile (`profile2_point` /
    `profile2_circle` / `profile2_rect`) changes the section's
    **OUTLINE** along the path: round skull into a narrow muzzle, a
    round shaft into a square post, a forearm flattening into a hand, a
    duct meeting a rectangular vent.  `t` runs 0 (first profile) to 1
    (second); omit `point_morph` and you get a linear 0 -> 1 ramp.
    A closed loop (`path_closed TRUE`) instead requires explicit
    `point_morph` values -- the ramp is refused there, since it would
    jump at the seam.
  - `point_scale <sx> <sy>` scales the two profile axes
    **INDEPENDENTLY** per station -- flatter than it is wide, and
    changing that ratio along the spine.  This is the torso / fin /
    strap / hull / keel / seat-rail control, and it is the form to
    reach for by default.  The 1-arg `point_scale <s>` still means
    UNIFORM on both axes (a genuinely round varying radius: a tapered
    tentacle, a tendril thinning to its tip).
  - `point_width <sx>` is the HISTORICAL x-only control.  Anything it
    can say, `point_scale <sx> <sy>` says with the y axis stated
    instead of left implicit; prefer the 2-arg form in new work.

  What still separates the two verbs: a lathe spins ONE silhouette
  about a fixed straight axis, so its outline is free to swell and neck
  arbitrarily along that axis; a sweep interpolates between at most TWO
  sections along an arbitrary 3D path.  A body whose profile changes
  character three or more times (a belly, then a waist, then a
  shoulder) is a lathe, or two chained sweeps -- not one sweep.  A TUBE
  THAT FOLLOWS A CURVE, with or without a changing section, is the
  sweep: a retort's curved neck, a spout, a handle, a bail, a cable, a
  tapered tentacle, a snout, a limb.  A retort is therefore both verbs
  -- a lathe profile for the bulb, a sweep for the neck -- and Recipe 4
  shows the pair.  For a round cross-section, `profile_circle <r> [n]`
  writes the bore in one line instead of a hand-listed `profile_point`
  N-gon (and `profile2_circle` / `profile2_rect` do the same for the
  morph target).

**When a cylinder IS the right answer** -- do not cargo-cult this into
banning cylinders.  `cylinder_geometry` is correct, and cheaper and
more exact than any SDF, whenever the thing genuinely IS a cylinder:
a straight shaft or rod, a peg, a dowel, a pipe or tube seen straight,
a cork, a candle body, a coin or puck, a drinking glass with truly
straight sides, a table leg that is not turned, a drum.  The test is
whether the real object's radius CHANGES along its axis.  Constant
radius, flat ends: use the cylinder.  Radius that swells and necks:
use a profile.

## Blockout -> refine workflow

Proportions before detail, always, and cheap previews before a full
render -- the same observe loop, applied at the object level:

1. **Blockout with the cheapest primitives that suggest the silhouette.**
   A mug is a cylinder before it is a CSG hollow shell; a lamp is three
   stacked primitives before any of them has a material pass.  Use
   `render {width:128, height:128, samples:1}` and a plain
   `lambertian_material` on everything -- you are checking proportions,
   not shading.
2. **Render from 2-3 angles before trusting the proportions.**  A mug
   blocked out as a lone cylinder looks identical to a soup can from
   the front; the second angle (see the camera-override recipe in
   modeling-workflow-and-geometry) is what tells you the handle is
   actually attached to the body and not floating beside it.
3. **Swap in the real geometry (CSG hollow, SDF taper, torus handle)
   once the blockout proportions read correctly** -- fixing proportions
   is cheap at the primitive stage and expensive after a CSG/SDF pass
   is wired in (each `csg_object`/`sdf_geometry` edit is a bigger,
   costlier re-derive than moving a primitive's `position`).
4. **Materials and lighting passes come LAST.**  A correctly-shaped
   gray blockout under one directional light tells you everything
   about form; add real colors/dielectric/metal only once the shape
   itself is confirmed (see materials-and-media-basics).
5. **`render {imageMaxEdge:...}`** once you actually need to SEE the
   frame (silhouette, proportion relationships) rather than just check
   the render's channel means -- the image rides back in that one call.

**A one-call materials pass.** Once the blockout proportions are
confirmed, `insert_material_scaffold {family:"rough_stone",
name:"block1", tone:"0.5 0.48 0.44", wear:0.6, scale:1.2}` expands a
family template (`weathered_wood`, `rough_stone`, `brushed_metal`,
`aged_bronze`, `glazed_ceramic`) into a wired painter graph -- here a
`cooktorrance_material` (`tmpl_block1_mat`) with both `rd` and `facets`
bound to real painters -- in place of hand-typing the painter chain
yourself; point any `standard_object.material` at `tmpl_block1_mat` and
continue the refine pass from there.

## Recipe 1: a mug (`csg_object` hollow body + `torus_geometry` handle)

A cylinder body with a second, slightly-taller and narrower cylinder
subtracted out to hollow it (open top, solid base), plus a torus ring
rotated on its side for the handle.  Carries forward the CSG no-scale
rule from modeling-workflow-and-geometry: both operand cylinders are
sized BEFORE the `csg_object` combines them.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	3 2 4
	lookat		0 0.4 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
}

uniformcolor_painter
{
	name	pnt_mug_lo
	color	0.75 0.78 0.86
}

uniformcolor_painter
{
	name	pnt_mug_hi
	color	0.94 0.94 0.98
}

# Billowy-but-clumpy blend reads as a mottled ceramic glaze rather than
# a flat-painted mug.
perlinworley3d_painter
{
	name			pnt_mug
	colora			pnt_mug_lo
	colorb			pnt_mug_hi
	octaves			3
	persistence		0.55
	worley_jitter	1.0
	blend			0.5
	scale			6.0 6.0 6.0
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_mug
	reflectance	pnt_mug
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.01 0
	orientation	-90 0 0
}

# Outer body and the bore that hollows it -- both sized here, BEFORE
# the csg_object combines them (csg_object itself takes no scale).
cylinder_geometry
{
	name	body_outer
	axis	y
	radius	0.5
	height	1.0
}

standard_object
{
	name		obj_body_outer
	geometry	body_outer
	material	mat_mug
}

# Taller and narrower than the outer body, open past the top and
# stopping short of the base -- the bore that leaves a solid bottom
# and an open rim.
cylinder_geometry
{
	name	body_inner
	axis	y
	radius	0.42
	height	1.2
}

standard_object
{
	name		obj_body_inner
	geometry	body_inner
	material	mat_mug
	position	0 0.05 0
}

csg_object
{
	name		obj_mug_body
	obja		obj_body_outer
	objb		obj_body_inner
	operation	subtraction
	material	mat_mug
	position	0 0.5 0
}

# Handle: torus_geometry's ring axis is always Y (flat in XZ by
# default) -- rotate 90 about Z so the ring stands up against the
# mug's side, then position it overlapping the body so it reads as
# ATTACHED rather than floating beside it.
torus_geometry
{
	name	handle_torus
	majorradius	0.28
	minorratio	0.18
}

standard_object
{
	name		obj_handle
	geometry	handle_torus
	material	mat_mug
	position	0.62 0.5 0
	orientation	0 0 90
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Rendered at 128px this reads as a mug: a cylindrical body with a
visibly hollow dark rim (the CSG bore) and a side handle merged into
the body -- not a cylinder with a stray ring floating next to it.  If
your render shows the handle detached, it is not overlapping the
body's outer radius; increase the X position past `body radius -
torus minorratio*majorradius` or bring it closer to `0.5`.

## Assemblies are subtrees; repeats are one chunk

Objects form a TREE, not a flat list.  Two parameters on
`standard_object` carry all of it, and both are worth reaching for
before you write the tenth near-identical chunk:

- **`parent <object>`** -- the node's transform becomes LOCAL, and its
  world transform is `parent.world * local`.  Move the parent and the
  whole subtree moves.  A `standard_object` with NO `geometry` is a
  pure CONTAINER: invisible to the renderer, but a real node whose
  transform everything under it composes against.  That is what an
  assembly is -- one container plus its parts -- and it is what lets
  you place, rotate or scale a bench, a lamp or a whole building with
  ONE edit instead of N.
- **`source <object>`** -- this node becomes an INSTANCE of that
  object.  If the source has children, its whole subtree is copied and
  the descendants are named `<this name>.<their name>`.  Add
  `count_u U` (and optionally `count_v V`) and the whole instance is
  repeated `U x V` times as `<this name>[i,j]`, with the instancing
  chunk's own parameters free to be per-component `expr(...)` over the
  instance variables `i` / `j` (indices) and `u` / `v` (the same,
  normalized into [0,1]).
- **`mirror x|y|z`** -- reflects the node (and everything parented under
  it, including a whole cloned subtree) across the plane through its own
  origin, so a bilateral pair is ONE authored half plus
  `source <half>  mirror x` instead of two hand-built ones that drift
  apart.  Author the half UN-mirrored and put the `mirror` on the copy:
  `mirror` is instance-own, never inherited through `source`, so a plain
  `source` of an ALREADY-mirrored node drops its reflection and comes out
  as that node's mirror image (repeat the same `mirror <axis>` on the copy
  if you wanted an exact duplicate -- the derive warns when you have not).

None of this WELDS anything: RISE only reads two chunks as connected where
their geometry actually overlaps, so any attaching part -- a leg into a
torso, a handle into a mug, a membrane into the body it grows from -- must
penetrate its neighbor, not merely touch it, or displacement/blending will
open a seam.  The converse trap: the coordinates in a `point`/`joint`/
`profile` line are the AXIS of a solid, not its surface -- the surface lies
the local radius away from every number you can read, so anything placed AT
another chunk's own coordinates lands at its core (a rivet row using a
pipe's path points sits inside the pipe; a handle started at a vase's
profile coordinates starts at the vase's centerline).  Attaching parts go
PAST the surface; surface-riding parts go OFFSET OUTWARD from the axis.

Read `read_schema` for `standard_object` for the full parameter text;
what follows is the part the schema cannot tell you -- when to reach
for them, and what bites.

**When to reach for them.**  Copy-paste is right for a handful of
objects that differ in more than their placement -- and "a handful"
stretches further than it sounds: a dozen deliberately varied
apothecary bottles are twelve better-authored objects than one array
of twelve stamped ones, because the variety is the point.  A subtree
is right the moment you would otherwise repeat a placement edit; an
instance array is right once the repeats are LONG and REGULAR --
think ten-or-more identical units on a line or grid where uniformity
IS the point and hand-authoring each one is a real, measurable burden:
fence posts, a colonnade, a rivet row.  The array is also ONE row in
the outliner and ONE chunk to edit -- change `count_u` and the whole
arrangement changes -- where thirty pasted chunks are thirty edits.
Recipe 2 below sits comfortably on the copy-paste side of that line: a
table's four legs share ONE geometry across four `standard_object`s,
and four is the right shape -- and so is a shelf lined with a half
dozen bottles that vary from one another, which is authoring, not a
smell. A fence of thirty pickets, or a colonnade of a dozen matched
columns, is past the line -- author ONE and let `count_u` write the
rest.

**Already pasted them?  `collapse_to_instances` fixes it in one
call.**  It keeps the first copy, replaces the rest with the `source`
+ `count_u` chunk that reproduces exactly the positions they already
had, and leaves the rendered image unchanged -- one call, one undo
step.  Called with no arguments it takes the largest such run in the
scene, which is what a DESIGN NOTE about repeated copies is pointing
at.  It REFUSES, changing nothing, when the copies are not on a
regular line or grid, when they differ in more than `position`, or
when another chunk names one of them -- so trying it is free, and a
refusal is an answer to read rather than a cue to hand-write the
chunk yourself.

**Five things that bite.**

1. **A `parent` and a `source` must be DECLARED EARLIER in the file.**
   Same rule as every other reference.  So an assembly reads top-down:
   the container first, then its parts.
2. **`source` COPIES; it does not move or hide anything.**  The source
   object keeps rendering where it is.  So an array of `count_u 5` off
   a visible source gives you SIX, not five -- author the source where
   you actually want one of them, as the example below does.
3. **A repetition is not a chunk.**  `grid[3,1]` is a real object with
   a real name you can see in `scene_inventory`, but there is nothing
   named `grid[3,1]` to patch.  Edit the instancing chunk.
   `scene_inventory` names it for you under `instancedFrom`.
4. **A CSG operand cannot be parented.**  An operand's transform is
   read in the composite's frame, so parent the `csg_object` itself and
   leave `obja` / `objb` unparented.
5. **The counted form always names `[i,j]`,** even at `count_u 1` --
   so a `parent` pointing at a counted chunk must say `parent
   name[0,0]`, not `parent name`.

**Scaling a container is a scene-graph scale**, and every component
must be greater than zero: a zero makes the composed matrix singular
for every object under it, which is a whole subtree of wrong
intersections rather than one flat object.

If you are working through the staged build protocol, this is already
happening for you: every object you create inside an element's window
is parented to that element's root container, and `place_element`
writes one transform onto it.  Reach for `parent` yourself for
hierarchy INSIDE an element -- a hand under a wrist, a shade on a lamp
-- and for `source` whenever a part repeats.

```rise
RISE ASCII SCENE 7

# A fence: ONE post authored, five more from ONE instancing chunk, all
# hanging off a container so the whole run swings with a single
# `orientation`.  Nine objects; the parts a human edits are four chunks.

standard_shader
{
name global
shaderop DefaultPathTracing
}

pathtracing_pel_rasterizer
{
samples 12
pixel_filter box
oidn_denoise FALSE
}

film
{
width 96
height 72
}

pinhole_camera
{
name cam
location 3.4 2.6 6.6
lookat 2.1 0.7 0
up 0 1 0
fov 42
}

uniformcolor_painter
{
name pnt_wood
color 0.52 0.36 0.22
}

lambertian_material
{
name mat_wood
reflectance pnt_wood
}

uniformcolor_painter
{
name pnt_ground
color 0.55 0.55 0.52
}

lambertian_material
{
name mat_ground
reflectance pnt_ground
}

clippedplane_geometry
{
name geo_ground
pta -9 0 -9
ptb 9 0 -9
ptc 9 0 9
ptd -9 0 9
}

box_geometry
{
name geo_post
width 0.16
height 1.2
depth 0.16
}

box_geometry
{
name geo_rail
width 4.8
height 0.14
depth 0.1
}

standard_object
{
name ground
geometry geo_ground
material mat_ground
}

# THE CONTAINER.  No `geometry`, so it renders nothing -- it is a
# transform the three chunks below compose against.  Change this one
# `orientation` and the whole fence turns.
standard_object
{
name fence
position 0 0 0
orientation 0 -12 0
}

# Post 0, authored once.  Its `position` is LOCAL to `fence`.
standard_object
{
name fence_post
parent fence
geometry geo_post
material mat_wood
position 0 0.6 0
}

# Posts 1..5, from one chunk.  `source` copies `fence_post`; `count_u`
# mints `fence_posts[i,0]` per repetition, and this chunk's own
# `position` is evaluated per repetition over `i`.  The SOURCE still
# renders at its own spot, which is why this starts at i*0.9 + 0.9 --
# six evenly spaced posts from two chunks.
standard_object
{
name fence_posts
parent fence
source fence_post
count_u 5
position expr(0.9+i*0.9) 0.6 0
}

standard_object
{
name fence_rail
parent fence
geometry geo_rail
material mat_wood
position 2.25 1.05 0
}

# `direction` is the vector FROM the surface TO the light, so a camera
# at +Z needs a POSITIVE z here.
directional_light
{
name key
power 2.4
color 1 0.97 0.90
direction 0.35 0.80 0.55
}
```

## Recipe 2: a table (primitive reuse, one geometry reused by four objects)

Declare the leg geometry ONCE and reference it from four
`standard_object`s at four positions -- geometry is a reusable
template, not copied per placement.  Four is the honest upper end of
hand-authoring: past about four near-identical chunks, author ONE and
repeat it with `source` + `count_u` instead (see "Assemblies are
subtrees; repeats are one chunk" above), so a railing or a picket
fence is two chunks rather than thirty.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	3.2 2.2 4.2
	lookat		0 0.6 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.45 0.45 0.45
}

uniformcolor_painter
{
	name	pnt_wood_pale
	color	0.55 0.35 0.18
}

uniformcolor_painter
{
	name	pnt_wood_dark
	color	0.24 0.12 0.045
}

# Anisotropic scale (tight across the plank, loose along it) reads as
# grain, not noise -- see procedural-textures for the full nested
# fibre+ring recipe.
perlin3d_painter
{
	name		pnt_wood
	colora		pnt_wood_pale
	colorb		pnt_wood_dark
	octaves		4
	persistence	0.65
	scale		9.0 1.0 0.45
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

uniformcolor_painter
{
	name	pnt_wood_spec
	color	0.4 0.4 0.4
}

# SCALAR pipe: varnish sheen worn thinner near the edges from handling.
expression_function2d
{
	name	fn_wood_wear
	param	bands 1.0
	def		s abs( u * bands - 0.5 )
	expr	smoothstep( 0.1, 0.5, s )
}

scalar_painter
{
	name		sp_wood_wear
	function2d	fn_wood_wear
	scale		0.45
	bias		0.03
}

ggx_material
{
	name		mat_wood
	rd			pnt_wood
	rs			pnt_wood_spec
	alphax		sp_wood_wear
	alphay		sp_wood_wear
	ior			1.5
	extinction	0.0
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.01 0
	orientation	-90 0 0
}

box_geometry
{
	name	tabletop
	width	2.0
	height	0.1
	depth	1.2
}

standard_object
{
	name		obj_tabletop
	geometry	tabletop
	material	mat_wood
	position	0 1.0 0
}

# One leg geometry, reused via FOUR standard_objects.
cylinder_geometry
{
	name	leg
	axis	y
	radius	0.06
	height	1.0
}

standard_object
{
	name		obj_leg_fl
	geometry	leg
	material	mat_wood
	position	0.85 0.5 0.5
}

standard_object
{
	name		obj_leg_fr
	geometry	leg
	material	mat_wood
	position	-0.85 0.5 0.5
}

standard_object
{
	name		obj_leg_bl
	geometry	leg
	material	mat_wood
	position	0.85 0.5 -0.5
}

standard_object
{
	name		obj_leg_br
	geometry	leg
	material	mat_wood
	position	-0.85 0.5 -0.5
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Rendered, this is unambiguously a table: a flat wooden slab standing
on four thin legs, each leg's shadow separated from the others (the
second-angle check from the observe loop confirms all four legs are
genuinely under the corners, not collapsed into one silhouette from
the authored camera).

### Recipe 2b: the same table top, painted field -> ramp

Recipe 2 gets its wood from a `perlin3d_painter` interpolating two flat
colours -- fine, and limited: two endpoints is all a noise painter has.
When the surface wants MORE than two tones (weathered timber going pale
grey at the wear lines, terrain, patina, glaze), split the job in two.
The one-line rule: **field in the expression, colour in the ramp.**
An `expression_painter` computes a scalar field and makes no
colour decision at all; a `ramp_painter` turns that field into as many
stops as the look needs, and the stop list is the thing a human edits.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.44 0.48 0.56
}

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples					12
	pixel_filter			box
	oidn_denoise			FALSE
	radiance_map			pnt_sky
	radiance_background		TRUE
}

film
{
	width	112
	height	112
}

pinhole_camera
{
	location	0 1.5 2.6
	lookat		0 0.25 0
	up			0 1 0
	fov			45.0
}

# THE FIELD.  Scalar-typed, anisotropic (tight across the plank, loose
# along it) so the bands read as grain.  `clamp(...*contrast + 0.5, 0, 1)`
# is the remap raw fbm always needs -- it spans roughly -0.4 .. 0.4, not
# [0,1].  Every knob is a `param` with a range, so the panel renders
# sliders and propose_patch retunes it by name.
expression_painter
{
	name		pnt_grain_field
	param		across 11.0 min 1.0 max 40.0 step 0.5 label "Grain frequency across"
	param		along 0.7 min 0.1 max 5.0 step 0.1 label "Grain frequency along"
	param		contrast 2.2 min 0.5 max 5.0 step 0.05 label "Grain contrast"
	seed		7.0
	def			q vec3(P.x*across, P.y*2.0, P.z*along) + vec3(seed, 0, seed*1.3)
	def			n fbm(q, 4, 0.55, 2.0)
	expr		clamp(n*contrast + 0.5, 0, 1)
}

# THE COLOUR.  Four stops instead of two endpoints: dark heartwood, mid
# oak, pale sapwood, bleached wear line.  Edit HERE, never in the body.
ramp_painter
{
	name			pnt_grain
	input			pnt_grain_field
	channel			R
	interpolation	smooth
	stop			0.00  0.07 0.030 0.012
	stop			0.42  0.26 0.130 0.050
	stop			0.78  0.52 0.310 0.140
	stop			1.00  0.66 0.520 0.380
	color_space		Rec709RGB_Linear
}

lambertian_material
{
	name		mat_wood_ramp
	reflectance	pnt_grain
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.34 0.34 0.36
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

box_geometry
{
	name	tabletop
	width	2.0
	height	0.12
	depth	1.1
}

standard_object
{
	name		obj_top
	geometry	tabletop
	material	mat_wood_ramp
	position	0 0.25 0
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.4 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		3.0
	color		1 0.98 0.94
	direction	0.3 0.6 0.75
}
```

## Recipe 3: a lamp (CSG-clipped `sdf_geometry roundcone` for the tapered shade)

No analytic primitive tapers from a wide base to a narrow top, and
`sdf_geometry`'s `roundcone` primitive on its own is the WRONG shape
for a lampshade -- `roundcone` is Quilez's rounded-CAPSULE-like cone:
both ends are hemispherical caps (radius `r1` at the wide end, `r2` at
the narrow end), so used bare it renders as a teardrop/balloon-on-a-
stick, not a lamp -- there is no flat rim at the bottom and no flat
disc at the top, which is exactly what a human eye needs to read
"lampshade" instead of "rounded blob".  The fix is a `csg_object`
INTERSECTION with a `box_geometry`: the box's flat faces slice off both
rounded caps, leaving only the straight tapered SIDE wall of the cone
with flat top and bottom cuts -- a genuine frustum silhouette.  This
was verified by rendering both the bare-roundcone version (confirmed:
reads as a teardrop/balloon from every angle) and the CSG-clipped
version (confirmed: reads as a lamp from two angles, see below) --
don't skip the CSG step and assume the bare primitive is "close enough".

**Two shapes this clip-the-caps reflex is NOT the answer for.**  A
lampshade is itself a solid of revolution, so `lathe_geometry` draws
the same frustum in four `profile_point` lines and no CSG; keep the
SDF-plus-clip below for a tapered form that must stay an SDF, and as
the general way to put a flat cut on one.  A rounded-cornered BOX or
cushion needs neither -- one `superellipsoid` part IS that shape.

`part` field layout: `<prim> <op> <k>  <pos xyz>  <euler xyz deg>
<scale xyz>  <a b c>  <round>`.  For `roundcone`, `a`/`b`/`c` are `<r1>
<r2> <h>` -- the primitive grows along LOCAL +Y from `y=0` (radius
`r1`, the WIDE base) to `y=h` (radius `r2`, the narrow top).  Size the
clipping box's `height` slightly less than the roundcone's `h` (e.g.
`0.5` box height against a `h=0.5` roundcone with rounded caps that
bulge past each end) so the intersection cuts INTO the rounded caps
rather than leaving a sliver of curvature at the rim -- the box's
`width`/`depth` just need to exceed `2*r1` so they don't clip the
tapered sides themselves, only the caps.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	2.6 1.6 3.2
	lookat		0 0.9 0
	up			0 1 0
	fov			42.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.45 0.45 0.45
}

uniformcolor_painter
{
	name	pnt_metal_dark
	color	0.12 0.12 0.14
}

uniformcolor_painter
{
	name	pnt_metal_light
	color	0.32 0.32 0.36
}

# Directional streaks read as brushed metal on the base and pole.
gabor3d_painter
{
	name			pnt_metal
	colora			pnt_metal_dark
	colorb			pnt_metal_light
	frequency		8.0
	bandwidth		1.5
	orientation		0 1 0
	impulse_density	4.0
	scale			3.0 3.0 3.0
}

uniformcolor_painter
{
	name	pnt_shade_pale
	color	0.94 0.9 0.78
}

uniformcolor_painter
{
	name	pnt_shade_warm
	color	0.82 0.72 0.5
}

# Warped noise reads as dyed-fabric mottle rather than a flat shade.
domainwarp3d_painter
{
	name			pnt_shade
	colora			pnt_shade_pale
	colorb			pnt_shade_warm
	octaves			3
	persistence		0.6
	warp_amplitude	2.5
	warp_levels		2
	scale			4.0 4.0 4.0
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

uniformcolor_painter
{
	name	pnt_metal_spec
	color	0.55 0.55 0.58
}

# SCALAR pipe: banded machining marks around the base and pole.
expression_function2d
{
	name	fn_metal_wear
	param	bands 10.0
	def		s sin( u * bands * tau )
	expr	smoothstep( -0.4, 0.4, s )
}

scalar_painter
{
	name		sp_metal_wear
	function2d	fn_metal_wear
	scale		0.12
	bias		0.03
}

ggx_material
{
	name		mat_metal
	rd			pnt_metal
	rs			pnt_metal_spec
	alphax		sp_metal_wear
	alphay		sp_metal_wear
}

lambertian_material
{
	name		mat_shade
	reflectance	pnt_shade
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.01 0
	orientation	-90 0 0
}

# Base: a squat, wide cylinder for stability at a glance.
cylinder_geometry
{
	name	base
	axis	y
	radius	0.3
	height	0.1
}

standard_object
{
	name		obj_base
	geometry	base
	material	mat_metal
	position	0 0.05 0
}

# Pole: a thin tall cylinder.
cylinder_geometry
{
	name	pole
	axis	y
	radius	0.04
	height	1.2
}

standard_object
{
	name		obj_pole
	geometry	pole
	material	mat_metal
	position	0 0.7 0
}

# Shade core: r1=0.35 (wide base) tapering to r2=0.15 (narrow top) over
# h=0.5 -- bare, this is a rounded-cap teardrop, NOT a lampshade.
sdf_geometry
{
	name	shade_taper
	part	roundcone union 0  0 0 0  0 0 0  1 1 1  0.35 0.15 0.5  0.0
}

standard_object
{
	name		obj_shade_taper
	geometry	shade_taper
}

# Clip box: flat faces slice off the roundcone's rounded caps top and
# bottom, leaving a straight-sided frustum.  width/depth (0.9) clear
# 2*r1 (0.7) so only the caps are cut, not the tapered sides; height
# (0.5) matches the roundcone's h so both caps get cut into.
box_geometry
{
	name	shade_clip
	width	0.9
	height	0.5
	depth	0.9
}

standard_object
{
	name		obj_shade_clip
	geometry	shade_clip
	position	0 0.25 0
}

csg_object
{
	name		obj_shade
	obja		obj_shade_taper
	objb		obj_shade_clip
	operation	intersection
	material	mat_shade
	position	0 1.3 0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Rendered at 256px from two angles (a 3/4 front view and a near-top-down
rear view), this reads as a lamp: a wide flat foot, a thin pole, and a
straight-sided conical shade with a visible flat disc at the top and a
flat rim at the bottom -- three stacked parts that individually are
unremarkable but compose into a recognizable object.  Confirm the
shade isn't floating: its `position.y` should be close to the pole's
top (`pole position.y + pole height/2`), and the taper's local `y=0`
(the WIDE end, pre-clip) should coincide with that meeting point.  If
the render still shows a rounded/bulging cap instead of a flat rim,
the clip box isn't cutting deep enough -- shrink its `height` (or grow
its Y `position` overlap into the roundcone's caps) until the
curvature is gone.

## Recipe 4: a turned vessel (`lathe_geometry` profile + `sweep_geometry` neck)

The lathe recipe from "Turned forms" above, built for real: a flask
whose whole body is one continuous profile, plus a curved neck that a
profile cannot express.  This is the shape family that a cylinder stack
ruins -- bottles, jars, retorts, mortars, vases, decanters.

Three things to notice:

1. **The `profile_point` list IS the silhouette**, in `<r> <h>` --
   radius from the axis, height along it -- read off the reference
   bottom-up: base rim `0.26 0`, belly `0.31 0.28`, shoulder
   `0.22 0.54`, neck-in `0.075 0.72`, rolled lip `0.10 0.99`.  Nothing
   is derived; you write down what you see.  Points interpolate
   STRAIGHT, so add one wherever the outline actually curves.
2. **It starts and ends at `r = 0`**, on the axis, so those two points
   collapse to poles and the vessel is closed and watertight with NO
   cap geometry -- and since the first two share `h = 0`, that opening
   horizontal segment is the flat base it sits on.
3. **The neck is a `sweep_geometry`, not part of the profile**, because
   it CURVES, which no surface of revolution can do.  Its cross-section
   is the eight-point circular polygon (`profile_point` here means
   `<x> <y>` on that polygon, NOT the lathe's `<r> <h>`; `profile_circle
   0.035 8` writes the same ring in one line), carried along a
   five-point Catmull-Rom path whose first point `0.16 0.62 0` sits
   INSIDE the body's surface at that height, so it reads as joined
   rather than floating alongside.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	1.1 0.85 1.9
	lookat		0.15 0.45 0
	up			0 1 0
	fov			42.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.45 0.45 0.45
}

uniformcolor_painter
{
	name	pnt_glass
	color	0.72 0.78 0.72
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_glass
	reflectance	pnt_glass
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.01 0
	orientation	-90 0 0
}

# The PROFILE: <r> <h> pairs read bottom-up off the reference, spun
# about the default y axis.  First and last points sit at r = 0 (on the
# axis), so the vessel closes at both ends with no caps; the first two
# share h = 0, so the base is a flat disc.
lathe_geometry
{
	name	flask_body
	profile_point	0 0
	profile_point	0.26 0
	profile_point	0.30 0.10
	profile_point	0.31 0.28
	profile_point	0.28 0.44
	profile_point	0.22 0.54
	profile_point	0.19 0.62
	profile_point	0.075 0.72
	profile_point	0.075 0.92
	profile_point	0.10 0.99
	profile_point	0.075 1.03
	profile_point	0 1.05
}

standard_object
{
	name		obj_flask_body
	geometry	flask_body
	material	mat_glass
}

# The NECK: a fixed circular cross-section swept along a curve -- the
# job sweep_geometry actually does.  profile_point is <x> <y> in the
# sweep frame (a closed polygon, CCW); point is a path control point.
sweep_geometry
{
	name	spout
	profile_point	0.035 0.0
	profile_point	0.0247 0.0247
	profile_point	0.0 0.035
	profile_point	-0.0247 0.0247
	profile_point	-0.035 0.0
	profile_point	-0.0247 -0.0247
	profile_point	0.0 -0.035
	profile_point	0.0247 -0.0247
	point	0.16 0.62 0
	point	0.36 0.68 0
	point	0.55 0.60 0
	point	0.68 0.40 0
	point	0.72 0.22 0
	n_len	64
	cap_start	TRUE
	cap_end		TRUE
}

standard_object
{
	name		obj_spout
	geometry	spout
	material	mat_glass
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Rendered at 128px this already reads as a turned vessel: the silhouette
runs from a flat base out through a full belly, tucks into a shoulder,
draws in to a slim neck and finishes on a rolled lip, with no visible
step anywhere along it -- and the swept neck arcs away and back down as
one continuous tube.  Compare that against the same silhouette authored
as five `cylinder_geometry` chunks, which produces a staircase.

If your version shows a visible ledge, two adjacent `profile_point`s
drop too much radius over too little height -- add a point between
them.  If the vessel sinks into the table, its first `profile_point`'s
`h` is not the base plane, or the profile never reaches `r = 0` there
and the base is open.  If the neck floats beside the body, its
first `point` is outside the body's radius at that height -- move it
inward until it is buried, and confirm with a render, not with
arithmetic.

**A one-call alternative to the body profile above.**
`insert_geometry_scaffold {family:"blended_vessel", name:"vessel1",
size:1.0, detail:0.5, aspect:1.0}` expands a base/belly/rim roundcone
`smin` chain plus a flat-bottom `box subtract` into ONE
`sdf_geometry` chunk (`tmpl_vessel1_vessel`) in a single call -- the
FALLBACK form, so it is the one to reach for when the vessel must also
take part in CSG; hand-write the `profile_point` list above otherwise --
`size` sets the base/belly radii, `aspect` elongates total height (a
squat bowl at low aspect, a tall vase at high), `detail` is smin blend
tightness (crisper joints as it rises toward 1, softer shoulders as it
falls toward 0).  Point a `standard_object.geometry` at
`tmpl_vessel1_vessel` exactly like `flask_body` above; the scaffold is
geometry-only, so a swept neck (or any other hand-authored addition)
still composes alongside it the same way Recipe 4's `spout` does, and
you still wire the material and `standard_object` yourself.

## Recipe 5: a compact standalone sweep (no vessel)

Recipe 4's sweep is scenario-glued to the flask's neck; this is the
generic form -- a closed profile tapered at both ends via
`end_scale_x`/`end_scale_y`, carried along a short 3-point path, with
no cross-object coordination required.  The square profile below uses
the `profile_rect 1.0 1.0` convenience (a hand-listed four-corner
`profile_point` form gives the identical shape -- see the vessel
recipes above); a round rail would be `profile_circle <r> [n]` instead.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	0 -9 4.5
	lookat		0 0 0.6
	up			0 0 1
	fov			42.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
}

uniformcolor_painter
{
	name	pnt_rail
	color	0.75 0.45 0.15
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_rail
	reflectance	pnt_rail
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 0 -0.6
}

sweep_geometry
{
	name			rail1geom
	profile_rect	1.0 1.0
	point			-3 0 0
	point			0 0 1.2
	point			3 0 0
	n_len			32
	end_scale_x		0.2
	end_scale_y		0.2
}

standard_object
{
	name		rail1
	geometry	rail1geom
	material	mat_rail
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

## Recipe 6: surface detail via `displaced_geometry` over a BUILDER's mesh

`displaced_geometry.base_geometry` is a plain `Reference` to any already-
declared geometry, so it composes over `sdf_geometry` (including a
`superellipsoid` part), `lathe_geometry`, `sweep_geometry` and
`skin_geometry`, not just the analytic primitives -- a fillip-free way to
get scales, bark or hammered-metal dimples onto a shape you already built
some other way, instead of hand-authoring the relief into the base.  Below:
a `superellipsoid` cushion (Recipe 3's SDF vocabulary) with `perlin2d_painter`
dimples -- the clean case, verified end to end (derives, bakes, renders).

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	0 -5 2.4
	lookat		0 0 0
	up			0 0 1
	fov			38.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 0 -1.1
}

uniformcolor_painter
{
	name	pnt_dark
	color	0 0 0
}

uniformcolor_painter
{
	name	pnt_light
	color	1 1 1
}

perlin2d_painter
{
	name		pnt_dimples
	colora		pnt_dark
	colorb		pnt_light
	persistence	0.5
	octaves		4
	scale		9.0 9.0
	shift		0 0
}

uniformcolor_painter
{
	name	pnt_pebble
	color	0.62 0.42 0.28
}

lambertian_material
{
	name		mat_pebble
	reflectance	pnt_pebble
}

sdf_geometry
{
	name	pebblebase
	part	superellipsoid union 0  0 0 0  0 0 0  1.15 0.85 1.0  1.0 0.45 0.45  0
}

displaced_geometry
{
	name			pebblegeom
	base_geometry	pebblebase
	detail			48
	displacement	pnt_dimples
	disp_scale		0.05
}

standard_object
{
	name		pebble
	geometry	pebblegeom
	material	mat_pebble
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 -0.7 0.7
}
```

Three things worth knowing before reaching for this on a different base,
verified by direct measurement rather than assumed from the `Reference`
plumbing:

- **`sdf_geometry` (including a `skeleton_geometry`-expanded body) stays
  SMOOTH under displacement** -- its own `TessellateToMesh` emits real
  shared-vertex topology (a dual-contoured surface mesh), so the
  post-displacement normal recompute genuinely averages across neighbouring
  faces.  This is the recipe above, and it is the safe default when the
  relief needs to read as smooth (scales, dimples, skin pores).
- **`lathe_geometry`/`sweep_geometry`/`skin_geometry` bases lose smooth
  shading the moment ANY non-zero displacement is applied.**  Their bake is
  already a plain triangle mesh, and the generic mesh-to-mesh tessellation
  path used to feed the displacer re-emits every triangle corner as its OWN
  vertex (no shared indices), so the post-displacement normal recompute has
  nothing to average across -- every triangle reads its own flat face
  normal.  The base looks fine un-displaced (its own construction-time
  normals are smooth); it goes visibly faceted -- NOT a subtle effect, hard
  per-triangle facets -- the instant `disp_scale` is non-zero.  If you need
  bark/wrinkle relief on a turned or swept body, raise that base's own
  tessellation (`n_radial`, `n_len`) until the facets are smaller than the
  desired texture wavelength, or accept the faceted look as part of the
  texture (works for something already angular; not for skin).
- **An INTERIOR-pinch `lathe_geometry` profile (an hourglass waisted to
  `r 0` partway along, not just at the two ends) TEARS at the pinch under
  displacement** -- the pinch bakes as two coincident vertices with opposite
  normals (`ProceduralDescriptors.h`'s `LatheDescriptor` documents this
  as deliberate, not a bug: welding them back into one vertex would put one
  band's shading normal in the wrong half-space instead).  Displacing one
  pushes the pair apart by `2 * disp_scale`, opening a visible crack.
  Displace an UNPINCHED profile, or keep `disp_scale` small relative to the
  waist.

A `skeleton_geometry`-expanded creature has no per-limb UV: every part
shares ONE cylindrical wrap around the whole body's own bounding box (`u` =
angle about local Y, `v` = normalized height), so a displacement pattern
sized for one limb will read at a different scale on another and can repeat
oddly across the seam -- scale the painter for the WHOLE silhouette, not one
limb, or keep the pattern small and high-frequency (like the dimples above)
so the per-limb scale difference does not read.

`path_instances_geometry` composes the other way: its template is realized
and tessellated through the same universal contract, so a `displaced_geometry`
can BE the template -- a row of dimpled pebbles along a path is one
`displaced_geometry` chunk plus the `path_instances_geometry` that stamps it,
not N hand-placed copies.

## Recipe 7: edge wear from `curv` -- bright metal where the form is convex

Any convenient geometry works; a rounded box makes the point cleanly
because its flat faces are exactly `curv == 0` and its filleted edges are
uniformly convex.  `clamp(curv, 0, 1)` alone (no noise needed for a crisp
trim look) picks out the edges; the SAME field bridges into a
`scalar_painter` for roughness so the exposed metal is also the polished
one -- see `materials-and-media-basics.md`'s patina section for the
concave twin (`clamp(-curv, 0, 1)`) and the full sign convention, and for
`occlusion(radius)`, the crevice-dirt companion signal that deepens grime
in a genuinely enclosed pocket a fine-scale curvature read underrates.
For a thin rim, ear, fin, or shell that should glow or tint differently
where the form is thinnest, reach for `thickness(radius)` instead of
either -- see `materials-and-media-basics.md`'s "Glow that dies in thick
walls" section for the worked idiom (a varying-thickness SDF shell + a
`thin`-driven `translucent_material`), which is the fix for the
single most-repeated agent failure of faking translucency with a
painted emissive gradient on an opaque shell.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	1.5 1.2 1.7
	lookat		0 0 0
	up			0 1 0
	fov			38.0
}

expression_painter
{
	name		pnt_edge_field
	param		edge_gain 3.0 min 0.5 max 12 step 0.5 label "Edge sensitivity"
	expr		clamp(curv * edge_gain, 0, 1)
}

uniformcolor_painter
{
	name	pnt_paint
	color	0.55 0.08 0.06
}

uniformcolor_painter
{
	name	pnt_bare_metal
	color	0.85 0.85 0.82
}

blend_painter
{
	name	pnt_edge_color
	colora	pnt_bare_metal
	colorb	pnt_paint
	mask	pnt_edge_field
}

scalar_painter
{
	name		sp_edge_rough
	painter		pnt_edge_field
	channel		R
	scale		-0.55
	bias		0.60
}

ggx_material
{
	name		mat_edge
	rd			pnt_edge_color
	rs			pnt_bare_metal
	alphax		sp_edge_rough
	alphay		sp_edge_rough
	ior			1.4
	extinction	2.2
}

sdf_geometry
{
	name	block
	part	roundbox union 0   0 0 0   0 0 0   1 1 1   0.5 0.5 0.5   0.08
}

standard_object
{
	name		obj_block
	geometry	block
	material	mat_edge
	position	0 0 0
}

directional_light
{
	name		key
	power		3.2
	color		1 0.98 0.95
	direction	0.4 0.6 0.7
}
```

## Pooled water on large terrain -- a geometry problem, not a shading mask

`occlusion(r)`'s radius is a fraction of the HIT GEOMETRY's OWN bbox
diagonal (Recipe 7's companion signal, and
`materials-and-media-basics.md`'s wetness/patina recipes both key off
it).  On a single large ground mesh that means a shading-level pooling
mask can only ever resolve basin-scale hollows -- a 40 m terrain's
`occlusion(0.06)` query is a couple of metres wide, so a puddle-sized
cavity is below its resolution no matter what radius you pick.  Small
pools on one big terrain mesh are a geometry problem, not a bigger
radius: author the pool as its OWN object.

**The recipe:** a `dielectric_material` (`ior 1.33`, `scattering
1000000` for a delta-flat surface, `tau` per the water-tint idiom in
`materials-and-media-basics.md`), on one of:

- a squashed `ellipsoid_geometry` for a simple round pool --
  `tidepools.RISEscene`'s idiom;
- a `displaced_geometry` over a flat `box_geometry` or
  `cartesian_disk_geometry`, driven by a `gerstnerwave_painter` or an
  `IFunction2D`, for a rippled surface --
  `vcm_sdf_luminaire_jellyfish.RISEscene`'s idiom;
- a `csg_object` intersection of the terrain with a half-space, to
  carve a level cap that follows an irregular basin.

Reach for this whenever the terrain is one large mesh, not only when
the water must be level -- it is also what a genuinely raised, level
pool of any size needs, since a shading-level mask never lies flat, it
only follows the host surface's own normals (fine at cobblestone scale,
wrong at pond scale).  A geometry-level pool is a real dielectric
surface and does add light-transport chains a shading-level coat does
not; a scene whose hero effect is light focused through the pool wants
VCM, per the integrator map (PT/BDPT miss most caustic energy).

> **Trap, flag it loudly: heightfield-mode `sdf_geometry` cannot drive
> ANY occlusion-based pooling mask.**  `occlusion()` and `thickness()`
> return their neutral fallback in heightfield mode (a heightfield's
> global Lipschitz bound would make a local answer systematically
> wrong).  So the natural instinct -- build a terrain as an SDF
> heightfield, then pool water in its hollows with `occlusion()` --
> silently produces a uniformly DRY terrain: neutral occlusion is 1
> (unoccluded) everywhere, there is no error and no warning, the mask
> simply never lights up.  Terrain that must drive occlusion-based
> puddles has to be a MESH, a `displaced_geometry` (which bakes to an
> indexed mesh), or a PART-BASED `sdf_geometry` -- never heightfield
> mode.

## Traps specific to object modeling

1. **`torus_geometry`'s ring axis is always Y** -- there is no `axis`
   parameter like `cylinder_geometry` has.  To stand a torus up (a
   mug handle, a ring on its edge), rotate it with `orientation`;
   90 about X or Z both work depending which way you want the ring
   facing.  Don't over-rotate by pattern-matching the mug recipe: a
   FLAT collar/ring lying in XZ (a pawn's collar, a disk-shaped ring
   resting on a surface) wants the DEFAULT orientation with no rotation
   at all; it's only a STANDING handle (the mug recipe's ring-on-its-
   edge) that needs the 90-degree rotate.
2. **`sdf_geometry`'s first `part` line must be `union` or `smin`** --
   the field starts empty, so `subtract`/`intersect` as the first
   operation leaves nothing to subtract from/intersect with.  This is
   a HARD parse-time failure, not a silent bad render: `ParsePartLines`
   rejects it with an explicit `eLog_Error` naming the offending part
   line, and the chunk refuses to derive.
3. **Reuse geometry across objects instead of redeclaring it** -- a
   table's four legs, a fence's posts, a railing's balusters are all
   ONE geometry chunk referenced by N `standard_object`s at N
   positions.  Redeclaring identical geometry per instance wastes
   authoring effort and drifts if you tweak one copy and not the
   others.
4. **CSG and SDF both cost more per-ray than a single analytic
   primitive** -- `csg_object` tests both operands, `sdf_geometry`
   sphere-traces up to `maxsteps` steps per ray.  Reach for them when
   the shape genuinely needs a boolean or a smooth blend/taper;  don't
   wrap a shape in CSG/SDF that a single primitive already produces.
5. **Blockout accuracy compounds** -- a proportion error caught at the
   cheap-primitive blockout stage is a one-line `position`/`radius`
   edit; the same error caught after wiring in the CSG hollow or the
   SDF taper means re-deriving a costlier chunk.  Confirm proportions
   from 2-3 angles BEFORE the refine pass, every time.
6. **A superellipsoid exponent near 0.5-0.6 renders as a rounded CUBE,
   not an organic mass.**  A cat body built from `superellipsoid` parts
   at exponents in that range read as blocky, melted-looking lumps once
   actually rendered, even though the parts composed correctly -- the
   shape itself was the wrong primitive for the intent.  Organic
   masses (a torso, a head, a limb) want exponents CLOSE TO 1.0 (a true
   ellipsoid); reserve 0.4-0.7 for a literal cushion/pillow, and render
   before committing to a low exponent on anything meant to read as
   flesh.
7. **A sub-feature has to be proportioned to what it attaches to, or no
   blend width will make it read.**  An ear, a horn, a fingertip
   authored at less than 1/5 the characteristic dimension of the mass
   it joins is invisible at ordinary framing no matter how tight the
   `smin` k is drawn -- that 1/5 figure is not a rule of thumb, it is
   the exact ratio `fix_blend_scale`'s own proportion caveat gates on,
   so a sub-feature under that line will be flagged with "narrowing k
   will not make this part readable" even after the verb clamps its
   blend.  Author the sub-feature at 1/5 or more of what it joins
   first, THEN use `smin` k to blend the seam -- not the other way
   around.
