# Materials and Media Basics
> hook: Read before adding or editing materials (diffuse, glass, metal, PBR, CLOTH/FABRIC), scalar parameters like IOR/roughness, or participating media. Cloth is NOT a coloured Lambertian -- see "Cloth and fabric" below, or just call `make_fabric`.

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
- **`uniformcolor_painter` is for FLAT colour only.**  The moment the ask
  names a real material (wood, stone, marble, rust, wear, water, cloth)
  or the surface is a large hero one (table top, floor, wall, ground), a
  flat colour is the wrong answer and RISE's ~36 procedural painters are
  the right one — read
  `read_skill {name:"procedural-textures"}` for the intent →
  painter-family map and two verified recipes.  This skill covers the
  material and how its slots bind; that one covers what fills the
  colour slot.
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

# SCALAR pipe: facets is a physical scalar, so it takes a scalar_painter,
# never a colour painter -- a worn gold surface polishes unevenly with
# handling rather than holding one uniform facet size everywhere.
expression_function2d
{
	name	fn_gold_wear
	param	bands 3.0
	def		s abs( v * bands - 0.5 )
	expr	smoothstep( 0.05, 0.3, s )
}

scalar_painter
{
	name		sp_gold_wear
	function2d	fn_gold_wear
	scale		0.10
	bias		0.03
}

# 3. Metal: a glossy Cook-Torrance conductor (rd/rs tints, facets =
#    microfacet roughness, ior/extinction = conductor Fresnel).  facets
#    is bound to the scalar_painter above instead of one constant.
cooktorrance_material
{
	name		mat_gold
	rd			pnt_gold_deep
	rs			pnt_gold_warm
	facets		sp_gold_wear
	ior			2.5
	extinction	3.0
}

uniformcolor_painter
{
	name	pnt_pbr_rough_lo
	color	0.15 0.15 0.15
}

uniformcolor_painter
{
	name	pnt_pbr_rough_hi
	color	0.55 0.55 0.55
}

# roughness is a COLOUR-painter slot on this chunk (auto-adapted to a
# scalar internally), not a scalar_painter slot -- a moulded/machined
# part rarely holds one uniform roughness edge to edge.
perlin3d_painter
{
	name		pnt_pbr_roughness
	colora		pnt_pbr_rough_lo
	colorb		pnt_pbr_rough_hi
	octaves		3
	persistence	0.6
	scale		5.0 5.0 5.0
}

# 4. PBR metallic-roughness (glTF-style; metallic/roughness accept a
#    painter name OR an inline scalar -- roughness here uses the
#    grayscale procedural painter above).
pbr_metallic_roughness_material
{
	name		mat_pbr
	base_color	pnt_red
	metallic	0.0
	roughness	pnt_pbr_roughness
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

## Roughness is not a number — bind it to an expression

Use this whenever a surface is bigger than a trinket: real objects are
polished in some places and worn in others, and one constant in
`roughness`/`alphax`/`facets` is the flat-plastic look no amount of
lighting fixes.  A `scalar_painter { expression ... }` is the whole fix —
ONE chunk, no colourspace, no adapter chain.  Note the shape to copy:
every art-directable number is a `param` with `min`/`max`/`step`/`label`
(so a human retunes it with a slider, and `propose_patch` retunes it by
name), and the noise is remapped into `[0,1]` with `clamp` before the
`mix`, because raw `fbm` runs roughly -0.4 .. 0.4 and an unclamped mix
would walk the roughness outside the band.

If you would rather not type it, `vary_material` writes exactly this chunk
for the most prominent bare-number material in the scene and rebinds the
slot, in one call.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.42 0.46 0.55
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
	location	0 0.7 3.2
	lookat		0 0 0
	up			0 1 0
	fov			38.0
}

uniformcolor_painter
{
	name	pnt_pewter_deep
	color	0.10 0.10 0.11
}

uniformcolor_painter
{
	name	pnt_pewter_spec
	color	0.62 0.62 0.60
}

# THE POINT OF THIS EXAMPLE.  A worley cell field bands the microsurface
# between polished cell centres and weathered cell edges -- hammered
# pewter.  `P` is the world intersection point, so this needs no UVs and
# shows no seams.  No colourspace conversion and no spectral uplift ever
# touch a scalar_painter, which is why `expression` belongs here and a
# colour painter does NOT bind to alphax/alphay.
scalar_painter
{
	name		sp_hammered
	param		cell_freq 4.5 min 0.5 max 12 step 0.25 label "Cell frequency"
	param		rough_lo 0.06 min 0.001 max 1 step 0.005 label "Polished roughness"
	param		rough_hi 0.48 min 0.001 max 1 step 0.005 label "Weathered roughness"
	def			f1 worley_f1(P*cell_freq, 1.0)
	expression	mix(rough_lo, rough_hi, clamp(f1, 0, 1))
}

