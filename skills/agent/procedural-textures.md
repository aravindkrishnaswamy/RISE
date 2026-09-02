# Procedural Textures
> hook: Read before authoring any material or texture -- how a surface stops being a flat colour (wood, stone, marble, rust, brushed metal, cloth, worn paint), how roughness and other physical scalars VARY across a surface, and which painter chunk -- noise, expression, ramp, image -- to reach for.

On a bare build prompt, state a one-line design plan -- materials AND forms -- in the reply and proceed; do not ask about style.

## When a flat colour is the wrong answer

`uniformcolor_painter` has ZERO spatial variation.  It is correct for
small parts, for test scenes, and as an operand of something else.  It is
the WRONG answer for a hero surface, and a scene whose table top, wall,
floor, ground and props are each one flat colour is the single most
recognisable amateur-render signature -- the render reads as untextured
geometry, not as a room.

Reach for a spatially-varying painter whenever EITHER holds:

- **The request names a material with visible structure.**  wood, oak,
  walnut, plank, stone, granite, marble, concrete, brick, rust,
  corrosion, patina, wear, scuffs, grime, dirt, weathered, water, waves,
  clouds, fog, fabric, leather, hide, moss, lichen, hammered, brushed,
  pitted.  Every one of those words is a texture cue, and answering it
  with a flat colour ignores the request.
- **The surface is larger than a trinket.**  A table top, a floor, a
  wall, a ground plane, a large vessel body -- anything that occupies a
  meaningful fraction of the frame -- needs variation, even when the
  prompt did not ask for it.  Two painters and one extra chunk is a
  cheap edit; a flat hero surface is not recoverable by lighting.

RISE ships more than three dozen painter chunk kinds.
`read_schema {category:"painter"}` is a cheap one-line-per-kind listing of
all of them; the decision map below is the shortcut.

## Decision map: surface intent -> painter family

