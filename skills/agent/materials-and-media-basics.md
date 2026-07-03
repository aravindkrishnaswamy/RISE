# Materials and Media Basics
> hook: Read before adding or editing materials (diffuse, glass, metal, PBR), scalar parameters like IOR/roughness, or participating media.

## Material starters (painter wiring included)

Materials reference painters BY NAME; declare the painter first.  Four
common starters — matte, glass, metal, PBR — on one lit stage:

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
	location	0 1 8
	lookat		0 0.5 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_blue
	color	0.15 0.25 0.75
}

uniformcolor_painter
{
	name	pnt_silver
	color	0.95 0.95 0.95
}

uniformcolor_painter
{
	name	pnt_gold
	color	0.83 0.69 0.22
}

uniformcolor_painter
{
	name	pnt_red
	color	0.8 0.15 0.1
}

# 1. Matte diffuse.
lambertian_material
{
	name		mat_diffuse
	reflectance	pnt_blue
}

# 2. Glass: tau = transmittance tint, ior/scattering are PHYSICAL
#    scalars (inline numbers are fine; see the scalar section below).
dielectric_material
{
	name		mat_glass
	tau			0.95 0.98 0.95
	ior			1.5
	scattering	100000.0
}

# 3. Metal: a glossy Cook-Torrance conductor (rd/rs tints, facets =
#    microfacet roughness, ior/extinction = conductor Fresnel).
cooktorrance_material
{
	name		mat_gold
	rd			pnt_gold
	rs			pnt_gold
	facets		0.08
	ior			2.5
	extinction	3.0
}

# 4. PBR metallic-roughness (glTF-style; metallic/roughness accept a
#    painter name OR an inline scalar).
pbr_metallic_roughness_material
{
	name		mat_pbr
	base_color	pnt_red
	metallic	0.0
	roughness	0.35
}

sphere_geometry
{
	name	sph
	radius	0.9
}

standard_object
{
	name		s_diffuse
	geometry	sph
	material	mat_diffuse
	position	-3 0.5 0
}

standard_object
{
	name		s_glass
	geometry	sph
	material	mat_glass
	position	-1 0.5 0
}

standard_object
{
	name		s_gold
	geometry	sph
	material	mat_gold
	position	1 0.5 0
}

standard_object
{
	name		s_pbr
	geometry	sph
	material	mat_pbr
	position	3 0.5 0
}

directional_light
{
	name		key
	power		3.14
	color		1 1 1
	direction	0.3 0.5 0.8
}
```

## Colors vs physical scalars — why `scalar_painter` matters

RISE has TWO painter pipes.  `IPainter` is the COLOR pipe (reflectance,
emission, tau tints): its values pass through colorspace conversion and
spectral uplift.  `IScalarPainter` is the PHYSICAL-SCALAR pipe (IOR,
roughness, scattering/absorption coefficients, phase asymmetry): values
are raw magnitudes, NEVER color-converted or uplifted.  Binding a color
painter into a scalar slot silently mangles values in spectral renders
(e.g. `scattering 1000000` clamped to ~1).  Route by physical meaning:
tinted attenuation/reflectance/emission -> color painter; coefficient
with units -> `scalar_painter` (forms: `value <x>`, `values <r g b>`,
`file <spd>`, `sellmeier ...`).  Inline numbers in scalar slots (`ior
1.5`) are already scalar-safe.

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
	location	0 0 5
	lookat		0 0 0
	up			0 1 0
	fov			40.0
}

# A named physical scalar: crown-glass IOR through the scalar pipe.
scalar_painter
{
	name	sp_ior_crown
	value	1.52
}

dielectric_material
{
	name		mat_crown_glass
	tau			0.98 0.98 0.98
	ior			sp_ior_crown
	scattering	100000.0
}

sphere_geometry
{
	name	sph
	radius	1.0
}

standard_object
{
	name		s_glass
	geometry	sph
	material	mat_crown_glass
}

directional_light
{
	name		key
	power		3.14
	color		1 1 1
	direction	0.2 0.4 0.9
}
```

## Participating media starter

A `homogeneous_medium` gives volumetric absorption/scattering; bind it
to a closed object via `interior_medium`.  `absorption`/`scattering`
are per-channel coefficients (units 1/distance); `phase` is
`isotropic` or `hg <g>` (Henyey-Greenstein, g in [-1,1], + = forward).

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
	location	0 0 5
	lookat		0 0 0
	up			0 1 0
	fov			40.0
}

# Green-tinted absorbing interior (absorbs red+blue, passes green).
homogeneous_medium
{
	name		medium_green
	absorption	0.9 0.05 0.9
	scattering	0.05 0.05 0.05
	phase		isotropic
}

dielectric_material
{
	name		mat_shell
	tau			0.98 0.98 0.98
	ior			1.5
	scattering	100000.0
}

sphere_geometry
{
	name	sph
	radius	1.0
}

standard_object
{
	name			s_flask
	geometry		sph
	material		mat_shell
	interior_medium	medium_green
}

directional_light
{
	name		key
	power		3.14
	color		1 1 1
	direction	0.2 0.4 0.9
}
```
