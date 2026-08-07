# Lighting Recipes
> hook: Read when adding or tuning lights — three-point setups, environment/IBL, emissive geometry, or when brightness/units look wrong.

## Three-point lighting

Key = a spot aimed at the subject; fill = a dim omni opposite the key;
rim = a directional from behind-above to separate subject from
background.

**A warm key light ALONE crushes the scene to orange -- always pair it
with a fill.** This is the single most common lighting failure on a
"warm"/"cozy"/"pastel" brief: authors give the room one warm light
(exitance like `1.0 0.6 0.3`) and nothing else, so every shadow falls to
near-black with no blue in it at all, and the whole frame reads as a
muddy amber blob -- not the warm-but-readable room the brief wanted.
Measured: a warm key over grey walls with NO fill renders at a
red-to-blue channel ratio of ~3-40 (the higher the more crushed);
adding even a dim fill pulls it back to ~1.4-2.2 and the form becomes
legible. So on any warm brief: place the warm key, then add a SECOND,
dimmer light -- a cool-ish fill (`0.4 0.55 0.8`, ~half the key's scale)
is best because it puts blue back into the shadows, but even a neutral
or gently-warm fill rescues it. "Warm" means warm KEY, not warm-only;
the shadows still need light in them. If a render comes back visibly
orange or you cannot make out the subject's shading, you are missing
the fill.  Remember: `direction` on a directional light points
FROM-surface-TO-light; spots aim with `position` + `target` instead.
The floor is deliberate: without a backdrop there is nothing for the
fill to lift or the rim to separate the subject FROM.  Samples 64
renders reasonably clean at this size; production quality wants
several hundred.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			64
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
	location	0 1 6
	lookat		0 0.5 0
	up			0 1 0
	fov			40.0
}

uniformcolor_painter
{
	name	pnt_gray_a
	color	0.82 0.83 0.86
}

uniformcolor_painter
{
	name	pnt_gray_b
	color	0.35 0.37 0.42
}

# View-angle colour shift instead of a flat grey -- a lit subject reads
# its shading better than a plain matte ball.  Swap back to
# uniformcolor_painter for a neutral subject.
iridescent_painter
{
	name	pnt_gray
	colora	pnt_gray_a
	colorb	pnt_gray_b
	bias	0.05
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.4 0.4 0.4
}

lambertian_material
{
	name		mat_subject
	reflectance	pnt_gray
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

sphere_geometry
{
	name	sph
	radius	1.0
}

standard_object
{
	name		subject
	geometry	sph
	material	mat_subject
	position	0 0.5 0
}

# The floor the shadows fall on (an infinite plane defaults to the XY
# plane; rotate -90 about X to lay it flat).
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
	position	0 -0.5 0
	orientation	-90 0 0
}

# KEY: bright, warm, above-front-right.  inner/outer are cone
# half-angles in degrees; power falls off as 1/r^2.
spot_light
{
	name		key
	power		60
	color		1.0 0.95 0.85
	position	3 4 4
	target		0 0.5 0
	inner		20
	outer		45
}

# FILL: dim, cool, front-left -- lifts the key's shadows.
omni_light
{
	name		fill
	power		12
	color		0.7 0.8 1.0
	position	-4 1 4
}

# RIM: from behind-above (negative Z = behind the subject, since the
# camera is at +Z).  direction is FROM-surface-TO-light.
directional_light
{
	name		rim
	power		2.0
	color		1 1 1
	direction	-0.2 0.6 -0.77
}
```

**A quick varied subject material.** The demo above gives the subject a
view-shifting `iridescent_painter` tint; for a subject with real surface
texture instead, `insert_material_scaffold {family:"brushed_metal",
name:"fixture1", tone:"0.55 0.56 0.6", wear:0.5, scale:4.0}` expands
into a wired `ward_anisotropic_material` (`tmpl_fixture1_mat`) with
both `alphax`/`alphay` bound to a spatially-varying scalar field, in
one call -- point `mat_subject`'s slot at it, or reference it from a
fresh `standard_object.material`.

## Environment / IBL

`radiance_map` on the rasterizer names a painter used as the
environment; `radiance_background TRUE` also shows it behind the
scene.  Any painter works — a `uniformcolor_painter` gives a constant
dome (below); for a real light probe use an `hdr_painter` with
`file lightprobes/<name>.hdr` and scale with `radiance_scale`.  Pick
a subject color/albedo with CONTRAST against the dome — a white
sphere inside a bright uniform dome converges to the dome color and
disappears into the background.  A uniform dome also gives flat,
shadowless shading — physically correct, since the incident light is
identical from every direction; use an `hdr_painter` probe when you
want directional modelling and soft shadows.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.5 0.65 0.9
}

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples					16
	pixel_filter			box
	oidn_denoise			FALSE
	radiance_map			pnt_sky
	radiance_scale			1.0
	radiance_background		TRUE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	0 0 5
	lookat		0 0 0
	up			0 1 0
	fov			40.0
}

uniformcolor_painter
{
	name	pnt_rust_dark
	color	0.25 0.08 0.04
}

uniformcolor_painter
{
	name	pnt_rust_bright
	color	0.6 0.15 0.1
}

# Ridged/veiny turbulence reads as oxidation mottle -- a flat rust
# colour is exactly the amateur-render tell procedural-textures warns
# about.
turbulence3d_painter
{
	name		pnt_rust
	colora		pnt_rust_dark
	colorb		pnt_rust_bright
	octaves		4
	persistence	0.6
	scale		4.0 4.0 4.0
}

lambertian_material
{
	name		mat_rust
	reflectance	pnt_rust
}

sphere_geometry
{
	name	sph
	radius	1.0
}

standard_object
{
	name		obj_sphere
	geometry	sph
	material	mat_rust
}
```

