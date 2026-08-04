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
| `infiniteplane_geometry` | floors, walls, backdrops | cheap analytic, unbounded | the CHUNK itself only takes `name`/`xtile`/`ytile` -- placement/tilt comes from the enclosing `standard_object`'s `position`/`orientation` (universal for every geometry chunk); default lies in XY facing +Z before that transform |
| `clippedplane_geometry` | bounded floors, area-light quads, framed backdrops | cheap analytic | four explicit corners; vertex WINDING picks which side renders/emits |
| `csg_object` | booleans of two already-declared objects | cost of both operands + one more test | **no `scale` parameter** -- size the operands, not the CSG result |
| `sdf_geometry` | **TURNED/LATHE PROFILES (bottles, jars, vases, mortars, goblets -- see "Turned forms" below)**; melded/filleted/tapered organic shapes (fillets, cones, capsules, smooth unions) that no analytic primitive covers | sphere-traced -- more expensive per-hit than an analytic primitive, cost scales with `maxsteps` | inline `part` lines compose in order; the FIRST part must be `union` or `smin` (the field starts empty); see the lamp and turned-vessel recipes below for the field layout |
| `sweep_geometry` | tubes, rails, mouldings, cable runs, and any TUBE THAT FOLLOWS A CURVE (a retort's neck, a spout, a handle, a bail) | mesh cost (tessellated once) | it sweeps a FIXED cross-section along a path -- it is NOT a lathe (see "Turned forms" below); OPEN paths only -- a closed loop needs `torus_geometry`, not a sweep with matching endpoints |
| `path_instances_geometry` | fence posts, rivets, beads, chain links along a path | one tessellation + N cheap instances | template +Y aligns with the path tangent -- orient the template accordingly before instancing |
| `displaced_geometry` | bumpy/organic surfaces (a `base_geometry` tessellated + offset by a painter) | tessellation + per-vertex offset | prefer FEWER bumps with LONGER wavelengths -- finer `detail` does not fix a too-busy displacement (SMS docs lesson) |
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

**The verb is `sdf_geometry` with `roundcone` parts joined by `smin`.**
`roundcone` IS a profile segment: `<r1> <r2> <h>` is a frustum that runs
along local +Y from radius `r1` at `y=0` to radius `r2` at `y=h`, i.e.
exactly one (height, radius) span of your profile.  Chain them
end-to-end -- each part's `y` position is the previous part's top -- and
join them with `smin <k>` instead of `union`.  `k` is the blend radius
in world units, and it is the whole point: `smin` fillets the joint
between two segments into a continuous curve, so the silhouette flows
where a hard `union` (or a cylinder stack) would step.  Pick `k` around
a third of the local radius; larger `k` = softer shoulder.

Reading a profile off a reference is mechanical.  Write down (height,
radius) pairs from the bottom up -- base, belly, shoulder, neck, lip --
then emit one `roundcone` part per span, and a `torus` part for a rolled
lip or a raised collar.  Recipe 4 below does exactly this and renders.

Two things the profile approach needs:

- **A flat bottom needs a cut.**  `roundcone`'s `y=0` end carries a
  hemispherical cap of radius `r1`, so the bottom-most segment bulges
  below its own origin and a vessel authored naively sinks through the
  table.  End the part list with `part box subtract 0` positioned so the
  box's top face sits at the intended base plane -- one line, and the
  bottom is flat.  (A round-bottomed florence flask genuinely wants the
  cap; leave it in that one case.)
- **`sweep_geometry` is NOT the lathe verb.**  It sweeps a FIXED profile
  polygon along a path, and its only per-station control, `point_width`,
  scales the profile's x-axis alone (`end_scale_y` on the other axis is
  linear-only) -- so a varying-radius sweep goes ELLIPTICAL, not round.
  What `sweep_geometry` is genuinely the right verb for is a TUBE THAT
  FOLLOWS A CURVE at roughly constant bore: a retort's curved neck, a
  spout, a handle, a bail, a cable.  A retort is therefore both verbs --
  an SDF profile for the bulb, a sweep for the neck.  Recipe 4 shows the
  pair.

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

## Recipe 4: a turned vessel (`sdf_geometry` profile + `sweep_geometry` neck)

