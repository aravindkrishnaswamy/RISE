# Modeling Workflow and Geometry
> hook: Read when composing or iterating on scene GEOMETRY -- placing objects, choosing primitive types, or checking placement cheaply from multiple angles.

## The observe loop (build in coherent groups, then look)

Modeling is: build a coherent GROUP of related chunks, then look.
Batch the group so it costs one round-trip, and render often enough
that a mistake is one edit to fix instead of thirty edits buried.  A
render is cheap -- a single small call -- and it is how you catch the
floor you forgot, the light that is off, the hero placed off-frame,
while they are still easy to fix.

**Batch related chunks with `insert_chunks`.**  A painter, its
material, a geometry, and the object that binds them are one coherent
unit -- add them in a single `insert_chunks` call, in dependency order,
not four separate `insert_chunk` calls.  Same for a whole lighting rig
or a piece of furniture.  That is one round-trip instead of many, and
it means you render once you have a whole THING to look at rather than
after each fragment.  Batching is where you SAVE calls; the renders
between groups are cheap and you should keep them.

**Batch related edits with `propose_patches`.**  The same rule applies
to CHANGING things, and this is where the calls really add up: a
correction pass after a render is usually "nudge these six objects and
retune two lights", which is a dozen or more parameter edits.  Send
them as ONE `propose_patches` call, not a dozen `propose_patch` calls
turn by turn -- an agent that patches one parameter per round will burn
its entire call budget on a single correction pass and never reach a
finished scene.  Reach for single `propose_patch` only when you are
genuinely changing ONE thing.

Two things to know about the batch form.  Elements are applied in array
order and it is BEST-EFFORT -- one rejected element does not stop the
rest -- so **read the `applied`/`total` it returns**: `8/12 applied`
means four edits did not land, and `results[i]` tells you which and
why.  Do not assume a batch succeeded because the call returned.  The
one exception to best-effort: if the `baseHeadVersion` you passed is
stale, the WHOLE batch stops with nothing applied (so it cannot
half-clobber an edit someone else made) -- re-read the document and
resubmit.

**Render after each coherent group, and always after lighting.**  Once
a group has landed -- the surface, a cluster of furniture, the light
rig -- render a small preview and check it before you build the next
group on top.  In particular:
- **the LIGHTING render is the one you must not skip.**  Once your
  lights are in place -- and again whenever you change a light or a
  material -- render and actually CHECK the lit result.  Is it too dark,
  too bright, or crushed to a single colour (a warm key with no fill
  reads as an orange blob)?  Brightness and colour balance are visual
  facts you cannot read off the chunk text, and getting the light wrong
  is the single most common way a build fails -- a scene that is black,
  blown out, or mono-coloured fails no matter how good the geometry is,
  and you will not know until you look;
- render the first time there is a lit surface to SEE -- is the scene
  not black, is the hero in frame, does the floor exist;
- render to settle a spatial relationship a single mental model cannot
  -- is that object behind, inside, or floating above another; does the
  hero actually rest ON the table (a second camera angle is what catches
  depth ambiguity);
- render before you build a LOT more on top of something unverified --
  check the foundation before thirty chunks bury it.

The only render not worth making is one that tells you nothing you
already know: re-rendering when nothing visual changed since the last
look, or rendering to confirm a binding that `read_document` or the
insert's own `issues` diagnostics already answered.  Short of that,
when in doubt, look -- the render is cheap and the failure it catches
is not.

**Do not fly blind.**  The failure that actually loses builds is the
opposite of over-rendering: assembling an entire scene and never
rendering once, then discovering at the end that the key light was off
or the hero off-frame, with no budget left to fix it.  If you have
built a substantial scene and have not looked, render now.

**How to render cheaply, when you do render:**
- **`render {width:160, height:120}`** from the default camera -- the
  `meanR/meanG/meanB` alone often answer "is it lit / still black /
  blown out" without reading a single pixel.