| Intent | Reach for | Notes |
|---|---|---|
| Wood grain, marble veins, mottled stone | `perlin3d_painter` or `turbulence3d_painter` | turbulence is the ridged/veiny one, perlin the smooth swell.  NEST them (below) for coarse-plus-fine grain. |
| Cracks, crazing, dried mud, cell walls | `worley3d_painter` with `output f2-f1` | `f2-f1` is ~0 on the cell BOUNDARIES. |
| Pebbles, aggregate, leather, hammered metal, wear patches | `worley3d_painter` with `output f1` | `f1` is 0 at cell CENTRES -- blobby cells. |
| Organic spots, stripes, labyrinths (hide, coral, lichen) | `reactiondiffusion3d_painter` | `feed` + `kill` select which pattern; keep `grid_size` and `iterations` modest, the setup is a real simulation. |
| Brushed metal, wood fibre, fur flow, scratch fields | `gabor3d_painter` | The only DIRECTIONAL noise -- `orientation` + `frequency`. |
| Clouds, foam, moss | `perlinworley3d_painter` | Billowy but clumpy. |
| Smoke, marbled paper, whorls | `curlnoise3d_painter` | Divergence-free -- filaments, not blobs. |
| Iridescence: soap film, beetle shell, oil slick | `iridescent_painter` | View-angle blend, NOT a physical thin film.  For real heat-tint / anodizing use `ggx_material` with `fresnel_mode thinfilm`. |
| Water surface | `gerstnerwave_painter` | Best as a `displaced_geometry` displacement (real waves) with the colour reading trough-vs-crest. |
| Break up the regularity of any of the above | `domainwarp3d_painter` | Noise whose input coordinates are themselves noise-displaced.  The cheapest way to stop fBm reading as "noise" -- and by itself the best marble. |
| Art-directed cells (mosaic, tile, terrazzo) | `voronoi2d_painter` / `voronoi3d_painter` | Each `gen` line seeds ONE cell with its OWN painter -- placed, not random. |
| Incandescent / flame / hot metal colour | `blackbody_painter` | A temperature in Kelvin beats a guessed RGB triple. |
| Combine, tint, or mask two of the above | `blend_painter`, `channel_painter` | See "Composition" below. |
| A pattern you can write as maths, in 3D | `expression_painter` (colour) / `scalar_painter { expression ... }` (physical scalar) | The texture-expression VM: one string over `u v P Po N fw time`, with noise builtins.  **The default answer for anything the fixed painters cannot say**, and the only route to spatially-varying roughness that is not an adapter chain -- see "The expression VM" below. |
| A pattern over UV only, as maths | `expression_function2d` | The older UV-only evaluator (`u`, `v`, no noise builtins).  Still the right choice for a `displaced_geometry` `function` slot and the `scalar_painter { function2d ... }` bridge, which take an IFunction2D. |
| Turn a grey field into real colour (terrain bands, patina, rust-to-metal) | `ramp_painter` | Multi-stop colour ramp driven by any painter's channel.  The universal scalar -> colour remap; see "The composition boundary" below. |
| Drive a PHYSICAL SCALAR from any colour painter you already have | `scalar_painter { painter <name> channel <R\|G\|B\|A> scale <s> bias <b> }` | The any-painter -> scalar bridge: binds ANY of the painter kinds above (a worley field, an image, an expression) to roughness / IOR / scattering. |
| Retile, rotate, or reproject an existing painter without rebuilding it | `mapping_painter { source <name> projection uv\|world\|object\|triplanar scale rotate translate }` | Wraps ANY painter and transforms the DOMAIN it is evaluated at before delegating -- the general-purpose scale/rotate/offset tool.  `triplanar` is also how a UV-only painter (a `png_painter`, `checker_painter`, ...) gets projected onto UV-less geometry (an `sdf_geometry`, a heavily displaced mesh) via three axis-blended samples. |
| Kill the visible repeat of a small tiling photo/scan (bark, plaster, rust, fabric) | `stochastic_tile_painter { source <name> tile_scale <n> mean <r g b> }` | Hex-tiles `source` with histogram-preserving blending (Heitz & Neyret 2018) -- one small photo covers an unbounded area with no grid repetition.  `mean` is AUTHOR-SUPPLIED (match the source's actual average value); there is no auto-estimation.  UV only -- wrap in `mapping_painter { projection triplanar }` for UV-less geometry. |
| Scatter discrete elements (rivets, leaves, scratches, stains, decals) that noise cannot produce | `scatter_painter { source <stamp> background <name> cell_scale stamp_scale jitter_position jitter_rotation jitter_scale probability }` | Texture-bombing: stamps `source` on a jittered lattice over `background`, alpha-gated (an RGBA cutout stamp shows background through its transparent pixels).  `stamp_scale * (1 + jitter_scale)` is capped at sqrt(2) -- the parser rejects an oversized stamp. |

`checker_painter`, `lines_painter` and `mandelbrot_painter` also exist and
are spatially varying, but they are deliberately synthetic: right for
test / reference surfaces and tiled floors, wrong when the ask is
material realism.

## 2D (UV) vs 3D (solid): pick the domain first

Every painter is one or the other, and the chunk name tells you:

- **3D / solid** (`perlin3d`, `turbulence3d`, `simplex3d`, `wavelet3d`,
  `worley3d`, `perlinworley3d`, `gabor3d`, `curlnoise3d`,
  `domainwarp3d`, `reactiondiffusion3d`, `sdf3d`, `voronoi3d`) is
  evaluated at the **WORLD-SPACE intersection point** -- with ONE
  historical exception: `voronoi3d_painter` has always sampled OBJECT
  space instead (an instanced/transformed object keeps the same cell
  pattern).  Its `space` field (P2.5, doc 88) makes that explicit;
  `space world` opts it into the same world-space convention as the
  rest of this family -- but flipping it RE-INTERPRETS every authored
  `gen`/border position against the other domain, since the same raw
  x/y/z you wrote for `object` space now gets read as a world
  coordinate, so treat a `space` change as a re-authoring pass on the
  generator coordinates, not a free toggle.  Needs no UVs,
  shows no seams, and looks like the object was CARVED OUT of the
  material.  **Default to these** for wood, stone, marble, metal.
  Two consequences of "world space":
  - Moving an object slides it through a fixed world field, so its
    pattern changes.  Usually a feature (no two props repeat); use
    `shift` when you need to re-register one.
  - `scale` is a per-axis FREQUENCY on the world coordinate, so it is
    tied to your scene's units, not to the object's size.  Unequal
    components stretch the pattern along an axis -- that is how you get
    grain or banding instead of blobs.
- **2D / UV** (`perlin2d`, `checker`, `lines`, `mandelbrot`,
  `voronoi2d`, `gerstnerwave`, `expression_function2d`, the
  `polynomial_/composite_function2d` painters, and every image loader:
  `png_painter` / `jpg_painter` / `hdr_painter` / `exr_painter` /
  `tiff_painter`) is evaluated at the surface UV.  Right when the
  pattern genuinely belongs to the SURFACE -- a label, a decal, a woven
  cloth, a wave field on a water plane, anything authored against a UV
  layout.  It stretches with UV distortion and can show seams.

`expression_painter` and `scalar_painter { expression ... }` are **3D**:
their bodies see `P` (world position) and `Po` (object position) as well
as `u`, `v`.  `expression_function2d` is **2D** and sees only `u`, `v` --
that is the whole difference between the two expression surfaces, and
getting it backwards is the trap below.

## The expression VM -- one string instead of a painter graph

`expression_painter` (colour pipe) and `scalar_painter { expression ... }`
(physical-scalar pipe) run the same little language.  Reach for it the
moment the fixed painters cannot say the thing you mean -- a contrast
curve, a threshold, a mix of two noises, a scalar that varies -- because
it costs ONE chunk instead of a graph of them.

The body sees `u`, `v`, `P` (world position, a `vec3`), `Po` (object
position), `N` (shading normal), `fw` (world-space filter-width
estimate; real on primary hits against mesh geometry, 0.0 -- an honest
"point sample" -- on secondary bounces and non-mesh geometry), and
`time`.  `fbm`/`turbulence`/`ridged` use `fw` automatically to fade out
octaves the sample footprint can't resolve, cutting shimmer on
distant/grazing procedural surfaces.  Watch domain scaling: `fw` is
passed through UNSCALED, so scaling the position argument (e.g.
`fbm(P*10, ...)`) scales the effective filter width the fade sees by
that same factor -- the standard frequency idiom silently shifts the
fade threshold, so retune the fade expectations (or divide `fw`
accordingly) whenever you scale `P`.  Builtins: `perlin`, `fbm(p, octaves, gain,
lacunarity)`, `turbulence`, `ridged`, `worley_f1/f2/f2f1/id(p, jitter)`,
`cellhash`, `ramp(t, pos0,val0, ...)`, plus `mix/clamp/smoothstep/step/
select/pow/abs/floor/frac/min/max/sin/cos/...` and the vec3 ops
`vec3()`, `.x/.y/.z`, `dot`, `cross`, `length`, `normalize`.

The body also sees `curv`/`curvR` (surface curvature at the hit: positive
convex, negative concave, 0 flat) for geometry-driven wear and grime
masks -- `read_skill {name:"materials-and-media-basics"}`'s patina
section has the sign convention and a full worked, execution-validated
example.

Three authoring rules, and the first is a contract, not a style note:

1. **Every art-directable number goes in a `param` with a range, never a
   literal in the body.**  `param ring_scale 4.0 min 0.5 max 20 step 0.1
   label "Ring density"` -- the compiler ignores the metadata and the
   property panel turns it into a slider, so a human retunes your texture
   by scrubbing a named knob and never reads the expression.  A body full
   of bare constants is not editable by anyone but you.
2. **Use `def` for stages, not for constants.**  `def` is a let-binding
   evaluated in order (`def warp ...`, `def grain ...`, `def wear ...`);
   naming the stages is what makes a long body readable, and each def is
   its own editable row in the inspector.  (Evaluated-stage previews --
   seeing the actual noise field each `def` produces, not just its text --
   are Phase-2 P5.3, not shipped yet.)
3. **`seed` is a free per-instance knob.**  It is auto-registered as a
   named scalar constant, so `perlin(P + vec3(seed*17, seed*31, seed*13))`
   gives two objects sharing one painter chunk different noise -- change
   one number, not the body.

Domain warping needs no builtin -- it is composition:
`fbm(P + amp*vec3(fbm(P+o1,4,0.5,2), fbm(P+o2,4,0.5,2), fbm(P+o3,4,0.5,2)), 4, 0.5, 2)`.

**Raw `fbm`/`perlin` do NOT span [0,1]** (roughly -0.4 .. 0.4 measured),
so remap before you mix: `clamp(n*contrast + 0.5, 0, 1)`.  An unclamped
`mix(lo, hi, fbm(...))` extrapolates past both ends -- which for a
roughness slot can walk toward zero and silently turn your surface into a
mirror.

### The composition boundary: field in the expression, colour in the ramp

Expressions compute scalar **fields**.  Colour decisions belong in
`ramp_painter`, which takes a `input <painter>` + `channel`, `>= 2`
`stop <pos> <r> <g> <b>` lines and an `interpolation
linear|constant|smooth`.  Keep to that split and the thing a human most
wants to tweak -- the colours -- always has a stop list to edit and never
requires touching expression text.  Recipe 3 below is the worked form.

## Recipe 1 -- wood grain on a table top

Two things make this read as wood rather than as noise: `scale` is
ANISOTROPIC (high across the plank, low along it, so the grain runs
lengthwise), and the painters are NESTED -- a fine-fibre `perlin3d` is
itself the `colora` of a coarse-ring `perlin3d`, which is how real timber
looks (broad rings modulating fine fibre).  Bind the result to the
material's colour slot like any other painter.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.42 0.47 0.55
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
	location	0 1.7 2.1
	lookat		0 0.2 0
	up			0 1 0
	fov			48.0
}