ggx_material
{
	name		mat_pewter
	rd			pnt_pewter_deep
	rs			pnt_pewter_spec
	alphax		sp_hammered
	alphay		sp_hammered
	ior			2.2
	extinction	3.4
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.32 0.32 0.34
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

sphere_geometry
{
	name	bowl
	radius	0.85
}

standard_object
{
	name		obj_bowl
	geometry	bowl
	material	mat_pewter
	position	0 0.15 0
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
	position	0 -0.75 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		3.2
	color		1.0 0.955 0.89
	direction	0.4 0.55 0.8
}
```

## Wear follows the form, not the axis — patina in the crevices, done right

The prior a lot of agents carry is "masks are made of `P.z`" — pick a world
axis, threshold it, call it grime.  That fakes a rim of dirt at a fixed
*altitude* no matter what the object's actual shape is.  Real patina
collects where the FORM traps it — inside a scar, along a seam, in the
crease where two masses meet — which is a statement about curvature, not
position.  RISE's `curv` context variable (`expression_painter` /
`scalar_painter { expression ... }`) is exactly that signal: **positive =
convex (an edge), negative = concave (a crevice), 0 = flat**, exact on
`sdf_geometry` / `skeleton_geometry` and the analytic curved primitives.
`clamp(-curv, 0, 1)` is a crevice mask on ANY scene scale — no per-object
tuning, because `curv` is already normalized to the hit geometry's own
size.  Composing it with a second `fbm` field breaks the mask into patchy
oxidation instead of a smooth, obviously-procedural AO ramp — the tell
that gives away a lazy wear pass.  As in the roughness example above,
every art-directable number is a `param` with `min`/`max`/`step`/`label`.

If you would rather not hand-author it at all, call **`add_wear`** — zero
required arguments; it finds the material that is still one flat colour on
geometry that curves, writes exactly this composition banded around the
colour already there (plus the matching roughness field), and rebinds the
slots, in one call and one undo step.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.35 0.40 0.50
}

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples					32
	pixel_filter			box
	oidn_denoise			FALSE
	radiance_map			pnt_sky
	radiance_background		TRUE
}

film
{
	width	256
	height	256
}

pinhole_camera
{
	location	0 0 3.2
	lookat		0 0 0
	up			0 1 0
	fov			32.0
}

# THE FIELD.  Scalar-typed crevice mask driven by NEGATIVE curv (concave =
# crevice, per the sign convention above), DEEPENED by an occlusion-derived
# boost -- (1-occlusion(0.08)) is 0 where a point is genuinely in the open
# and grows toward 1 the more enclosed it truly is, and occl_gain turns
# that into a >=1 multiplier, so curv's own reading is left untouched in
# the open and pushed harder in a real pocket -- then broken up by a
# second fbm so the patina reads as patchy oxidation instead of a flat AO
# ramp.
expression_painter
{
	name		pnt_wear_field
	param		crevice_gain 6.0 min 0.5 max 20 step 0.5 label "Crevice sensitivity"
	param		breakup_freq 10.0 min 1 max 40 step 0.5 label "Breakup frequency"
	param		occl_gain 1.5 min 0 max 4 step 0.1 label "Occlusion boost"
	def			crevice_mask clamp(-curv * crevice_gain, 0, 1)
	def			cavity_boost 1.0 + occl_gain * (1.0 - occlusion(0.08))
	def			breakup 0.5 + 0.5 * fbm(P*breakup_freq, 4, 0.5, 2.0)
	expr		clamp(crevice_mask * cavity_boost * breakup, 0, 1)
}

# CONSUMER 1 -- colour: clean bronze at 0, dark crusted patina at 1.
ramp_painter
{
	name			pnt_patina_color
	input			pnt_wear_field
	channel			R
	interpolation	smooth
	stop			0.00  0.42 0.28 0.14
	stop			1.00  0.04 0.07 0.05
	color_space		Rec709RGB_Linear
}

# CONSUMER 2 -- the PHYSICAL SCALAR, same field, any-painter bridge:
# roughness 0.10 (polished bronze) .. 0.60 (crusted patina crust).
scalar_painter
{
	name		sp_wear_rough
	painter		pnt_wear_field
	channel		R
	scale		0.50
	bias		0.10
}

uniformcolor_painter
{
	name	pnt_bronze_spec
	color	0.55 0.48 0.32
}

ggx_material
{
	name		mat_patina
	rd			pnt_patina_color
	rs			pnt_bronze_spec
	alphax		sp_wear_rough
	alphay		sp_wear_rough
	ior			1.18
	extinction	2.8
}

# THE CREATURE-LIKE BODY: a round head with a carved scar/dimple
# (subtract) -- an unambiguous concave crevice (inside the scar) against
# an unambiguous convex body (the rest of the head), the family curv is
# EXACT on -- plus a small fused knot (union) on the opposite side.  The
# knot's own crease reads negative under curv too (a union between two
# spheres is a genuine concave wedge, not a hemispherical dimple), but it
# is ALSO genuinely enclosed in a way a subtract-carved scar is not:
# occlusion(0.08) reads down to roughly 0.68 right at the seam where the
# knot meets the skull, comfortably below unoccluded, while the scar
# itself stays close to 1 (occlusion sees creases and folds, not smooth
# dimples -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md).  That is what
# cavity_boost is for.
sdf_geometry
{
	name	head
	part	sphere union 0     0 0 0     0 0 0   1 1 1   0.75 0 0   0
	part	sphere subtract 0.08   0.35 0.1 0.55   0 0 0   1 1 1   0.42 0 0   0
	part	sphere union 0     -0.455 0.273 0.727   0 0 0   1 1 1   0.25 0 0   0
}

standard_object
{
	name		obj_head
	geometry	head
	material	mat_patina
	position	0 0 0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.30 0.30 0.32
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
	position	0 -0.9 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		3.2
	color		1.0 0.955 0.89
	direction	0.35 0.6 0.75
}
```

Rendering just `pnt_wear_field` in isolation (bind it straight to an
unlit `exitance` slot and nothing else) makes the geometric keying
undeniable: the raw mask reads exactly 0 everywhere on the convex body
and a strong nonzero value the instant a ray lands inside the concave
scar — the crevice, not an axis, is what turns the field on.  It also
shows exactly where `cavity_boost` earns its keep and where it does not:
rendering the field with and without `cavity_boost` (64 spp, PNG output
at `color_space Rec709RGB_Linear` / `display_transform none` so pixel
value IS mask value) and averaging over the same 17x17-pixel patch in
each render, the scar's own peak is UNCHANGED at 0.188 in both and its
surrounding patch is unchanged at a mean of 0.112 — curv already
saturates the mask there and `occlusion(0.08)` reads close to 1
(unoccluded), so `1.0 + occl_gain * (1.0 - occlusion(...))` is close to
its neutral 1 and multiplies in almost nothing new.  The same comparison
over the seam where the fused knot meets the skull goes from a mean of
0.0016 to 0.0041 — a 2.5x deepening — because that seam is a genuine
fold occlusion sees as enclosed even though its curv reading, on its own,
was no more dramatic than a lot of other mild creases on the head.  Bind
`curv` (not `P`) whenever the question is "where does wear collect on
THIS shape," and reach for `occlusion(radius)` specifically when some of
those creases are shallow, WIDE folds that a fine-scale, radius-free
signal underrates relative to how enclosed they really are.  There is no
dedicated wear-verb yet; `insert_material_scaffold {family:"aged_bronze",
wear:...}` gets you a comparable patina look in one call today, but
through a reaction-diffusion field rather than curvature or occlusion, so
it won't specifically hug a crevice or a pocket the way this hand-typed
field does.

## Glow that dies in thick walls — thickness, not painted emission

The fake: an emissive gradient painted straight onto an opaque shell
LOOKS like translucency in the one lighting setup the agent happened to
render, and breaks in every other — it doesn't dim when the wall
thickens, doesn't react to the interior light being moved or switched
off, and doesn't tint a cast shadow, because no light is actually
passing through anything.  It is also the single most-repeated agent
failure on record: asked for a thin-walled vessel that glows from
within with the glow dying out where the walls thicken, the agent never
calls `thickness()` at all — it computes a height-based mask, binds it
to `exitance`, and calls the shell "translucent" in its summary even
though the shell is fully opaque.  `thickness(radius)` is occlusion's
sibling for TRANSLUCENCY rather than grime: it returns `[0,1]` with 1 =
thick, so `1 - thickness(0.1)` is a THIN-region mask — the correct fix
binds it into a REAL `translucent_material`'s `ref`/`tau`, not into an
emission slot, so the glow is light that actually crossed the wall.

The shape to copy, distilled from the votive shell in
`scenes/FeatureBased/GeometrySignals/weathered_reliquary.RISEscene`
(that scene's deluxe, plinth-mounted version of this same idiom): an
off-centre CSG subtraction so the wall genuinely varies in thickness,
`thickness(radius)` turned into a `thin` mask, and a `ramp_painter` pair
where reflectance FALLS and transmittance RISES together toward the
thin end — dropping reflectance is what makes the thin rim read as "lit
from within" instead of merely "less shiny".  As in every other starter
in this file, every art-directable number is a `param` with
`min`/`max`/`step`/`label`.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_dusk
	color	0.05 0.06 0.09
}

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples					32
	pixel_filter			box
	oidn_denoise			FALSE
	radiance_map			pnt_dusk
	radiance_background		TRUE
}