- **`render` with a `camera` override from another angle** to resolve
  depth -- keep `lookat` on the subject and move `location` (swap X and
  Z for a side view, raise Y for top-down).  `location`/`lookat` are
  `"x y z"` strings of exactly 3 finite numbers; `fov` is optional in
  the open range `(0, 180)`.  The override is EPHEMERAL -- restored
  after that one render, never touches the document.
- **`read_image {maxEdge:160}`** only when you actually need to SEE the
  frame (composition, silhouette, colour) -- the channel means usually
  already answered "did this change".
- **`validate`** before a render if you are unsure an edit is
  well-formed -- cheaper than a failed render.
- **Full-size, full-sample render** (no width/height/camera override)
  ONLY for final verification, once you are confident.

**Why this matters (measured):** a `maxEdge 128`-`192` preview PNG is
roughly an order of magnitude smaller than a full 800^2 frame --
**8-17x**, depending on content (PNG size varies with image
complexity, so don't expect an exact multiplier to reproduce across
scenes).  A blocking-out pass that renders from 3 angles at 160x120
and reads back one `maxEdge 160` image costs a small fraction of the
tokens (and render time) of doing the same pass at full resolution
each step.  Reserve full resolution for the shot you are actually
going to keep.

## Geometry-type selection

Survey of every geometry/object chunk kind actually registered in the
parser (`src/Library/Parsers/ChunkParserRegistry.cpp`) -- do not
invent kinds not listed here.

- **Analytic primitives** -- `sphere_geometry` (`radius`),
  `box_geometry` (`width`/`height`/`depth`, axis-aligned),
  `cylinder_geometry` (`axis` x/y/z, `radius`, `height`),
  `torus_geometry` (`majorradius`, `minorratio` -- minor radius as a
  FRACTION of major), `ellipsoid_geometry` (`radii`, per-axis
  semi-axes).  Exact silhouettes, no tessellation, cheapest to
  intersect.  Prefer these for blocking out a scene and for anything
  that genuinely IS that primitive shape (a ball, a crate, a pipe,
  a ring).
- **Planes** -- `infiniteplane_geometry` (tiling, unbounded: floors,
  walls, backdrops -- see the lighting/materials skills, a specular
  object needs one of these behind it or it renders black) and
  `clippedplane_geometry` (four explicit corners `pta/ptb/ptc/ptd`:
  a bounded quad -- area-light emitters, framed backdrops, cropped
  floors).  `clippedplane_geometry`'s corner WINDING picks which side
  renders/emits, same rule as the emissive-quad trap in the lighting
  skill.
- **`csg_object`** -- boolean combination of two already-declared
  objects (`obja`, `objb`) via `operation` (`union` / `intersection` /
  `subtraction`).  Size the result by sizing the CHILD geometries
  before combining -- `csg_object` itself takes only `position` and
  `orientation`, **no `scale` parameter** (a scale edit on a
  `csg_object` is refused).  Good for cut/drilled/intersected shapes
  (a box with a cylindrical bore, a lens from two overlapping
  spheres).
- **Triangle meshes** -- `3dsmesh_geometry`, `rawmesh_geometry` /
  `rawmesh2_geometry`, `risemesh_geometry`, `plymesh_geometry`,
  `gltfmesh_geometry`, each taking a `file` pointing at the source
  asset.  Use for imported/authored assets that are not primitive-
  shaped.  Declare the geometry chunk before the `standard_object`
  that references it, same declare-before-use rule as painters and
  materials.
- **Other real kinds worth knowing about**: `circulardisk_geometry`,
  `bezierpatch_geometry`, `bilinearpatch_geometry` (patch/surface
  primitives), `sdf_geometry` (signed-distance-field geometry),
  `cartesian_disk_geometry`, `sweep_geometry`, `path_instances_geometry`
  (instancing along a path), and `displaced_geometry` (tessellates a
  `base_geometry` and offsets vertices by a `displacement` painter --
  a real modeling tool for bumpy/organic surfaces, but if you use it:
  prefer FEWER bumps with LONGER wavelengths over many small bumps,
  matching the displacement lesson in the SMS docs -- finer
  tessellation (`detail`) does not fix a too-busy displacement and
  can make downstream specular/caustic solving worse).  These are
  real chunks but more specialized -- reach for the analytic
  primitives and meshes above first.

