# Procedural Textures
> hook: Read when a surface should look like a real material -- wood, stone, marble, metal wear, water, cloth -- rather than a flat colour, or when picking a spatially-varying painter.

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

RISE ships 36 painter chunk kinds.  `read_schema {category:"painter"}` is
a cheap one-line-per-kind listing of all of them; the decision map below
is the shortcut.

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
| A pattern you can write as maths | `expression_function2d` | Full expression language over `u`, `v`.  Also the ONLY route to a spatially-varying physical scalar -- see the trap. |

`checker_painter`, `lines_painter` and `mandelbrot_painter` also exist and
are spatially varying, but they are deliberately synthetic: right for
test / reference surfaces and tiled floors, wrong when the ask is
material realism.

## 2D (UV) vs 3D (solid): pick the domain first

Every painter is one or the other, and the chunk name tells you:

- **3D / solid** (`perlin3d`, `turbulence3d`, `simplex3d`, `wavelet3d`,
  `worley3d`, `perlinworley3d`, `gabor3d`, `curlnoise3d`,
  `domainwarp3d`, `reactiondiffusion3d`, `sdf3d`, `voronoi3d`) is
  evaluated at the **WORLD-SPACE intersection point**.  Needs no UVs,
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
	color		1 0.96 0.88
	direction	0.35 0.72 0.6
}
```

## THE ISCALARPAINTER TRAP -- colour slots and physical-scalar slots are different pipes

This is the one mistake that will stop a procedural material from
deriving at all, and `read_schema` alone will not warn you: **every
painter-taking parameter reports the same `references:["painter"]`,
whether it wants a COLOUR painter or a PHYSICAL SCALAR.**  Read the
parameter's `description` -- the scalar ones say so -- or use this rule:

- **COLOUR slots take an `IPainter` chunk** -- any of the 36 painter
  kinds above.  `reflectance`, `base_color`, `rd`, `rs`, `ref`,
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

**So how do you get spatially-varying ROUGHNESS?**  Only two
`scalar_painter` forms vary across a surface at all; the other eight
(`value`, `values`, `file`, `sellmeier`, `polynomial`, `function1d`,
`base`, `multiply`) are spatially constant:

1. `scalar_painter { function2d <name> scale <s> bias <b> }` -- wraps a
   named `IFunction2D` and evaluates it at the surface UV, as
   `out = bias + scale * f(u,v)`.  Author the field with
   `expression_function2d` (full maths over `u`, `v`), or use a
   `polynomial_function2d_painter` / `composite_function2d_painter`.
   **This is the route to take.**
2. `scalar_painter { texture <image painter> channel <R|G|B> scale <s>
   bias <b> }` -- samples a declared `png_painter` / `jpg_painter` /
   `hdr_painter` / `exr_painter` / `tiff_painter` at the surface UV, with
   no colourspace conversion.  Use it when you actually have a
   roughness map on disk.

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
	color		1 1 0.97
	direction	0.25 0.45 0.86
}
```

## Composition

- **Two colours plus a mask**: `blend_painter { colora colorb mask }`
  computes `colora * mask + colorb * (1 - mask)`, per channel.  So
  **mask 1 selects colora and mask 0 selects colorb** -- the opposite
  of the reading most people assume.  Any painter can be the mask; a
  coloured mask tints as well as blends.
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
  thresholds), you need `expression_function2d`, which is UV-domain.

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

- `read_schema {category:"painter"}` -- all 36 kinds, one line each.
  Cheap; do this before guessing a name.
- `read_schema {keywords:["perlin3d_painter","worley3d_painter",
  "blend_painter","scalar_painter"]}` -- batch the ones you picked.
  Each parameter carries its meaning and its real parser default.
- Colour-slot vs scalar-slot wiring, material starters, and the glass /
  metal "needs something to reflect" rule live in
  `read_skill {name:"materials-and-media-basics"}` -- read that one for
  the material, this one for what fills its colour slot.