film
{
	width	160
	height	160
}

pinhole_camera
{
	location	0 0.05 1.55
	lookat		0 0 0
	up			0 1 0
	fov			30.0
}

# THE VESSEL.  Outer sphere (radius 0.40) minus an inner sphere (radius
# 0.25) offset 0.10 units UP -- the off-centre subtraction is what makes
# the wall genuinely thin at the top rim (0.40-0.10-0.25=0.05) and thick
# at the base (0.40+0.10-0.25=0.25), the same recipe as the votive shell
# in scenes/FeatureBased/GeometrySignals/weathered_reliquary.RISEscene.
sdf_geometry
{
	name	geo_lantern_shell
	part	sphere union    0     0.00 0.00 0.00   0 0 0   1 1 1   0.40 0 0   0
	part	sphere subtract 0.02  0.00 0.10 0.00   0 0 0   1 1 1   0.25 0 0   0
}

# THE POINT OF THIS EXAMPLE.  `thickness(radius)` answers with [0,1],
# 1=thick -- `1 - thickness(...)` is the THIN mask a rim-glow wants.
# This is a LIVE query on the SDF (any expression radius is fine here;
# a mesh would need a literal -- see the descriptor note above).
expression_painter
{
	name	pnt_shell_thinness
	param	thick_radius 0.10 min 0.03 max 0.30 step 0.01 label "Thickness sample radius (bbox fraction)"
	def		thin clamp(1.0 - thickness(thick_radius), 0.0, 1.0)
	expr	thin
}