## Placement and scale sanity

- Scale the OBJECT to the SCENE, not to some absolute unit -- a
  `radius 1.0` sphere and a camera at `location 0 0 5` fills a
  comfortable fraction of a 40-degree-fov frame; the same sphere at
  `location 0 0 50` is a speck.  When in doubt, keep geometry sizes
  near 1 and move the CAMERA to frame it, then adjust.
- `lookat` should point at the subject's approximate center, not at
  the world origin if the subject is not there -- an off-target
  `lookat` silently crops or empties the frame.
- **When the camera is GIVEN and you cannot change it** (a build-from-
  scaffold task hands you a fixed `pinhole_camera`, and the rasterizer/
  camera chunks are often not agent-editable), read where it already
  looks -- typically `lookat 0 0 0` -- and place your HERO object THERE,
  at the focal point, sized to fill a comfortable fraction of the frame.
  Put the surface it rests on just below it and the supporting/decor
  geometry AROUND and BEHIND it. Do not build the hero off to one side
  and hope: the frame is fixed, so an off-origin hero renders as empty
  space where the subject should be. If "the camera focuses on X" is in
  the brief, X belongs at the point the given camera is aimed at.
- A single render angle cannot tell you whether an object is in front
  of, behind, or intersecting another -- it can only tell you the
  silhouette agrees.  The second-angle render in the observe loop
  above is what actually catches depth placement bugs; do not skip it
  for any placement you are not already sure of.

## Complete example 1: blocking out three primitives on a floor

A box pedestal, a sphere, and a cylinder on a `clippedplane` floor,
lit by one directional key -- the primitive-palette starter for
blocking out a composition.

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
	location	3 3 6
	lookat		0 0.5 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.55 0.55 0.55
}

uniformcolor_painter
{
	name	pnt_box
	color	0.7 0.3 0.2
}

uniformcolor_painter
{
	name	pnt_sphere
	color	0.2 0.5 0.7
}

uniformcolor_painter
{
	name	pnt_cyl
	color	0.3 0.7 0.3
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_box
	reflectance	pnt_box
}

lambertian_material
{
	name		mat_sphere
	reflectance	pnt_sphere
}

lambertian_material
{
	name		mat_cyl
	reflectance	pnt_cyl
}

# The floor: a bounded quad big enough to hold the three primitives.
clippedplane_geometry
{
	name	floor
	pta		-4 0 -4
	ptb		4 0 -4
	ptc		4 0 4
	ptd		-4 0 4
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
}

# Pedestal: a box, sized directly via width/height/depth.
box_geometry
{
	name	pedestal
	width	1.2
	height	0.6
	depth	1.2
}

standard_object
{
	name		obj_pedestal
	geometry	pedestal
	material	mat_box
	position	0 0.3 0
}

sphere_geometry
{
	name	sph
	radius	0.5
}

standard_object
{
	name		obj_sphere
	geometry	sph
	material	mat_sphere
	position	0 1.1 0
	# Genuinely ON TOP of the pedestal: the pedestal top is at y=0.6
	# (position.y 0.3 + half-height 0.3), so the sphere's center at
	# y=1.1 rests its radius-0.5 bottom exactly on that surface, same
	# X/Z as the pedestal underneath it.
}

# Cylinder: axis "y" stands it upright (default axis is "x", which
# would lay it on its side).  Placed well clear of the box's footprint
# on the floor -- from the authored camera it can look close to the
# pedestal group in silhouette, but the observe loop's second angle
# (swap X and Z, e.g. location "6 3 3") shows all three primitives as
# genuinely separate objects instead of one occluding another.
cylinder_geometry
{
	name	cyl
	axis	y
	radius	0.35
	height	1.0
}

