# Object Modeling Recipes
> hook: Read when building an actual OBJECT (a mug, a table, a lamp) rather than blocking out a generic scene -- the geometry vocabulary, per-type gotchas, and CSG/SDF composition recipes.

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
| `infiniteplane_geometry` | floors, walls, backdrops | cheap analytic, unbounded | needs a `position`/`orientation` to place and tilt -- default lies in XY facing +Z |
| `clippedplane_geometry` | bounded floors, area-light quads, framed backdrops | cheap analytic | four explicit corners; vertex WINDING picks which side renders/emits |
| `csg_object` | booleans of two already-declared objects | cost of both operands + one more test | **no `scale` parameter** -- size the operands, not the CSG result |
| `sdf_geometry` | melded/filleted/tapered organic shapes (fillets, cones, capsules, smooth unions) that no analytic primitive covers | sphere-traced -- more expensive per-hit than an analytic primitive, cost scales with `maxsteps` | inline `part` lines compose in order; the FIRST part must be `union` or `smin` (the field starts empty); see the lamp recipe below for the field layout |
| `sweep_geometry` | tubes, rails, mouldings, cable runs | mesh cost (tessellated once) | OPEN paths only -- a closed loop needs `torus_geometry`, not a sweep with matching endpoints |
| `path_instances_geometry` | fence posts, rivets, beads, chain links along a path | one tessellation + N cheap instances | template +Y aligns with the path tangent -- orient the template accordingly before instancing |
| `displaced_geometry` | bumpy/organic surfaces (a `base_geometry` tessellated + offset by a painter) | tessellation + per-vertex offset | prefer FEWER bumps with LONGER wavelengths -- finer `detail` does not fix a too-busy displacement (SMS docs lesson) |
| `circulardisk_geometry`, `cartesian_disk_geometry` | flat disks (dials, coins, disk-shaped bases) | cheap | `cartesian_disk_geometry` has uniform Cartesian UV density; the polar disk does not -- pick by what you're displacing/texturing onto it |
| `bezierpatch_geometry`, `bilinearpatch_geometry` | authored curved/patch surfaces | analytic (bezier) / cheap (bilinear) | `bezierpatch_geometry`'s old tessellation params (`detail`, `cache_size`, ...) are retired -- wrap it in `displaced_geometry` if you need that control |
| Mesh imports (`3dsmesh_geometry`, `rawmesh_geometry`/`rawmesh2_geometry`, `risemesh_geometry`, `plymesh_geometry`, `gltfmesh_geometry`) | authored/imported assets that are not primitive-shaped | mesh cost | `file` path resolved via the media-path search (see the reference-image skill for the exact resolution order); declare before the `standard_object` that references it |

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
5. **`read_image`** once you actually need to SEE the frame (silhouette,
   proportion relationships) rather than just check the render's
   channel means.

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
	name	pnt_mug
	color	0.85 0.85 0.9
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

## Recipe 2: a table (primitive reuse, one geometry instanced four times)

Declare the leg geometry ONCE and reference it from four
`standard_object`s at four positions -- geometry is a reusable
template, not copied per placement.  This is the cheapest way to build
anything with repeated parts (chair legs, railings, a picket fence)
before reaching for `path_instances_geometry`.

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
	name	pnt_wood
	color	0.55 0.35 0.18
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_wood
	reflectance	pnt_wood
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

## Recipe 3: a lamp (`sdf_geometry roundcone` for the tapered shade)

No analytic primitive here tapers from a wide base to a narrow top --
`sdf_geometry`'s `roundcone` primitive does exactly that, stacked over
a cylinder base and pole.  `part` field layout: `<prim> <op> <k>  <pos
xyz>  <euler xyz deg>  <scale xyz>  <a b c>  <round>`.  For
`roundcone`, `a`/`b`/`c` are `<r1> <r2> <h>` -- the primitive grows
along LOCAL +Y from `y=0` (radius `r1`, the WIDE base) to `y=h` (radius
`r2`, the narrow top), so no extra orientation flip is needed to sit
it wide-end-down on the pole.

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
	name	pnt_metal
	color	0.2 0.2 0.22
}

uniformcolor_painter
{
	name	pnt_shade
	color	0.9 0.85 0.7
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_metal
	reflectance	pnt_metal
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

# Shade: r1=0.35 (wide base) tapering to r2=0.15 (narrow top) over
# h=0.4 -- the lampshade silhouette no analytic primitive gives directly.
sdf_geometry
{
	name	shade
	part	roundcone union 0  0 0 0  0 0 0  1 1 1  0.35 0.15 0.4  0.0
}

standard_object
{
	name		obj_shade
	geometry	shade
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

Rendered, this reads as a lamp: a wide flat foot, a thin pole, and a
rounded tapered shade sitting on top -- three stacked primitives that
individually are unremarkable but compose into a recognizable object.
Confirm the shade isn't floating: its `position.y` should be close to
the pole's top (`pole position.y + pole height/2`), and its local
`y=0` (the WIDE end) should coincide with that meeting point given
`roundcone` grows in +Y from there.

## Traps specific to object modeling

1. **`torus_geometry`'s ring axis is always Y** -- there is no `axis`
   parameter like `cylinder_geometry` has.  To stand a torus up (a
   mug handle, a ring on its edge), rotate it with `orientation`;
   90 about X or Z both work depending which way you want the ring
   facing.
2. **`sdf_geometry`'s first `part` line must be `union` or `smin`** --
   the field starts empty, so `subtract`/`intersect` as the first
   operation leaves nothing to subtract from/intersect with (parses,
   derives, but sphere-traces an empty/degenerate field).
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
