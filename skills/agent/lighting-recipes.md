# Lighting Recipes
> hook: Read when adding or tuning lights — three-point setups, environment/IBL, emissive geometry, or when brightness/units look wrong.

## Three-point lighting

Key = a spot aimed at the subject; fill = a dim omni opposite the key;
rim = a directional from behind-above to separate subject from
background.  Remember: `direction` on a directional light points
FROM-surface-TO-light; spots aim with `position` + `target` instead.

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
	width	256
	height	256
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
	name	pnt_gray
	color	0.7 0.7 0.7
}

lambertian_material
{
	name		mat_subject
	reflectance	pnt_gray
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

## Environment / IBL

`radiance_map` on the rasterizer names a painter used as the
environment; `radiance_background TRUE` also shows it behind the
scene.  Any painter works — a `uniformcolor_painter` gives a constant
dome (below); for a real light probe use an `hdr_painter` with
`file lightprobes/<name>.hdr` and scale with `radiance_scale`.

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
	radiance_scale			1.5
	radiance_background		TRUE
}

film
{
	width	256
	height	256
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
	name	pnt_white
	color	0.8 0.8 0.8
}

lambertian_material
{
	name		mat_white
	reflectance	pnt_white
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
	material	mat_white
}
```

## Emissive geometry (area light)

A `lambertian_luminaire_material` turns any object into an area
emitter: `exitance` is the emission color painter, `scale` the
brightness, `material` the underlying surface (`none` = pure emitter).
Prefer area emitters over point lights for soft shadows and for BDPT /
VCM scenes; prefer point/omni lights for cheap hard-shadow setups.

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
	width	256
	height	256
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

# An emitter quad facing the sphere from the camera side.
clippedplane_geometry
{
	name	quad_emit
	pta		-0.6 0.6 3.5
	ptb		0.6 0.6 3.5
	ptc		0.6 -0.6 3.5
	ptd		-0.6 -0.6 3.5
}

standard_object
{
	name		obj_emit
	geometry	quad_emit
	material	mat_emit
}
```

## Power / units guidance

- Directional & ambient: radiance = `color * power`, no distance
  falloff.  `power 3.14` is a good starting key.
- Omni & spot: `color * power / r^2` — scale power with the SQUARE of
  the distance (a light 10 units away needs ~100x the power of one at
  1 unit).
- Luminaire `scale` multiplies exitance directly; 10-150 is a typical
  range for a small quad lighting a room-sized scene.
- If everything is black: check the directional `direction` sign first
  (FROM-surface-TO-light), then power magnitudes.
