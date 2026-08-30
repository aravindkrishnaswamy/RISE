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
	color		1 0.98 0.95
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
	color		1 0.98 0.95
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

`thickness(radius)` is occlusion's sibling for TRANSLUCENCY rather than
grime: it returns `[0,1]` with 1 = thick, so `1 - thickness(0.1)` is a
THIN-region mask — bind it into a subsurface tint or an SSS-style rim
term and a creature's ears, fins, or a leaf's edge light up first, which
a curvature-only edge mask cannot do because curv has no notion of wall
thickness.  Like `occlusion`, it is exact only on the volumetric SDF
family (`sdf_geometry` / `skeleton_geometry`) and reads the neutral 1
(thick) everywhere else — including a heightfield-mode SDF — so an
unsupported geometry stays inert rather than lighting up.

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
