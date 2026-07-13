# Materials and Media Basics
> hook: Read before adding or editing materials (diffuse, glass, metal, PBR), scalar parameters like IOR/roughness, or participating media.

## EFFICIENT MATERIAL WORKFLOW — reuse painters, pick the cheap chunk kind

For a plain "add/change this material" request (e.g. "give the sphere a
shiny metallic look"), the whole edit is 4 tool calls: read_document ->
read_schema (only if unsure of a param) -> insert_chunk -> propose_patch
(bind).  Two rules keep it that short instead of ballooning past budget:

- **Reuse the scene's existing colour painter** for the new material's
  colour slot instead of inserting a new `uniformcolor_painter`.  A new
  painter is an extra insert_chunk call AND an extra name to manage —
  only add one when the request needs a genuinely different colour
  than anything already in the scene.
- **Default to `pbr_metallic_roughness_material`, not `ggx_material` /
  `cooktorrance_material`, for ordinary metal/shiny asks.**  Verified
  against the chunk parsers (`ChunkParserRegistry.cpp` /
  `Job::AddPBRMetallicRoughnessMaterial` vs `Job::AddGGXMaterial` /
  `Job::AddCookTorranceMaterial`): PBR-MR's `metallic` and `roughness`
  accept EITHER a painter reference OR a bare inline scalar string
  (`metallic 1.0`, `roughness 0.1` — no painter needed); only
  `base_color` must be a real painter name (reuse one).  `ggx_material`
  / `cooktorrance_material` are stricter: their `rd` and `rs`
  (diffuse/specular reflectance) MUST each be an existing painter
  name — there is NO inline-number fallback for `rd`/`rs` (a bare `rs
  0.9 0.9 0.9` is rejected), even though their *other* params
  (`alphax`/`alphay`/`ior`/`extinction`/`facets`) DO accept a single
  inline scalar (`ior 2.5`).  That asymmetry is what burns tool-call
  budget: reach for ggx/cooktorrance only when the task needs explicit
  conductor Fresnel control; otherwise PBR-MR does the same job for
  fewer calls because it needs only one painter reference, not two.

Golden sequence, scene already has a `pnt_albedo` painter bound to the
sphere's current material:

```
1. read_document                                 # find the object + its current painter name
2. read_schema {keyword:"pbr_metallic_roughness_material"}   # skip if you already know the shape below
3. insert_chunk  pbr_metallic_roughness_material {
       name        mat_metallic
       base_color  pnt_albedo   # REUSED, not a new painter
       metallic    1.0          # inline scalar, no painter needed
       roughness   0.1          # inline scalar, no painter needed
   }
4. propose_patch                                 # bind: object.material -> mat_metallic
```

## Specular materials need something to reflect or refract

A glass or metal object shows ONLY what arrives at it from the rest of
the scene.  A lone specular sphere under a directional (delta) light in
an empty void renders BLACK BY CONSTRUCTION: NEE cannot connect a delta
light to a delta BSDF, and there is nothing for the surface to mirror
or transmit.  Always give glass/metal a diffuse backdrop AND/OR an
environment dome (`radiance_map` on the rasterizer) — the same
anti-pattern rule as docs/skills/effective-rise-scene-authoring.md
documents for metals.  Every snippet below follows it.

## Material starters (painter wiring included)

Materials reference painters BY NAME; declare the painter first.  Four
common starters — matte, glass, metal, PBR — on one lit stage with a
floor and a sky dome (so the glass and gold actually read):

```rise
RISE ASCII SCENE 7

# The dome painter FIRST -- the rasterizer references it by name.
uniformcolor_painter
{
	name	pnt_sky
	color	0.45 0.55 0.75
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
	radiance_background		TRUE
}

film
{
	width	128
	height	128
}

# fov 55 at this distance keeps all four spheres fully in frame.
pinhole_camera
{
	location	0 1.2 9
	lookat		0 0.5 0
	up			0 1 0
	fov			55.0
}

uniformcolor_painter
{
	name	pnt_blue
	color	0.15 0.25 0.75
}

# Gold reads as gold with a bright warm specular tint (rs) over a
# deep warm diffuse base (rd) -- identical rd/rs looks like plastic.
uniformcolor_painter
{
	name	pnt_gold_warm
	color	1.0 0.77 0.34
}

uniformcolor_painter
{
	name	pnt_gold_deep
	color	0.35 0.20 0.03
}

uniformcolor_painter
{
	name	pnt_red
	color	0.8 0.15 0.1
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
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
	rd			pnt_gold_deep
	rs			pnt_gold_warm
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

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

sphere_geometry
{
	name	sph
	radius	0.9
}

# The floor: gives the specular spheres something to reflect/refract.
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
painter into a scalar slot used to silently mangle values in spectral
renders (e.g. `scattering 1000000` clamped to ~1); today the parser
emits a per-parameter diagnostic at parse time — heed it rather than
guessing.  Route by physical meaning: tinted
attenuation/reflectance/emission -> color painter; coefficient
with units -> `scalar_painter` (forms: `value <x>`, `values <r g b>`,
`file <spd>`, `sellmeier ...`).  Inline numbers in scalar slots (`ior
1.5`) are already scalar-safe.

The demo puts the glass sphere in front of a checkered backdrop, under
a sky dome — remember, glass in a void is black; the backdrop and dome
are what you see refracted through the sphere:

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.45 0.55 0.75
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

# The backdrop the glass refracts: a two-tone checker wall.
uniformcolor_painter
{
	name	pnt_check_light
	color	0.85 0.85 0.85
}

uniformcolor_painter
{
	name	pnt_check_dark
	color	0.1 0.2 0.5
}

checker_painter
{
	name	pnt_checker
	colora	pnt_check_light
	colorb	pnt_check_dark
	size	0.5
}

lambertian_material
{
	name		mat_backdrop
	reflectance	pnt_checker
}

sphere_geometry
{
	name	sph
	radius	1.0
}

# An infinite plane defaults to the XY plane facing +Z -- a ready-made
# backdrop wall behind the sphere.
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
The gray wall and dome behind the flask are what make the tint
visible — light reaching the camera THROUGH the medium is what gets
colored, and a medium in a void has nothing behind it to color:

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.35 0.35 0.35
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

# The gray wall seen through the flask -- its light picks up the
# green tint on the way to the camera.
uniformcolor_painter
{
	name	pnt_wall
	color	0.6 0.6 0.6
}

lambertian_material
{
	name		mat_wall
	reflectance	pnt_wall
}

sphere_geometry
{
	name	sph
	radius	1.0
}

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
	material	mat_wall
	position	0 0 -2
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