standard_object
{
	name		obj_cyl
	geometry	cyl
	material	mat_cyl
	position	1.3 0.5 1.6
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

## Complete example 2: `csg_object` -- a box with a cylindrical bore

A box with a cylinder subtracted out of its center, demonstrating
boolean modeling.  Both operand geometries are declared and given
their own (unused-in-render) placeholder objects first -- `csg_object`
consumes them by NAME, and the position/orientation-only caveat
applies to the CSG result, not to its operands.

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
	location	2.5 2 4
	lookat		0 0 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_backdrop
	color	0.5 0.5 0.55
}

uniformcolor_painter
{
	name	pnt_block
	color	0.75 0.6 0.3
}

lambertian_material
{
	name		mat_backdrop
	reflectance	pnt_backdrop
}

lambertian_material
{
	name		mat_block
	reflectance	pnt_block
}

# Backdrop wall so the render isn't a bore hole floating in a void.
infiniteplane_geometry
{
	name	wall
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_wall
	geometry	wall
	material	mat_backdrop
	position	0 0 -2
}

# Operand A: the block. Size it here -- csg_object itself has no
# scale parameter, so the FINAL size is whatever these operand
# geometries are built at.
box_geometry
{
	name	block
	width	1.6
	height	1.6
	depth	1.6
}

standard_object
{
	name		obj_block
	geometry	block
	material	mat_block
}

# Operand B: the bore, a cylinder wider than the block along its
# axis so the subtraction cleanly punches all the way through.
cylinder_geometry
{
	name	bore
	axis	y
	radius	0.5
	height	2.2
}

standard_object
{
	name		obj_bore
	geometry	bore
	material	mat_block
}

# The CSG result: subtract the bore from the block.  Only position/
# orientation are transform-routable here -- a `scale` line would be
# refused; resize by editing `block`/`bore` above instead.
csg_object
{
	name		obj_drilled
	obja		obj_block
	objb		obj_bore
	operation	subtraction
	material	mat_block
	position	0 0 0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.8
}
```

## Traps

1. **Camera vectors are exactly 3 numbers.** `location`/`lookat`/`up`
   in a `render` camera override are each a single string of EXACTLY
   3 finite numbers `"x y z"` -- extra/missing tokens or a non-numeric
   component is rejected.  `fov` is a plain number in the exclusive
   range `(0, 180)`.
2. **Preview `width`/`height` must be paired, or NEITHER takes effect.**
   Passing only one is not rejected -- it is silently IGNORED, and the
   render proceeds at the scene's AUTHORED dimensions (there is no
   sane way to guess the other dimension, so the default is "do not
   override" rather than invent an aspect ratio).  A stray one-sided
   `render {width:160}` on a scene authored at 800x800 quietly costs a
   full-size render instead of the cheap preview you meant to ask for.
   Always pass BOTH, and confirm the override actually took by reading
   `previewWidth`/`previewHeight` back from the result -- if they don't
   match what you asked for, the pairing was broken.  When both ARE
   paired, they are clamped to `[16, 512]` and never touch the document
   (restored after the render).
3. **`csg_object` refuses `scale`.**  Position/orientation are
   transform-routable on a CSG result; size the operand geometries
   (`obja`/`objb`) BEFORE combining them.
4. **Declare before use, everywhere.**  A geometry/material/painter
   referenced by an object, and the operand objects referenced by a
   `csg_object`, must appear EARLIER in the file than their consumer
   -- same rule as painters-before-materials in the scene-skeleton
   skill.
5. **`insert_chunk`/`remove_chunk` are one-way doors per call** --
   each inserts or removes exactly ONE complete chunk and validates
   before applying; there is no multi-chunk batch and no undo verb,
   so prefer the observe loop (small renders between edits) over
   inserting several unverified chunks in a row.
6. **`cylinder_geometry`'s default axis is `x`.**  A cylinder with no
   `axis` line lies on its side; use `axis y` to stand it upright
   under a Y-up camera.