# Reflectance FALLS and transmittance RISES together toward the thin
# rim -- that pairing is what reads as "lit from within" instead of
# merely "less opaque".  ramp_painter, not a bare uniformcolor_painter,
# because translucent_material's own energy-conservation auto-scale only
# guards a uniformcolor_painter's ref+tau -- these are spatially varying,
# so the by-hand headroom below (0.70+0.02 and 0.30+0.55, both < 1 per
# channel) is what actually keeps the material physical end to end.
ramp_painter
{
	name			pnt_shell_ref
	input			pnt_shell_thinness
	channel			R
	interpolation	smooth
	stop			0.00  0.70 0.68 0.62
	stop			1.00  0.30 0.28 0.24
}

ramp_painter
{
	name			pnt_shell_tau
	input			pnt_shell_thinness
	channel			R
	interpolation	smooth
	stop			0.00  0.02 0.02 0.02
	stop			1.00  0.55 0.42 0.24
}

translucent_material
{
	name		mat_lantern_shell
	ref			pnt_shell_ref
	tau			pnt_shell_tau
	ext			0.0
	N			24.0
	scattering	0.0
}

standard_object
{
	name		obj_lantern
	geometry	geo_lantern_shell
	material	mat_lantern_shell
	position	0 0 0
}

# THE LIGHT THAT ACTUALLY MAKES IT GLOW.  A small warm point well inside
# the 0.25-radius cavity -- nowhere close to the wall -- so every photon
# reaching the camera through the thin rim genuinely passed through the
# translucent material's transmittance, not through a hole in the shell.
shape_light
{
	name		light_candle
	shape		sphere
	center		0 0.10 0
	size		0.05
	color		1.00 0.72 0.32
	exitance	60
}