The lathe recipe from "Turned forms" above, built for real: a flask
whose whole body is one continuous profile, plus a curved neck that a
profile cannot express.  This is the shape family that a cylinder stack
ruins -- bottles, jars, retorts, mortars, vases, decanters.

**The profile, read bottom-up as (height, radius) pairs:** base
`(0.02, 0.26)` -> belly `(0.28, 0.31)` -> shoulder `(0.54, 0.22)` ->
neck-in `(0.72, 0.075)` -> neck top `(0.98, 0.08)`, finished with a
rolled lip.  Each span becomes ONE `roundcone` part; each part's `y`
position is the previous part's top, so the segments chain end to end.

`part` field layout (same as Recipe 3): `<prim> <op> <k>  <pos xyz>
<euler xyz deg>  <scale xyz>  <a b c>  <round>`.  For `roundcone`,
`a b c` = `<r1> <r2> <h>`; for `torus`, `a b` = `<major> <minor>`; for
`box`, `a b c` = the HALF-extents.

Three things to notice in the part list:

1. **Every joint after the first is `smin`, not `union`**, with the
   blend radius `k` roughly a third of the local radius (`0.10` at the
   fat belly joint, tapering to `0.02` at the lip).  Swap those `smin`s
   for `union` and the same five parts render as a stepped stack -- the
   blend IS the difference between a turned vessel and a pile of cones.
2. **The last part is `box subtract`**, half-extents `1.2 0.6 1.2` at
   `y=-0.6`, so its top face lands exactly on `y=0`.  That flattens the
   hemispherical cap on the bottom-most `roundcone`, giving the flat
   base a vessel needs to sit on a table.
3. **The neck is a `sweep_geometry`, not part of the profile**, because
   it CURVES -- an eight-point circular `profile_point` polygon (radius
   0.035) swept along a five-point Catmull-Rom path.  Its first path
   point sits at `0.16 0.62 0`, inside the body's surface at that
   height, so it reads as joined rather than floating alongside.

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

# The PROFILE: one roundcone per (height, radius) span, chained end to
# end, every joint after the first blended with smin so the silhouette
# is continuous.  The closing box subtract flattens the bottom cap.
sdf_geometry
{
	name	flask_body
	part	roundcone union 0  0 0.02 0  0 0 0  1 1 1  0.26 0.31 0.26  0.0
	part	roundcone smin 0.10  0 0.28 0  0 0 0  1 1 1  0.31 0.22 0.26  0.0
	part	roundcone smin 0.08  0 0.54 0  0 0 0  1 1 1  0.22 0.075 0.18  0.0
	part	roundcone smin 0.05  0 0.72 0  0 0 0  1 1 1  0.075 0.08 0.26  0.0
	part	torus smin 0.02  0 0.99 0  0 0 0  1 1 1  0.085 0.024 0.0  0.0
	part	box subtract 0  0 -0.6 0  0 0 0  1 1 1  1.2 0.6 1.2  0.0
	maxsteps	256
}

standard_object
{
	name		obj_flask_body
	geometry	flask_body
	material	mat_glass
}

# The NECK: a fixed circular cross-section swept along a curve -- the
# job sweep_geometry actually does.  profile_point is <x> <h> in the
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
one continuous tube.  Compare that against the same five radii authored
as five `cylinder_geometry` chunks, which produces a staircase.

If your version shows a visible ledge at a joint, the `smin` `k` there
is too small for the radius change across it -- raise it.  If the vessel
sinks into the table, the closing `box subtract` is missing or its top
face is not at the base plane (`position.y + half-extent.y` must equal
the intended base height).  If the neck floats beside the body, its
first `point` is outside the body's radius at that height -- move it
inward until it is buried, and confirm with a render, not with
arithmetic.

**A one-call alternative to the body profile above.**
`insert_geometry_scaffold {family:"blended_vessel", name:"vessel1",
size:1.0, detail:0.5, aspect:1.0}` expands a base/belly/rim roundcone
`smin` chain plus the same flat-bottom `box subtract` into ONE
`sdf_geometry` chunk (`tmpl_vessel1_vessel`) in a single call --
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
no cross-object coordination required.

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
	profile_point	0.5 0.5
	profile_point	-0.5 0.5
	profile_point	-0.5 -0.5
	profile_point	0.5 -0.5
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
