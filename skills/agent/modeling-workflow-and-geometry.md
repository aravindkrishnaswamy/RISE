# Modeling Workflow and Geometry
> hook: Read when composing or iterating on scene GEOMETRY -- placing objects, choosing primitive types, or checking placement cheaply from multiple angles.

## The observe loop (cheap iteration, not full-size renders)

Modeling is: insert/edit ONE thing, then look, then adjust.  Looking
is cheap if you ask for a small preview instead of the final image.

1. **Insert or edit one chunk.**  `insert_chunk` for a new object/
   geometry/material; `propose_patch` for a single parameter tweak.
2. **`validate`** first if you are not sure the edit is well-formed
   (bad references, wrong param shape) -- cheaper than a failed render.
3. **`render {width:160, height:120}`** from the default camera.  The
   result's `meanR/meanG/meanB` alone often confirm placement sanity
   (e.g. "did the floor stop being solid black") without looking at a
   pixel.
4. **`render` again with a `camera` override from 1-2 MORE angles.**
   A single view hides depth -- an object can be behind, inside, or
   floating above another and look identical from the original
   camera.  To orbit: keep `lookat` fixed on the subject and move
   `location` to a different point around it, e.g. swap X and Z to
   look from the side, or raise Y to look down from above.  Both
   `location` and `lookat` are strings of EXACTLY 3 finite numbers
   `"x y z"` (space-separated) -- wrong token count or a non-numeric
   component is rejected.  `fov` is optional and defaults to the
   camera's current value; only set it (exclusive range `(0, 180)`)
   when you need wider/narrower framing to fit the subject.  The
   override is EPHEMERAL -- restored after that one render, never
   touches the document.
5. **`read_image {maxEdge:160}`** only when you actually need to SEE
   the frame (composition, silhouette, color relationships) -- the
   render result's channel means often already answer "did this
   change" or "is this still black/blown out".
6. **Full-size, full-sample render with no width/height/camera
   override** ONLY for final verification once you are confident the
   placement is right.

**Why this matters (measured):** a `maxEdge 128` preview PNG is about
**13x smaller** than a full 800^2 frame; `maxEdge 192` is about **8x**
smaller.  A blocking-out pass that renders from 3 angles at 160x120
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
	position	0 0.6 1.2
	# On top of the pedestal in Y, but pushed off to the side in Z so
	# a second camera angle (see the observe loop) can tell the two
	# apart instead of one occluding the other from every direction.
}

# Cylinder: axis "y" stands it upright (default axis is "x", which
# would lay it on its side).
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
	position	-1.4 0.5 -0.6
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
2. **Preview `width`/`height` must be paired.**  Passing one without
   the other is rejected; both are clamped to `[16, 512]` and never
   touch the document (they are restored after the render).
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