directional_light
{
	name		key
	power		0.6
	color		0.263 0.342 0.604
	direction	0.3 0.5 0.85
}
```

Rendering this scene and averaging luma (Rec.709 linear, `display_transform
none` so pixel value IS radiance) over horizontal bands from the rim down
to the base shows the gradient the geometry promises: mean band luma goes
65.0 (rim, thickness ≈0.05) → 30.4 → 24.1 → 21.2 → 16.6 (base, thickness
≈0.25) — monotonically brighter exactly where the wall is thinnest.  The
control that tells this apart from a painted fake: delete the
`shape_light` (an `exitance` of exactly 0 refuses to parse — "a light
that emits nothing is not a light" — so the light has to be genuinely
absent, not merely zeroed) and render again.  The same bands come back
14.9 → 20.0 → 19.6 → 16.9 → 12.4 — flat within noise, and the rim is now
the DARKEST band, not the brightest, because with no interior light to
transmit, the thin end's own lower reflectance is all that's left.  An
emissive-gradient fake would keep glowing in both renders, because its
"glow" was never conditioned on a light existing in the first place.

Both signals are answered by two geometry families, by different means.
The volumetric SDF family (`sdf_geometry` / `skeleton_geometry`)
evaluates them live from its distance field and takes a DYNAMIC radius —
any expression you like.  Indexed triangle meshes answer from a
per-vertex field BAKED on the first query that asks for that radius (the
bake logs its own cost) and interpolated over the hit triangle
afterwards, so on a mesh the radius must be a LITERAL: a computed one
reads the neutral value rather than quietly borrowing a table baked at
another scale, and only a handful of distinct literal radii per signal
per mesh are baked before the rest read neutral too.  Everything else —
the analytic primitives, non-indexed meshes, and a heightfield-mode SDF —
reads the neutral 1 (unoccluded / thick), so an unsupported geometry
stays inert rather than lighting up.

## Wet surfaces — a coat over an UNTOUCHED substrate, not a texture

Rain-wet is the most-requested "make it look real" ask after wear, and
it is not a paint job: water lays a real dielectric film over the
substrate, and the film's own layered transport (Fresnel in, attenuate,
scatter off the substrate, internal-reflection recycling back into it,
attenuate, Fresnel out) is what darkens and saturates the substrate —
not a separate paint step.  The recipe is `coated_material` wrapping
the ORIGINAL material chunk UNCHANGED: `base` names it, `coat_weight` =
coverage, `coat_roughness` = coat sharpness.  There is no `reflectance`/
darkening painter to write at all — the coat's own transport does that
work, so adding one on top would double-count the same physics.  One
`param`/`def` prelude, shared byte-for-byte across the two coat-field
chunks, drives everything: `occlusion(0.08)` gated by an up-facing
normal test finds cavities water actually pools in (gravity keeps it
off ceilings and vertical faces), `curv > 0` sheds it off convex
ridges, and a `base_wetness` constant is what makes a FLAT street
wettable at all — on planar geometry both `curv` and `occlusion` go
inert, so without that constant term the whole mask collapses to zero.
`coat_weight` reads the coverage mask directly; `coat_roughness`
sharpens only where `pooling` (not mere dampness) saturates — keying
the coat lobe on wetness alone is the classic "wet asphalt looks like
plastic" failure.  `coat_roughness` is a GGX ALPHA, not a Phong
exponent, so a gloss band authored in the more intuitive "cone
exponent" terms needs the `alpha = sqrt(2/(n+2))` conversion (see the
snippet below) before it lands in the chunk.

If you would rather not hand-type it, **`add_wetness`** — zero required
arguments — finds the qualifying material (non-metallic, flat-coloured,
not already wet) and writes exactly this composition FOR A LAMBERTIAN
BASE: it mints the `coated_material` wrapper and the two coat-field
chunks, and rebinds every bound object's `material` reference to the
wrapper — the original chunk is left byte-for-byte untouched, which is
strictly less destructive than an earlier design that rewrote it in
place.  (A GGX/PBR base still gets in-place roughness + reflectance
modulation instead — no separate coat lobe there yet; an Oren-Nayar
base gets darkening only, no coat, since Phase 2's `coated_material`
substrate allowlist accepts it but the verb has not been re-targeted to
use that for Oren-Nayar in this slice.)

The shape to copy (radius `0.08` on `occlusion` is a LITERAL, never a
`param` — a computed radius degrades to the neutral fallback on indexed
meshes).  This trims the full recipe's `dryness`/`film_amount`/`fbm`
breakup knobs to the essential mechanism; the full parameterized form —
every knob a `param`, plus the breakup and drying controls — is what
`add_wetness` emits and what ships as
`scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene`, the
execution-validated pooled-joint version of this same idiom.  **The
geometry below is a part-based `sdf_geometry` — two overlapping spheres,
not a bare analytic primitive** — on purpose: `occlusion(0.08)` needs a
real self-concavity to read anything but its neutral 1, and a lone
sphere or box has none.  Unioning two spheres gives one: the seam
between them is a genuine concave wedge that occlusion sees as enclosed
(the same fact the patina section above proves numerically), so the
joint between the two "cobbles" pools while the outer lobes stay merely
damp:

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.35 0.40 0.50
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
	location	0 0.85 1.9
	lookat		0 -0.05 0
	up			0 1 0
	fov			36.0
}

uniformcolor_painter
{
	name	pnt_cobble
	color	0.45 0.43 0.40
}

# The ORIGINAL material -- byte-for-byte untouched by the coat wrap
# below, exactly what add_wetness does for a Lambertian base (item 8).
lambertian_material
{
	name		mat_cobble
	reflectance	pnt_cobble
}

scalar_painter
{
	name		wet_coatweight
	def		up_facing clamp(dot(N, vec3(0,1,0)), 0, 1)
	def		cavity (1.0 - occlusion(0.08)) * up_facing
	def		pooling clamp(1.6*cavity, 0, 1)
	def		ridge clamp(curv * 2.2, 0, 1)
	def		damp_raw clamp(0.55 + pooling - ridge, 0, 1)
	def		damp smoothstep(0.0, 1.0, damp_raw)
	expression	damp
}

scalar_painter
{
	name		wet_coatrough
	def		up_facing clamp(dot(N, vec3(0,1,0)), 0, 1)
	def		cavity (1.0 - occlusion(0.08)) * up_facing
	def		pooling clamp(1.6*cavity, 0, 1)
	def		ridge clamp(curv * 2.2, 0, 1)
	def		damp_raw clamp(0.55 + pooling - ridge, 0, 1)
	def		damp smoothstep(0.0, 1.0, damp_raw)
	# alpha = sqrt(2/(n+2)) turns a Phong-cone gloss band (n=220 damp
	# floor, n=200000 pooled/mirror ceiling) into the GGX alpha
	# coat_roughness actually wants.
	expression	mix( sqrt(2.0/222.0), sqrt(2.0/200002.0), clamp(pooling * damp, 0, 1) )
}

# The coat: NO darkening painter here -- the layered transport (Fresnel
# in, recycle against the substrate, Fresnel out) performs it.
coated_material
{
	name		mat_wet
	base		mat_cobble
	coat_weight	wet_coatweight
	coat_roughness	wet_coatrough
}

# Two overlapping spheres, unioned -- a real enclosed joint at the seam
# for occlusion(0.08) to read, not a bare convex primitive.
sdf_geometry
{
	name	wet_cobbles
	part	sphere union 0   -0.28 0 0   0 0 0   1 1 1   0.4 0 0   0
	part	sphere union 0    0.28 0 0   0 0 0   1 1 1   0.4 0 0   0
}

standard_object
{
	name		obj_wet_cobbles
	geometry	wet_cobbles
	material	mat_wet
	position	0 0 0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

**`coated_material` has no per-substrate porosity knob to tune** — the
darkening it produces is a fixed function of the substrate's own
reflectance and the coat's IOR (the internal-reflection recycling
series), not an authored exponent.  The table below is still useful
INTUITION for how much a real substrate should visually darken when
wet, even though there is currently no `k`-shaped parameter on the
coated route to dial it with:

| substrate | how much it should darken when wet |
|---|---|
| glazed tile, sealed concrete, painted metal, varnished wood | almost none — what changes is almost entirely the coat |
| fired brick, dressed stone, cobble | moderate |
| unsealed concrete, plaster, dry soil, unglazed terracotta | strong |
| cloth, canvas, raw wood | strong |

A sealed, non-porous substrate barely darkens in reality — what changes
when you wet glazed tile is almost entirely the coat, not the substrate
colour.  If a scene genuinely needs to ART-DIRECT the darkening amount
independent of the coat physics, the Phase-1 hand recipe (an
`expression_painter` mixing the base colour toward `pow(base_rgb, k)`
under the damp mask, feeding a `polished_material`'s `reflectance`
instead of a `coated_material` wrap) is still valid RISE and still the
only route with an explicit, tunable exponent — just be aware it is a
DIFFERENT material shape from what `add_wetness` now emits, with the
sec 6.9 caveats that come with `polished_material`'s own dry-NEE gap.

**Deep or pooled water wants a real transmittance tint, not a
uniform-color guess.**  `colors/water_absorption.spectra` (a measured
Pope & Fry 1997 absorption table, pre-converted to a `pow(tau,
distance)` transmittance base) gives spectrally correct depth tint —
but only **under a spectral rasterizer**; `pathtracing_pel_rasterizer`
broadcasts the file's one 555 nm sample and the water comes out grey.
For the default RGB path, tint by hand with three explicit per-channel
`tau` numbers instead, red attenuating fastest — the idiom already
shipping at `tidepools.RISEscene:395`: `dielectric_material { tau 0.85
0.92 0.95  ior 1.33  scattering 5000.0 }`.  (`ior 1.333  scattering
1000000` — a flatter, more delta-mirror surface for a still, deep pool
— is `WETNESS_COAT_DESIGN.md` §6.7's own illustrative example, not a
shipped scene; don't cite it as one.)

**A wet PT render will not match a wet BDPT or VCM render of the same
scene.**  `occlusion()` and `curv` read their neutral fallback on parts
of BDPT/VCM/MLT transport (those integrators omit the derivative/signal
records PT carries), so the pooling term collapses and the surface
reads patchily drier wherever a non-PT strategy contributed — and
`auto_rasterizer` can route there without you choosing a non-PT
integrator by name.  Validate a wet-highlight render's numbers under PT
with `oidn_denoise FALSE`; OIDN is measured to inflate exactly the kind
of sharp, near-deterministic highlight a pooled coat produces.

## Cloth and fabric — a sheen lobe over a weave-shaped substrate

A cushion, a curtain, a jacket, upholstery, bedding: the single most
common way these come out wrong is a `lambertian_material` with a
cloth-coloured albedo, which reads as painted cardboard no matter how
good the colour is.  Cloth has two things a diffuse surface does not.
**A sheen lobe** — a bright grazing halo at the silhouette, from light
scattering off the fuzz and the fibre ends — and **a substrate whose own
highlight follows the weave**.  `fabric_material` is the chunk: an
energy-compensated Charlie sheen over a restricted substrate, evaluated
as the COMBINED response (so NEE and BDPT/VCM connections see the fabric,
not the bare base) and subtracting the sheen's energy from the base, so a
white fabric never returns more light than it receives at grazing.

**Pick a `fabric` preset and bind a `base`; everything else has a
calibrated default.**  The preset seeds `sheen_roughness` and, for
`velvet`, a dark sheen colour.

**The trap: a preset CANNOT configure the substrate.**  `fabric_material`
holds a *reference* to an already-constructed base material — it can
neither retype nor re-parameterise it.  So `fabric satin` bound over a
Lambertian gives **chalk with a faint sheen** plus a warning, because the
tight, directional, anisotropic highlight that *is* satin lives in a
substrate the chunk cannot reach.  Pair each preset with the substrate it
was calibrated for:

| `fabric` | substrate to author yourself |
|---|---|
| `cotton` / `linen` / `wool` | `orennayar_material`, `roughness` (sigma) 0.4 / 0.5 / 0.6 |
| `denim` / `silk` / `satin` | a **`weave_material`** carrying the same-named preset (see below) ‡ |
| `velvet` | a dark `lambertian_material` — a pile, not a weave, so **no anisotropy at all** |

**‡ Those three changed.** They used to want an anisotropic `ggx_material`
(`alphax`/`alphay` 0.34-0.22 / 0.30-0.10 / 0.34-0.06, with the † trap
below).  That composition was measured and it reads as **brushed metal**:
95-99 % of the substrate's anisotropy survives the sheen, so the sheen is
not the problem — a single elliptical lobe with a painted rotation field
simply has no **pattern scale**, no discrete floats with their own
orientation and mutual shadowing.  `weave_material` is that pattern
scale: two thread families, warp and weft, each with their own direction,
dye and pair of fibre lobes, mixed by a weave DRAFT.  Bind one as
`fabric_material`'s `base` (fuzz over weave is the physical stack), or
just call `make_fabric`, which now mints one.  The GGX numbers are still
in the preset table as the documented fallback and the composition still
builds; it just warns.

**† The single most likely way to hand-author a silent black satin.**
`ggx_material.fresnel_mode` defaults to **`conductor`**, and `rs`
("Specular reflectance / F0") is a **required colour-painter reference
resolved BY NAME** — `rs 0.04 0.04 0.04` looks up a painter with that
literal name, finds none, and fails the material outright.  A ggx base
carrying only `rd`/`alphax`/`alphay` therefore gets `rs` unset, which
resolves to the built-in `none` painter — **black** — under conductor
Fresnel: **no dielectric specular at all**, i.e. no highlight, which is
the entire reason ggx was chosen for those three.  So a hand-authored ggx
fabric substrate needs BOTH `fresnel_mode schlick_f0` AND an `rs` bound
to a ~0.04 dielectric-F0 `uniformcolor_painter` you declare yourself.

**`sheen_roughness` is CLAMPED to `[0.04, 1]`, and the floor is real.**
Below about 0.035 the Charlie lobe's baked directional-albedo table
exceeds 1 near grazing and the base-energy subtraction would go negative,
so the chunk clamps rather than letting a fabric emit more light than it
receives.  Note that 0.04 is *tighter* than `sheen_material`'s own 1e-3
floor — a value that was legal on the old chunk is not necessarily legal
here.  Hand-typing a tighter velvet or satin than the preset (0.08 /
0.12) therefore does nothing below the floor; if you want a narrower
highlight than velvet's, the knob to reach for is the SUBSTRATE's
roughness, not the sheen's.

**`weave_rotation` steers the SUBSTRATE's frame, not the sheen lobe**
(which is isotropic and unaffected).  Radians.  Paint it to make the
grain change across a seam follow the yarn; `0` is a bit-exact no-op.
When the substrate is a `weave_material`, put the angle on the WEAVE's
own `weave_rotation` instead and leave this one unwritten — both are
rotations about the same normal, so writing it in two places ADDS them
and turns the yarn twice.

**`weave_material` — the DRAFT, and the two slots that decide whether it
reads.**  `weave plain|twill_2_1|twill_3_1|satin_5|custom` picks which
family is on top per cell (a 3/1 twill is denim's wale; a 5-harness satin
is silk's and satin's float), and `fabric denim|silk|satin|linen|custom`
seeds every other slot including the draft.  Two things need your
attention and nothing else does:

- **`weave_scale`** — cells per unit of surface UV, and the one slot no
  preset can get right, because it depends on your geometry's UV scale
  and your framing.  Too coarse and the cloth reads as tiles; too fine
  and the material's own footprint fade takes over and the surface
  becomes its own mean coverage, which is correct but structureless.
  Aim for roughly one cell per four to eight pixels in the frame you
  care about, and expect to try a value and look.
- **`warp_color` / `weft_color`** — the two DYES, and the reason a weave
  looks like cloth.  Give the families DIFFERENT tones: denim IS an
  indigo warp floating over an undyed weft.  They tint the volume lobe
  only, so a coloured fabric correctly keeps a white highlight.

Set `transmission thin` for a backlit, see-through cloth (a sheer curtain,
a lampshade) — it adds a delta lobe glowing straight through the open gaps
(weighted by `gap`, which you can also spell `sheer`) and a Lambertian lobe
glowing through the yarn itself (`warp_transmit`/`weft_transmit`, [0,1]);
default `transmission none` is reflection-only and `gap` just darkens, as
before. `linen`/`silk`/`satin` default to `thin`; `denim`/`custom` default
to `none`.

Parses and renders today — the whole triad, in emission order
(declare-before-use is not a style choice here: `base` and `rs` resolve
out of already-registered managers, so a forward reference fails):

```
uniformcolor_painter
{
	name			dye_cushion
	color			0.32 0.10 0.16
	colorspace		Rec709RGB_Linear
}