## Emissive geometry (area light)

A `lambertian_luminaire_material` turns any object into an area
emitter: `exitance` is the emission color painter, `scale` the
brightness, `material` the underlying surface (`none` = pure emitter).
Prefer area emitters over point lights for soft shadows and for BDPT /
VCM scenes; prefer point/omni lights for cheap hard-shadow setups.
**Winding rule**: the quad's vertex ORDER picks the emitting side — if
an emissive quad gives a black render, flip the point order.

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
	location	0 0 3.5
	lookat		0 0 0
	up			0 1 0
	fov			40.0
}

uniformcolor_painter
{
	name	pnt_albedo
	color	0.5 0.5 0.5
}

lambertian_material
{
	name		mat_diffuse
	reflectance	pnt_albedo
}

sphere_geometry
{
	name	sph
	radius	0.8
}

standard_object
{
	name		obj_sph
	geometry	sph
	material	mat_diffuse
}

# The floor that shows the area light's soft falloff gradient.
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
	material	mat_diffuse
	position	0 -0.8 0
	orientation	-90 0 0
}

uniformcolor_painter
{
	name	pnt_emit
	color	1.0 1.0 1.0
}

lambertian_luminaire_material
{
	name		mat_emit
	exitance	pnt_emit
	scale		30.0
	material	none
}

# The emitter: a horizontal quad ABOVE-RIGHT of the sphere (off the
# camera axis, so the gradient it casts is actually visible).  This
# point order makes the quad emit DOWNWARD, toward the scene.
clippedplane_geometry
{
	name	quad_emit
	pta		0.9 1.8 0.2
	ptb		2.1 1.8 0.2
	ptc		2.1 1.8 1.4
	ptd		0.9 1.8 1.4
}

standard_object
{
	name		obj_emit
	geometry	quad_emit
	material	mat_emit
}
```

## Atmospheric volumes

A hand-built "atmosphere" is easy to get wrong two ways: a uniform-
density sphere/box filled with a plain `homogeneous_medium` never
swirls (it looks like a flat grey haze, not drifting mist or smoke),
and the medium is USELESS unless an object's `interior_medium` binds
it -- a medium chunk sitting unreferenced in the document does nothing.

**A one-call elongated, swirling volume: `volume_bank`.**
`insert_geometry_scaffold {family:"volume_bank", name:"mist1",
size:1.4, aspect:2.2, detail:0.6, tone:"0.75 0.8 0.85"}` expands a
fully-wired atmospheric bank in one call: an elongated ellipsoid
container, a near-invisible dielectric boundary (so the container's
own surface never reads as a hard edge), and a
`painter_heterogeneous_medium` whose density comes from a domain-
warped noise painter -- so the volume genuinely drifts and swirls
instead of reading as a flat fog block.  `aspect` elongates the bank
along its local X axis (a horizontal drift, not a ball of fog);
`detail` raises the noise field's warp amplitude and frequency
together for a wispier, more turbulent result; `tone` tints what the
medium scatters (and, inversely, what it absorbs) -- a cool pale blue
like the example above reads as mist, a warm dim orange as smoke or
haze.  Unlike every other `insert_geometry_scaffold` family, this ONE
family also emits the dielectric material AND a `standard_object`
binding geometry + material + `interior_medium` together (returned as
`material`/`medium`/`object`, each `{name,kind}`) -- a bare volume
graph would otherwise do nothing, and the medium's density field is
anchored in WORLD SPACE at creation time, so the scaffold has to place
the object itself to keep the density aligned with the container.  The
call's result carries a factual `message`: if you reposition or
rescale the emitted object afterward, the medium's `bbox_min`/
`bbox_max` need a matching hand-edit or the swirl will drift out of
alignment with the container's new position.

## Power / units guidance

- Directional & ambient: radiance = `color * power`, no distance
  falloff.  `power 3.14` is a good starting key.
- Omni & spot: `color * power / r^2` — scale power with the SQUARE of
  the distance (a light 10 units away needs ~100x the power of one at
  1 unit).
- Luminaire `scale` multiplies exitance directly; 10-150 is a typical
  range for a small quad lighting a room-sized scene.
- If everything is black: check the directional `direction` sign first
  (FROM-surface-TO-light), then power magnitudes; for an emissive
  quad, check the vertex winding (it picks the emitting side).