# Three tones, widest-apart first: the noise only ever lands in the
# MIDDLE of the colora..colorb interval, so the endpoints must be
# further apart than the look you want (see "What these painters
# cannot do").
uniformcolor_painter
{
	name	pnt_oak_pale
	color	0.50 0.29 0.12
}

uniformcolor_painter
{
	name	pnt_oak_mid
	color	0.24 0.12 0.045
}

uniformcolor_painter
{
	name	pnt_oak_dark
	color	0.055 0.022 0.009
}

# Fine fibre: very high frequency ACROSS the plank (x), low ALONG it (z).
perlin3d_painter
{
	name		pnt_fibre
	colora		pnt_oak_pale
	colorb		pnt_oak_mid
	octaves		3
	persistence	0.5
	scale		90 8 2.0
}

# Coarse rings, NESTED: the fibre painter is this one's colora, so the
# broad growth rings modulate the fine fibre instead of replacing it.
perlin3d_painter
{
	name		pnt_grain
	colora		pnt_fibre
	colorb		pnt_oak_dark
	octaves		4
	persistence	0.72
	scale		9 1 0.45
}

# The grain painter binds to base_color exactly like a flat painter would.
pbr_metallic_roughness_material
{
	name		mat_wood
	base_color	pnt_grain
	metallic	0.0
	roughness	0.3
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.3 0.3 0.32
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

box_geometry
{
	name	top
	width	2.6
	height	0.13
	depth	1.5
}

standard_object
{
	name		obj_top
	geometry	top
	material	mat_wood
	position	0 0.2 0
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
	position	0 -0.5 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		2.7
	color		1.0 0.911 0.748
	direction	0.35 0.72 0.6
}
```

## THE ISCALARPAINTER TRAP -- colour slots and physical-scalar slots are different pipes

This is the one mistake that will stop a procedural material from
deriving at all, and `read_schema` alone will not warn you: **every
painter-taking parameter reports the same `references:["painter"]`,
whether it wants a COLOUR painter or a PHYSICAL SCALAR.**  Read the
parameter's `description` -- the scalar ones say so -- or use this rule:

- **COLOUR slots take an `IPainter` chunk** -- any of the painter kinds
  above (more than three dozen; see `read_schema {category:"painter"}` for
  the live count).  `reflectance`, `base_color`, `rd`, `rs`, `ref`,
  `emissive`, `colora`/`colorb`/`mask`, a rasterizer `radiance_map`.
  These go through colourspace conversion and, in the spectral
  renderers, Jakob-Hanika spectral uplift.
- **PHYSICAL-SCALAR slots take a `scalar_painter` name, or an inline
  number** -- and nothing else.  `roughness`, `alpha`, `alphax`,
  `alphay`, `facets`, `ior`, `extinction`, `tau`, `scattering`,
  `absorption`, `film_ior`, `film_thickness`, phase asymmetry.  These
  are raw magnitudes: NEVER colour-converted, never spectrally
  uplifted, which is the whole point (`scattering 1000000` survives).

Binding a colour painter into a scalar slot is REFUSED, loudly, at
derive time -- it is not silently mangled any more:

```
ggx_material `mat_x`: parameter `alphax` is bound to `IPainter` chunk `pnt_wear`;
this slot now requires a `scalar_painter` (physical scalar, no JH spectral
uplift).  See docs/ISCALARPAINTER_REFACTOR.md.
```

**So how do you get spatially-varying ROUGHNESS?**  FOUR of
`scalar_painter`'s twelve forms vary across a surface; the other eight
(`value`, `values`, `file`, `sellmeier`, `polynomial`, `function1d`,
`base`, `multiply`) are spatially constant:

1. `scalar_painter { expression <body> }` -- the texture-expression VM on
   the scalar pipe, over the full 3D context (`u v P Po N fw`), with the
   noise builtins.  No colourspace, no uplift, by construction.  **This is
   the route to take**, and it is ONE chunk:

   ```
   scalar_painter
   {
       name        sp_wear
       param       cell_freq 3.5 min 0.5 max 12 step 0.25 label "Cell frequency"
       param       rough_lo 0.04 min 0 max 1 step 0.01 label "Polished"
       param       rough_hi 0.55 min 0 max 1 step 0.01 label "Weathered"
       def         f1 worley_f1(P*cell_freq, 1.0)
       expression  mix(rough_lo, rough_hi, clamp(f1, 0, 1))
   }
   ```
2. `scalar_painter { painter <name> channel <R|G|B|A> scale <s> bias <b> }`
   -- the any-painter bridge: drive the scalar from ANY colour painter you
   already declared (a worley field, a `domainwarp3d`, an
   `expression_painter`), as `out = bias + scale * channel(source)`.  It
   reads a POST-colourspace value, which is fine for a procedural mask and
   is not a spectral-fidelity path.
3. `scalar_painter { function2d <name> scale <s> bias <b> }` -- wraps a
   named `IFunction2D` and evaluates it at the surface UV, as
   `out = bias + scale * f(u,v)`.  Author the field with
   `expression_function2d`, or use a `polynomial_function2d_painter` /
   `composite_function2d_painter`.  Right when the field genuinely belongs
   to the UV layout (brush grooves running along the tangent frame).
4. `scalar_painter { texture <image painter> channel <R|G|B> scale <s>
   bias <b> }` -- samples a declared `png_painter` / `jpg_painter` /
   `hdr_painter` / `exr_painter` / `tiff_painter` at the surface UV, with
   no colourspace conversion.  Use it when you actually have a
   roughness map on disk.

**If you would rather not hand-author it at all, call
`vary_material`** -- zero required arguments; it finds the material whose
microsurface is still a bare number, adds exactly the chunk above banded
around that number, and rebinds the slot, in one call and one undo step.
Its output is the idiom to copy.

**The trap inside the trap** (verified by render, not by reading): a 3D
SOLID painter is *accepted* as a `function2d` source -- every painter is
registered in the function-2D index -- but it collapses to a
**spatially CONSTANT** value.  The bridge calls `Evaluate(u,v)`, which
synthesises an intersection whose position is the origin, and a 3D
painter reads that position, not the UV.  So
`scalar_painter { function2d <a perlin3d/worley3d/domainwarp3d name> }`
derives clean, renders clean, and produces a flat number.  Feed the
`function2d` form a **UV-domain** source only.  (The same applies to
`function2d_painter`, the greyscale-colour wrapper.)

## Recipe 2 -- marble, with a polished/honed finish

`domainwarp3d_painter` on its own is the best marble in the set: the
warped coordinates give soft, wandering veins that no plain fBm
produces.  The same slab also carries a spatially-varying roughness
through the scalar pipe -- alternating polished and honed bands from an
`expression_function2d`, mapped into `[0.015, 0.515]` by the
`scale`/`bias` affine.  Look for the veins first (obvious) and the
banded sheen second (subtle -- specular width needs a directional key
to read at all).

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.40 0.44 0.52
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
	location	0 1.9 2.3
	lookat		0 0.2 0
	up			0 1 0
	fov			48.0
}

uniformcolor_painter
{
	name	pnt_vein
	color	0.055 0.05 0.045
}

uniformcolor_painter
{
	name	pnt_stone
	color	0.60 0.58 0.53
}

# COLOUR pipe: warped noise -> wandering veins, in world space.
domainwarp3d_painter
{
	name			pnt_marble
	colora			pnt_stone
	colorb			pnt_vein
	octaves			4
	persistence		0.62
	warp_amplitude	3.0
	warp_levels		2
	scale			3.5 3.5 3.5
}

# SCALAR pipe, step 1: a UV field.  `u`, `v` are the surface UV;
# `param` declares a constant, `def` a let-binding, `expr` the value.
expression_function2d
{
	name	fn_polish
	param	bands 6.0
	def		s sin( u * bands * tau )
	expr	smoothstep( -0.35, 0.35, s )
}

# SCALAR pipe, step 2: wrap it as a PHYSICAL scalar.  out = bias + scale * f,
# so roughness sweeps 0.015 (polished) .. 0.515 (honed).  A colour painter
# would be REFUSED in the alphax/alphay slots below.
scalar_painter
{
	name		sp_polish
	function2d	fn_polish
	scale		0.5
	bias		0.015
}

uniformcolor_painter
{
	name	pnt_marble_spec
	color	0.5 0.5 0.5
}

ggx_material
{
	name		mat_marble
	rd			pnt_marble
	rs			pnt_marble_spec
	alphax		sp_polish
	alphay		sp_polish
	ior			1.55
	extinction	0.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.28 0.28 0.3
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

box_geometry
{
	name	slab
	width	2.4
	height	0.16
	depth	1.4
}

standard_object
{
	name		obj_slab
	geometry	slab
	material	mat_marble
	position	0 0.2 0
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
	position	0 -0.5 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		2.9
	color		1.0 1.0 0.933
	direction	0.25 0.45 0.86
}
```

## Recipe 3 -- rusted iron: one expression field, two consumers

The composition-boundary idiom end to end.  ONE `expression_painter`
computes a scalar corrosion field (fbm, domain-warped by a second fbm, all
in world space).  Nothing about that chunk is a colour decision.  Then
`ramp_painter` turns the field into rust-through-to-metal colour with four
editable stops, and a `scalar_painter { painter ... }` bridge feeds the
SAME field into GGX roughness -- so the surface is rough exactly where it
is rusty, which is what makes it read as corrosion rather than as a
painted pattern.  Two consumers, one field, no second noise to keep in
sync.

```rise
RISE ASCII SCENE 7

uniformcolor_painter
{
	name	pnt_sky
	color	0.40 0.45 0.55
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
	location	0 1.5 3.4
	lookat		0 0.15 0
	up			0 1 0
	fov			42.0
}

# THE FIELD.  Scalar-typed (it broadcasts to grey on the colour pipe), and
# deliberately free of any colour decision.  Every knob is a `param` with a
# range, so the panel renders sliders and propose_patch retunes it by name.
expression_painter
{
	name		pnt_corrosion
	param		freq 2.6 min 0.5 max 12 step 0.1 label "Corrosion frequency"
	param		warp 0.55 min 0.0 max 2.0 step 0.05 label "Warp amount"
	param		contrast 1.9 min 0.5 max 4.0 step 0.05 label "Corrosion contrast"
	seed		13.0
	def			o vec3(seed, seed*1.7, seed*2.3)
	def			w vec3(fbm(P*freq+o, 3, 0.5, 2.0), fbm(P*freq-o, 3, 0.5, 2.0), fbm(P*freq+o*2.0, 3, 0.5, 2.0))
	def			n fbm(P*freq + w*warp, 5, 0.5, 2.0)
	expr		clamp(n*contrast + 0.5, 0, 1)
}

# CONSUMER 1 -- colour.  Four stops: sound metal, darkened metal, active
# rust, powdery bloom.  This is where a human edits the look.
ramp_painter
{
	name			pnt_rust
	input			pnt_corrosion
	channel			R
	interpolation	smooth
	stop			0.00  0.30 0.31 0.33
	stop			0.42  0.20 0.16 0.13
	stop			0.68  0.42 0.17 0.06
	stop			1.00  0.55 0.32 0.16
	color_space		Rec709RGB_Linear
}

# CONSUMER 2 -- the PHYSICAL SCALAR, off the SAME field, through the
# any-painter bridge: out = bias + scale * R, so roughness sweeps
# 0.08 (sound metal) .. 0.62 (powdery rust).
scalar_painter
{
	name		sp_rust_rough
	painter		pnt_corrosion
	channel		R
	scale		0.54
	bias		0.08
}

uniformcolor_painter
{
	name	pnt_iron_spec
	color	0.45 0.44 0.42
}

ggx_material
{
	name		mat_rusted_iron
	rd			pnt_rust
	rs			pnt_iron_spec
	alphax		sp_rust_rough
	alphay		sp_rust_rough
	ior			2.6
	extinction	3.0
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

box_geometry
{
	name	plate
	width	2.2
	height	0.22
	depth	1.3
}

standard_object
{
	name		obj_plate
	geometry	plate
	material	mat_rusted_iron
	position	0 0.2 0
	orientation	0 22 0
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
	position	0 -0.4 0
	orientation	-90 0 0
}

directional_light
{
	name		key
	power		3.0
	color		1.0 0.955 0.869
	direction	0.35 0.6 0.75
}
```

## Composition

- **Two colours plus a mask**: `blend_painter { colora colorb mask }`
  computes `colora * mask + colorb * (1 - mask)`, per channel.  So
  **mask 1 selects colora and mask 0 selects colorb** -- the opposite
  of the reading most people assume.  Any painter can be the mask; a
  coloured mask tints as well as blends.  That formula is `mode mix`,
  the default.  `mode multiply|screen|overlay|add` (P2.4, doc 88)
  combines colora and colorb with the named formula FIRST, and the
  mask then interpolates between that combination (mask 1) and colorb
  (mask 0) exactly as before -- e.g. `mode multiply` for a shadow-mask
  AO tint, `mode add` for an emissive glow layered on a base colour.
- **Nesting is usually simpler than blending.**  Every noise painter
  already interpolates `colora -> colorb`, and those two slots take
  PAINTERS, not just flat colours.  Putting one noise painter in
  another's `colora` (Recipe 1) gives multi-scale detail in two chunks,
  with no third mask painter to manage.
- **Wrap, do not replace**: `domainwarp3d_painter` is the standard
  "make this less regular" pass.  Reach for it before adding octaves.
- **One channel of something else**: `channel_painter { source channel }`
  pulls R/G/B/A out of any painter as greyscale -- how a packed
  metallic-roughness image is decomposed.

## What these painters cannot do

The 3D noise painters have **no contrast, gain, or remap control**, and
their output does not span `[0,1]`.  Measured on the wood/marble setups
above: `perlin3d` lands roughly in the middle half of the interval, and
`turbulence3d` clusters LOW (mostly `colora`).  Consequences:

- **Set `colora` and `colorb` further apart than the range you want.**
  A pair of similar browns yields a near-flat surface; near-black
  against a bright tan yields believable wood.
- If a texture renders "flat" or "washed out", that is almost always
  this, not a bug.  Do not chase it with more `octaves` -- separate the
  endpoints, or nest a second painter.
- When you need a genuine contrast curve (`smoothstep`, `pow`,
  thresholds) or a real remap, write the field as an `expression_painter`
  (3D, so it can sit in the same domain as the solid noises) and colourise
  it with `ramp_painter` -- that pair is the contrast and remap control
  the fixed noise painters lack.  `expression_function2d` does the same
  job in the UV domain only.

Also: `octaves` costs render time linearly, and
`reactiondiffusion3d_painter` pays a real simulation at scene-load
(`grid_size^3 * iterations`).  Keep both modest until the look is right.

## colorspace on painters

`uniformcolor_painter` reads `color` as ALREADY-LINEAR Rec.709.  Add
`colorspace sRGB` when the value came from a colour picker or a hex code,
or the material comes out too bright.  On the image painters,
`color_space Rec709RGB_Linear` is the verbatim-store idiom -- use it for
anything that is not colour (normal maps, masks, roughness maps), since
any other setting applies a real conversion and would warp the values.

## Discovery

- `read_schema {category:"painter"}` -- every kind, one line each.
  Cheap; do this before guessing a name.
- `read_schema {keywords:["expression_painter","ramp_painter",
  "scalar_painter","perlin3d_painter","worley3d_painter",
  "blend_painter"]}` -- batch the ones you picked.
  Each parameter carries its meaning and its real parser default.
- `vary_material` -- zero required arguments; makes the most prominent
  bare-number microsurface in the scene vary, in one call.  Its output is
  a worked example of the `param`-with-a-range contract above.
- `add_wear` -- zero required arguments; rewrites the most prominent flat
  colour into the curvature wear composition (edge mask `clamp(curv*k +
  noise, 0, 1)`, crevice mask `clamp(-curv*k + noise, 0, 1)` deepened by
  `occlusion()`), banded around the values already there, and adds the
  matching roughness field -- one call, one undo step.
- Colour-slot vs scalar-slot wiring, material starters, and the glass /
  metal "needs something to reflect" rule live in
  `read_skill {name:"materials-and-media-basics"}` -- read that one for
  the material, this one for what fills its colour slot.