# The dielectric F0 the ggx substrate's `rs` MUST name (see † above).
uniformcolor_painter
{
	name			fabric_f0
	color			0.04 0.04 0.04
	colorspace		Rec709RGB_Linear
}

# The weave angle, in RADIANS -- 45 deg here.  A constant; make it a
# painted field and the float direction follows the yarn across a seam.
scalar_painter
{
	name			weave_angle
	value			0.7853981633974483
}

# The SUBSTRATE.  satin's calibrated anisotropy ratio: this is where the
# fabric's directional highlight lives, because the sheen lobe is
# strictly isotropic.
ggx_material
{
	name			cushion_base
	rd				dye_cushion
	rs				fabric_f0
	alphax			0.34
	alphay			0.06
	fresnel_mode	schlick_f0
}

# The FABRIC.  `sheen_color` / `sheen_roughness` deliberately omitted --
# the `fabric satin` preset seeds both, so retuning the preset later
# moves this material with it.
fabric_material
{
	name			cushion_fabric
	fabric			satin
	base			cushion_base
	weave_rotation	weave_angle
}
```

If you would rather not hand-type any of that, **`make_fabric`** — zero
required arguments — does the whole conversion in one call, one
headVersion bump, one undo step.  It takes the most prominent convertible
material, infers the fabric from the object's own name where that is
unambiguous (`denim_jacket` → denim; `cushion` names no fabric, so it
falls back to cotton and SAYS so — pass `fabric` to be explicit), and
**mints the substrate** when the bound base is not already the preset's
class: the weave/orennayar/lambertian base carrying the preset's numbers,
the weave painter, and the `fabric_material` itself —
then moves every bound object onto the wrapper.  Your colour painter is
**re-homed**, not re-authored, so a texture or expression graph survives
untouched.  The original chunk is never edited, but after a mint nothing
references it any more (the result says so as `originalNowUnreferenced`);
when the base already matches the preset's class it does a pure wrap
instead.  It refuses — changing nothing — on an already-fabric material,
on a dielectric / emissive / hair / BSSRDF / already-coated base, on a
material carrying none of `reflectance`/`base_color`/`rd` to re-home, and
on planar-only geometry under a grazing-halo preset (velvet/satin/silk
need a silhouette; cotton, linen, denim and wool read fine on a flat).

**Wet fabric is not composable yet.**  `make_fabric` refuses on a
material `add_wetness` has already coated, and vice versa —
`coated_material` does not yet accept a `fabric_material` substrate.
Pick one.

## A one-call route to a wired varied material

The four starters above are hand-typed, one painter and one material at
a time.  `insert_material_scaffold` gets a comparable richly-varied
material -- base colour AND microsurface both bound to a real painter --
in one call instead of a multi-chunk hand-wired graph:
`insert_material_scaffold {family:"aged_bronze", name:"urn1",
tone:"0.35 0.22 0.12", wear:0.5, scale:2.5}` expands into a
`cooktorrance_material` (`tmpl_urn1_mat`) with `rd` bound to a
reaction-diffusion patina field and `facets` bound to a spatially-varying
scalar field.  Every generated `tmpl_*` chunk is an ordinary chunk like
any other -- `propose_patch`/`remove_chunk` retune or rebind it exactly
as they would a hand-authored one.

## Colors vs physical scalars — why `scalar_painter` matters

RISE has TWO painter pipes.  `IPainter` is the COLOR pipe (reflectance,
emission, tau tints): its values pass through colorspace conversion and
spectral uplift.  `IScalarPainter` is the PHYSICAL-SCALAR pipe (IOR,
roughness, scattering/absorption coefficients, phase asymmetry): values
are raw magnitudes, NEVER color-converted or uplifted.  Binding a color
painter into a scalar slot used to silently mangle values in spectral
renders (e.g. `scattering 1000000` clamped to ~1); today the parser
REFUSES it with a per-parameter diagnostic at derive time — heed it
rather than guessing.  Route by physical meaning: tinted
attenuation/reflectance/emission -> color painter; coefficient
with units -> `scalar_painter` (forms: `value <x>`, `values <r g b>`,
`file <spd>`, `sellmeier ...`).  Inline numbers in scalar slots (`ior
1.5`) are already scalar-safe.

Note that `read_schema` cannot tell the two apart on its own: a colour
slot and a scalar slot BOTH report `references:["painter"]`, so read the
parameter's `description`.  For a scalar that must VARY across the surface
the shortest form is `scalar_painter { expression <body> }` (worked above);
`scalar_painter { painter <name> channel R }`, `{ function2d ... }` and
`{ texture ... }` are the other three varying forms — see
`read_skill {name:"procedural-textures"}` for all four and the sub-trap
that catches people.

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
