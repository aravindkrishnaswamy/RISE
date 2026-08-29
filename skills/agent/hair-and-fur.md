# Hair and Fur
> hook: Read before growing hair, fur, grass, whiskers, bristle or any fibre coat -- before you pick a strand count, a colour tier, or a styling knob.

Three chunks do all of it.  `hair_material` says what a fibre is made
of, `hair_geometry` grows a groom on a surface, and `hair_guides`
supplies explicit strand SHAPES when the styling knobs cannot.  The
chunk descriptors (`read_schema`) are the parameter reference; this
skill is the judgment -- which knob is the one that matters for the
look you were asked for, and which ones will waste an hour.

## Decide first

| You were asked for | count | length / width | `medulla_ratio` | colour tier |
| --- | --- | --- | --- | --- |
| Human hair (head) | 100k-300k (down to ~90k when guided waves must resolve) | 0.1-0.3 m, `width_root` 0.00009-0.00020 | **0** | Tier 1 melanin |
| Mammal coat / pelt | two layers, 150k + 70k | 0.02-0.07 m, 0.00010 / 0.00019 | 0.8 under, 0.5 guard | Tier 1 melanin |
| Rabbit-soft undercoat, velvet | 200k+ | 0.03 m, 0.00009 | 0.85-0.90 | Tier 1, low melanin |
| Grey / silver / white | any | any | any | **Tier 2 `sigma_a`** |
| Fleece, stylised, "this cream" | 190k | 0.06 m, 0.00024 | 0.72 | **Tier 3 `color`** |
| Bristle, quills, whiskers | 1k-15k, SPARSE | 0.07-0.12 m, 0.0002-0.0014 | 0.25-0.6 | Tier 1 |
| Grass, stalks, seed-heads | 70k-210k per patch | 0.013-0.2 m, 0.0008-0.0016 | 0.15-0.22 | Tier 3 `color` |
| Dandelion pappus, resolved fibres | **fewer** -- 20k not 200k | 0.014 m, 0.00008 | 0.85 | Tier 1, near-zero |

Everything in that table is measured off the three shipped scenes at
the end of this file.  Two rules to carry into every row:

* **Build at REAL SCALE.**  `width_root` defaults to `0.0001` and
  `width_tip` to `0.00003` **scene units** -- those are metres, and
  they are real human hair (0.1 mm root).  Set `scene_options
  scene_unit 1.0` and author the creature in metres.  A groom built on
  a 5-unit "head" gets 0.1 mm fibres on a 5 m skull and renders as
  invisible thread.
* `hair_geometry` **cannot emit and cannot be tessellated** -- it is
  refused as an area light and as a `displaced_geometry` base.  Light a
  groom with ordinary lights.

## The minimal groom

Four required things: a `hair_material` with exactly ONE colour tier
bound, a tessellatable `base_geometry`, a `count`, and a `length`.
Every other parameter has a working default.  Numeric slots on
`hair_material` and on the groom's painter ports accept an inline
number, so a first pass needs no painter chunks at all.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultDirectLighting
}

film
{
	width		96
	height		96
}

pinhole_camera
{
	location	0.00 0.06 0.34
	lookat		0 0.04 0
	up		0 1 0
	fov		34.0
}

# A named painter must be DECLARED BEFORE the chunk that references it.
uniformcolor_painter
{
	name	pnt_env
	color	0.55 0.62 0.78
}

pathtracing_pel_rasterizer
{
	samples			24
	rr_min_depth		8
	radiance_map		pnt_env
	radiance_scale		0.30
	radiance_background	TRUE
	oidn_denoise		FALSE
}

# Tier 1: melanin.  ~1.3 eumelanin is brown-black hair.
hair_material
{
	name		mat_hair
	eumelanin	1.3
	pheomelanin	0.4
}

sphere_geometry
{
	name	scalp
	radius	0.040
}

hair_geometry
{
	name		groom
	base_geometry	scalp
	count		8000
	length		0.045
	gravity		0.55
}

standard_object
{
	name		obj_groom
	geometry	groom
	material	mat_hair
	position	0 0.04 0
}

rect_light
{
	name		key
	center		-0.22 0.30 0.24
	size		0.20 0.20
	facing		0.55 -0.70 -0.45
	color		1.00 0.94 0.86
	exitance	900.0
}
```

The `base_geometry` **does not have to be rendered**.  Bind a
`standard_object` to the groom and not to the base and you get fibres
growing out of nothing -- which is how whiskers emerge from inside a
muzzle and how a fluff-ball forms a shell instead of a solid mass (see
"reads as fibres" below).

## Colour: pick a tier, bind exactly one

`hair_material` refuses zero tiers and refuses two.  Binding both
`color` and `eumelanin` fails the chunk with a diagnostic that names
all three options and counts what you bound.

* **Tier 1 -- `eumelanin` + `pheomelanin`** (either or both count as
  ONE tier).  The physically based default, and the whole human range
  comes out of that pair.  Useful anchors, all from the shipped
  plate: **0.28 / 0.42** golden blond, **~1.3** brown-black, **0.95 eu
  with 4.60 ph** copper (red hair is pheomelanin DOMINATING, not brown
  with a tint), **4.50 / 0.15** jet black, **5.50 / 0.10** the darkest
  on the plate, **0.02 / 0.015** whiskers.  Low melanin is not just
  "lighter" -- it is the regime where light bounces fibre-to-fibre
  inside the mass and the groom glows from within.
* **Tier 2 -- `sigma_a`** (a `scalar_painter` with three `values`, or
  an inline triple).  **This is the ONLY achromatic route.**  Both
  melanin pigments are coloured, so no mixture of them is flat grey:
  silver, grey and white hair must come from `sigma_a`.  `0.150 0.156
  0.175` is un-pigmented keratin, a hair's breadth cool.  `sigma_a 0`
  is the zero-absorption furnace case.
* **Tier 3 -- `color`** takes an artist reflectance swatch and inverts
  it to absorption.  Reach for it when the answer is "this cream" or
  "this green" and not "this chemistry": fleece, grass blades, stylised
  characters.

```text
scalar_painter
{
	name	sig_silver
	values	0.150 0.156 0.175
}

hair_material
{
	name		mat_silver
	sigma_a		sig_silver
	beta_m		0.30
	beta_n		0.30
}
```

`beta_m` (longitudinal roughness) and `beta_n` (azimuthal) both default
to `0.3` and are clamped to `[0.05, 1]`.  `alpha` is the cuticle scale
tilt in DEGREES (default 2.0); `ior` defaults to 1.55.

## The hair-to-fur axis is `medulla_ratio`

One parameter moves a groom from "hair" to "fur".  It defaults to `0`,
is clamped to `[0, 0.95]`, and at `0` it disables the medulla entirely
and reproduces the plain Chiang model exactly -- which is the correct
setting for human hair.

| kappa | what it is |
| --- | --- |
| 0 | human hair: thin, shiny, crisp backlit strands |
| 0.25 | just off pure absorption (dark coils, boar bristle) |
| 0.5 | coarse guard hair |
| 0.72-0.80 | fleece, dense undercoat |
| 0.85-0.90 | rabbit undercoat, pappus -- reads as VELVET, not as strands |

Turning it up splits the TT and TRT lobes into an unscattered and a
medulla-scattered half.  What changes as kappa rises is the SHARPNESS
of transmitted light: crisp individually-resolved backlit strands
become a soft saturated glow.  Two consequences worth knowing before
you tune it:

* **The medulla only touches TRANSMITTED light.**  The R lobe -- the
  surface highlight -- never crosses the fibre interior and is
  untouched by design.  A purely front-lit setup shows almost no
  difference between kappa 0 and kappa 0.9.  **Put a light behind the
  subject before you judge a medulla setting.**
* **Do not read per-groom brightness as an energy check.**  In a
  backlit view the medulla redirects transmitted light toward or away
  from one particular camera; a redistribution over the sphere is not
  a constant over any one direction.
* `medulla_scatter` (sigma_m, default 0.5) at 0 **or negative** silently
  disables the medulla exactly as `medulla_ratio 0` does, with no
  diagnostic.  If fur renders like plain hair, check that sign first.
  `medulla_g` (default 0.4) is the Henyey-Greenstein anisotropy;
  positive is forward-scattering, and the baked table spans +/-0.8.

## Two layers, not one

Real mammal coats are two coats, and **one layer alone reads as felt**.
The recipe, from the shipped rabbit and fox:

* **Undercoat** -- short, fine, dense, heavily medullated (kappa
  0.80-0.88), higher roughness, LIGHTER melanin.  This is the soft
  saturated light-diffusing bulk.
* **Guard coat** -- roughly 2x longer, ~2x thicker, a third to a half
  the count, a THINNER medulla (kappa 0.5), sharper `beta_m`/`beta_n`,
  and DARKER melanin.  This is the sheen and the strands that break
  the silhouette.

Both grooms share the same `base_geometry` and usually the same density
and length painters; only the two `hair_material`s and the two length /
width / count triples differ.  The value range you get from a lighter
undercoat showing between darker guard hairs is most of what makes a
coat read as a coat -- it is also the structural substitute for
per-fibre agouti ticking, and it does something a band on one groom
cannot: it separates guard silhouette from undercoat mass.

## Regions are painters in anatomy space

`density` (a `[0,1]` rejection mask) and `length_painter` (a multiplier
on `length`; 0 drops the strand) are evaluated **at each candidate ROOT**
when the groom is realized.  The root's own object position `Po` and
normal `N` are in scope, so a multi-region groom is stated in the
creature's own anatomy -- no UV chart, no seams, and one painter stays
correct for every `standard_object` placement of that groom, because
realization runs once per GEOMETRY and knows nothing of any object
transform.

What this buys, all shipped:

* a hairline (`smoothstep` on `Po.y + 0.55 * (-Po.z)` -- a plane tilted
  back over the skull, so growth stops at the front and runs to the nape);
* a pelt (`smoothstep` on `Po.y` alone -- everything above the base);
* thinning over ears and muzzle so the skin shows through;
* a per-root `fbm` jitter on the LENGTH, which makes the silhouette
  tuft instead of shrink-wrapping;
* **carve-outs**: subtract a small spherical footprint per hard-surface
  part, and eyes and a nose leather can sit in a groom without hair
  growing across them.  Get the carve radius wrong and you either bury
  the part or leave a bald crater around it;
* placement by NORMAL rather than position, which is how whisker pads
  work -- keep roots only where they face sideways-and-forward.

```text
scalar_painter
{
	name		pnt_den_carve
	def		dL   length(Po - vec3(-0.020, 0.012, 0.030))
	def		dR   length(Po - vec3( 0.020, 0.012, 0.030))
	def		eyes min(smoothstep(0.0090, 0.0150, dL), smoothstep(0.0090, 0.0150, dR))
	def		pad  smoothstep(0.35, 0.70, abs(N.x)) * smoothstep(0.05, 0.45, N.z)
	expression	eyes * max(pad, 0.35)
}
```

**Along the strand, use `u`.**  A `hair_material` painter is evaluated
at the SHADING point on the fibre, where `u` is the strand's
arc-length fraction root-to-tip and `v` is across the ribbon width.
So an ordinary `scalar_painter { expression ... }` bound to `eumelanin`
BANDS each fibre directly -- agouti ticking, dark roots with bleached
tips, a dip-dyed look:

```text
scalar_painter
{
	name		eu_band
	def		tip smoothstep(0.30, 0.72, u)
	expression	mix(2.40, 0.55, tip)
}
```

Older scenes in this repository fake along-strand banding with a
cylindrical-radius trick on `Po`; that predates the discovery that `u`
is already there and is only valid where the body really is a barrel.
Use `u`.  CROSS-strand variation (a calico patch, a tint that differs
from one strand to the next) is a different coordinate: each strand's
root UV on the base is baked at generation time into `ri.ptCoord1`, but
no scene-language painter reads that channel, so a `hair_material`
painter cannot see it.  Get cross-strand variation from the ports that
ARE evaluated per root -- `density` and `length_painter`, which see the
root's `Po` and `N` -- or by splitting the coat into two grooms with two
materials, which is what the two-layer recipe already does.

## Styling: gravity drapes, comb sweeps sideways, guides say shapes

Read this before spending time on `comb`.

* **`gravity` is the drape lever.**  World -Y droop at the TIP as a
  fraction of the strand's OWN length, weighted t^2 so roots stay
  normal-aligned; `1` means the tip falls a full strand-length, which
  is the clamp on its reach.  It is frame-independent and it is the
  only thing that actually makes hair fall.  It is a styling knob, not
  a simulation: **it knows nothing about collisions**, so a long groom
  will drape through a shoulder.
* **`comb` is AZIMUTHAL on any surface of revolution.**  It is a colour
  painter whose RGB encodes a tangent-space direction `d = 2*rgb - 1`
  in the frame built from the base's UV map, and the blue channel is
  inert (projected straight out).  On a `lathe_geometry`, and on the
  cylindrical wrap of an `sdf_geometry` / `skeleton_geometry`, `dP/du`
  runs AROUND the form -- so a constant comb reads as a side part and a
  lateral flare, and **cannot sweep hair down a head or back along a
  spine**.  This was A/B'd on both base kinds: comb up, comb down and
  no comb all flare.  Use it for what it does (`0.80 0.5 0.5` is a
  convincing side part), use it to kill the radial rosette that
  grow-along-the-normal gives any convex head, and reach for `gravity`
  for everything else.  On a base with NO texture coordinates the frame
  falls back to each triangle's first edge, which is PER-TRIANGLE and
  therefore noise.
* **`hair_guides` expresses what a comb cannot**, because a comb is ONE
  VECTOR and a guide is a WHOLE CURVE.  Each strand's shape is
  interpolated from its three nearest guides (nearest by root-to-guide-root
  distance, inverse-distance weights); binding `guides` REPLACES
  straight-along-the-normal growth and nothing else, so `gravity`,
  `frizz`, `clump` and `curl` still compose on top.  A guide is a
  SHAPE, not a root -- it does not plant a strand where you drew it.
  One whole guide per `guide` line, root first, at least two points.
  An unknown guide-set name is an error, never a silent fall-back.

  Five authoring lessons, all paid for on the shipped wave specimen:

  1. **Author in polar / root-local terms, not in world space.**  The
     transport is RIGID in the base's `{tangent, bitangent, normal}`
     frame, so a guide authored along world +X is replayed rotated with
     the frame; azimuthally-rotated copies of one world shape
     interpolate to mush.  A set wobbled along world +X came back MORE
     homogeneous, not less.
  2. **Integrate outward from the surface normal, don't plot a fall.**
     Leave along the local normal and turn toward -Y as you go.  On the
     top of a head, "straight down" is INTO the block, and a set that
     simply fell from its root left the crown bare.
  3. **Tangential wobble dominates; radial is depth-invisible.**  The
     shipped S-wave is ~1.65 periods of tangential swing against 1.15
     radial.
  4. **Ramp the amplitude from ZERO at the root**, so no guide ever
     starts inside the body.  The cost is a crown smoother than the
     fall; that is the correct trade.
  5. **Stagger the phase** -- advance it once around the head per ring
     and offset between rings, or crests ring up into hoops instead of
     spiralling.

  What survives resampling is the guide's SHAPE and a kink's position as
  a FRACTION of total arc length -- the polyline is resampled at uniform
  arc-length fractions onto the groom's own `segments` count, so two
  guides with the same points but different spacing between them give
  the identical strand.  Generate the numbers with a script; a real set
  is tens of curves:

```text
hair_guides
{
	name	gd_hook
	guide	0.000 0.040 0.000  0.004 0.052 0.002  0.012 0.060 0.001  0.021 0.063 -0.004
	guide	0.028 0.028 0.000  0.036 0.038 0.003  0.043 0.045 0.001  0.049 0.046 -0.005
}

hair_geometry
{
	name		groom_waved
	base_geometry	scalp
	count		88000
	segments	18
	length		0.30
	guides		gd_hook
	gravity		0.30
	frizz		0.045
	clump		0.52
	clump_size	0.026
}
```

* A **curl is a helix**, not a wave: `curl_radius` about the strand's
  own axis with `curl_step` as the pitch (arc length per full turn,
  and it must be > 0 whenever the radius is).  Pitch a quarter of the
  strand length is a ringlet; open the pitch far enough that it stops
  reading as a ringlet and what is left is a straight strand.  **There
  is no setting of the two numbers that is a wave, and `frizz` cannot
  supply one either -- noise is not undulation.**  A wave is a guide.
  A tight helix needs control points: `segments` defaults to 8, and 8-16
  is the range for a strong curl or comb (2 is a perfectly straight
  quill, 18 for a tight helix, cost is linear in this).

## Reads as fibres, or reads as mass

At any normal framing a 0.03-0.2 mm fibre is a fraction of a pixel, so
what a viewer sees is the converged MASS and never a strand.  Decide
which of the two you are making, because the counts go OPPOSITE ways.

* **Mass.**  Dense, thin, many.  Below ~64 spp a groom reads as
  GLITTER rather than as hair -- the difference between 64 and 256 spp
  is the difference between a texture and a material.  Look-dev at
  32-64 spp (composition, groom regions and light angle all read
  there) and commit at 256.
* **Fibres.**  **COUNT IS A LOOK PARAMETER, NOT A QUALITY PARAMETER.**
  The shipped dandelion clock started at 260 000 pappus filaments --
  the density that makes a coat read -- and came out a featureless
  white ball.  At **22 000** the structure came back and you can see
  through the head.  When fibres are meant to be SEEN, fewer is the
  correct direction, and thicker with it: the boar bristle on the
  variety plate is 14 000 strands at 1.4 mm root width, fifteen times
  fewer and fifteen times thicker than the angora beside it.  A bristle
  brush is SPARSE and that is what makes it a bristle brush; the
  temptation to raise the count is the thing to resist.  Pair it with
  `gravity` and `frizz` near zero so each quill stays straight and
  radial and the light rakes along individual fibres.
* You need a framing that resolves a fibre for this to pay -- roughly
  0.1 mm per pixel, i.e. a long lens close in on a small subject.
* **Grow the fluff off a larger INVISIBLE shell.**  A groom's base need
  not be rendered: growing outer filaments off a shell sphere larger
  than the visible core puts them in a spherical band instead of
  packing them into the middle.  The same trick buries whisker roots
  inside a muzzle so only the emergent length is ever seen.

## Sheen and rims

The three lobes behave differently and want different instruments; a
single light cannot show a fibre BCSDF.

* **R** (surface reflection) is the primary highlight.  It never enters
  the fibre, so **absorption never touches it** -- it carries the
  LIGHT's colour, not the hair's, and a black groom and a blond groom
  rim identically.  Density, count and light angle are the levers on it;
  melanin is not.
* **TT** (two transmissions) is the bright halo when backlit, and it is
  the single most expensive thing a hair BCSDF buys.  **It needs the
  light on the far side of the subject from the camera.**
* **TRT** (one internal reflection) is the coloured, shifted secondary
  highlight and the glints.

**Coherent gloss needs coherence.**  A mirror cone is only legible
while it stays correlated between neighbouring fibres, so the glossy
black specimen on the variety plate runs `frizz 0.000` and `clump 0.08`
-- the two lowest on the plate.  Jitter of either kind decorrelates the
lobe into glitter, which is exactly what its first pass produced.  Its
`gravity` is 0.72 rather than 1.00 for the same reason: a strand that
snaps vertical in its first centimetre offers ONE tangent direction,
where a slower drape sweeps its tangent smoothly through a range, and
the sheen IS the locus where that sweep crosses the mirror condition.

**Where the sheen band sits is GEOMETRY, not roughness.**  On that same
specimen the key sits 36.2 deg above a vertical fibre's normal plane
and the camera 1.9 deg below it, so the R lobe misses its mirror
condition by 38 deg against a `beta_m` of 6.3 deg.  No roughness value
fixes that.  Closing it needs the fibres TILTED off vertical across a
wide area, or a cuticle `alpha` near 19 -- five times any real hair.
If a sheen band is missing, move the light or tilt the hair.

For a mixed rig: a warm key in front for form and the R band, a wide
panel behind and above (out of frame) for the TT halo, and a dim cool
fill so the shadow halves stay chromatic -- a pale groom's
multiple-scattering glow only separates from a dark groom's absorption
when the unlit side has colour in it.  Keep the room dark; the groom
should be the brightest thing in the frame.

## Traps

1. **`oidn_denoise FALSE` on hair, and never `oidn_prefilter accurate`.**
   Measured: OIDN removes real strand structure, and the deficit does
   not close with sample count (15-18% structure deficit on `fast`,
   25-30% on `accurate`, at every sample count).  The crossover is
   between 64 and 128 spp -- below it the denoiser still helps, above
   ~100 spp it costs more than it returns.  Let the samples carry the
   convergence.
2. **`clump_size` comparable to fibre spacing shows the grid.**  Roots
   are quantised onto a grid of `clump_size` and each occupied cell's
   first strand becomes that cell's centre, so a cell that is small
   relative to the spacing renders the quantisation itself as corduroy.
   The shipped fleece went from `0.011` (visible grid) to `0.024`
   (locks).  `clump` also does nothing at all unless `clump_size > 0`.
   Clumping is what turns an even coat into LOCKS -- 0.5+ with a
   generous cell.
3. **Scale.**  See the top of this file: the width defaults are metres.
4. **Bind every scalar-pipe slot on nearby materials.**  A slot like
   `polished_material.tau` is an `IScalarPainter` whose "none" default
   is a COLOUR painter, so leaving it unbound fails that material.  The
   groom is fine; the eye or the nose leather beside it vanishes.
5. **Read the diagnostics -- the scene will still load.**  A chunk whose
   apply fails is diagnosed BY NAME and the derive keeps going, so
   everything after it still lands and the CLI prints a `PARTIAL SCENE`
   banner.  Do not conclude from a mostly-correct render that nothing
   failed; one run surfaces every independent problem, so fix them all
   from that one log.
6. **File mode is mutually exclusive with grow mode.**  `file` imports a
   Cem Yuksel `.hair` groom and REFUSES every grow parameter by name
   (`base_geometry`, `count`, `length`, `segments`, `seed`,
   `base_detail`, `density`, `length_painter`, `comb`, `guides`,
   `gravity`, `frizz`, `clump`, `clump_size`, `curl_radius`,
   `curl_step`).  In file mode `width_root` / `width_tip` change meaning
   to MULTIPLIERS on the file's own thickness (default 1.0 = verbatim;
   the format's thickness is read as FULL WIDTH, so pass 2 if the file
   meant radius, or 0.001 for a millimetre file in a metre scene), and
   `root_uv_mode` (`zero` / `scatter`, grow-mode-refused) is the only
   way an imported groom gets per-strand root UVs at all.
7. **`base_detail` quantises where hair can grow** (default 32).  Roots
   are sampled on the tessellated base, so a curved base whose groom
   looks faceted at the silhouette wants this raised -- the shipped
   scenes use 80-200.
8. **`count` is the budget BEFORE the density mask**, and is capped at
   2 000 000 with an error.  Halving a density painter halves the
   strands you get, not the number you asked for.
9. **`frizz` is a fraction of control-point spacing**, weighted linearly
   along the strand.  0.05-0.3 breaks up the machine-perfect look;
   large values shred the strand into noise.

## Worked example: a groomed creature head

Real scale, two layers, regions in anatomy space, an along-strand
melanin band on the guard coat, and a backlight for the TT rim.

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultDirectLighting
}

# Metres.  Build a groom at real scale -- the width defaults are metres too.
scene_options
{
	scene_unit	1.0
}

film
{
	width		112
	height		112
}

pinhole_camera
{
	location	0.220 0.090 0.300
	lookat		0 0.02 0
	up		0 1 0
	fov		30.0
}

pathtracing_pel_rasterizer
{
	samples		32
	rr_min_depth	6
	oidn_denoise	FALSE
}

uniformcolor_painter
{
	name	pnt_bg
	color	0.075 0.082 0.098
}

lambertian_material
{
	name		mat_bg
	reflectance	pnt_bg
}

infiniteplane_geometry
{
	name	geo_bg
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_bg
	geometry	geo_bg
	material	mat_bg
	position	0 0 -0.55
}

# `rim` sits behind the head and out of frame: the TT halo needs the
# light on the far side of the fibre from the camera.
rect_light
{
	name		rim
	center		-0.70 0.85 -0.95
	size		0.55 0.55
	facing		0.52 -0.52 0.68
	color		1.00 0.90 0.72
	exitance	2200.0
}

rect_light
{
	name		key
	center		0.42 0.32 0.34
	size		0.26 0.26
	facing		-0.66 -0.50 -0.56
	color		1.00 0.96 0.90
	exitance	190.0
}

rect_light
{
	name		fill
	center		-0.34 0.10 0.34
	size		0.30 0.30
	facing		0.66 -0.10 -0.74
	color		0.62 0.72 1.00
	exitance	70.0
}

# THE BASE.  Warm mid-brown skin, not dark: you see between the fibres,
# and a dark substrate under a light coat reads as a dirty animal.
uniformcolor_painter
{
	name	pnt_skin
	color	0.16 0.11 0.075
}

lambertian_material
{
	name		mat_skin
	reflectance	pnt_skin
}

ellipsoid_geometry
{
	name	geo_head
	radii	0.042 0.038 0.052
}

standard_object
{
	name		obj_head
	geometry	geo_head
	material	mat_skin
	position	0 0.020 0
}

# REGIONS IN ANATOMY SPACE.  `density` and `length_painter` are evaluated
# at each candidate ROOT, with the root's own object position `Po` and
# normal `N` in scope -- so the groom's regions are stated in the
# creature's own coordinates, with no UV chart and no seams.
scalar_painter
{
	name		pnt_den
	def		muzzle smoothstep(0.028, 0.046, Po.z)
	def		belly  1.0 - smoothstep(-0.030, 0.006, Po.y)
	def		n      0.5 + 0.5 * fbm(Po * 60.0, 3.0, 0.5, 2.0)
	expression	mix(1.0, 0.72, muzzle) * mix(1.0, 0.80, belly) * clamp(0.78 + 0.30 * n, 0.0, 1.0)
}

scalar_painter
{
	name		pnt_len
	def		muzzle smoothstep(0.026, 0.048, Po.z)
	def		jit    0.5 + 0.5 * fbm(Po * 85.0, 3.0, 0.5, 2.0)
	expression	mix(1.0, 0.50, muzzle) * (0.78 + 0.42 * clamp(jit, 0.0, 1.0))
}

# LAYER 1 -- the undercoat.  Short, fine, HEAVILY medullated: this is the
# soft diffusing bulk that makes a coat read as a coat.
scalar_painter
{
	name	eu_under
	value	0.30
}

scalar_painter
{
	name	ph_under
	value	1.25
}

hair_material
{
	name		mat_under
	eumelanin	eu_under
	pheomelanin	ph_under
	beta_m		0.45
	beta_n		0.55
	alpha		2.2
	medulla_ratio	0.85
	medulla_scatter	2.4
	medulla_g	0.50
}

hair_geometry
{
	name		groom_under
	base_geometry	geo_head
	count		60000
	segments	5
	length		0.012
	width_root	0.00010
	width_tip	0.00004
	base_detail	80
	seed		7
	density		pnt_den
	length_painter	pnt_len
	gravity		0.40
	frizz		0.22
	clump		0.34
	clump_size	0.005
}

standard_object
{
	name		obj_under
	geometry	groom_under
	material	mat_under
	position	0 0.020 0
}

# LAYER 2 -- the guard coat.  Longer, coarser, thinner medulla, sharper
# roughness.  `u` is the SHADING point's root-to-tip arc fraction, so the
# melanin painters band each fibre: dark base, ochre tip -- agouti ticking
# on one groom, no geometry tricks.
scalar_painter
{
	name		eu_guard
	def		tip smoothstep(0.30, 0.72, u)
	expression	mix(2.40, 0.55, tip)
}

scalar_painter
{
	name		ph_guard
	def		tip smoothstep(0.30, 0.72, u)
	expression	mix(0.40, 1.15, tip)
}

hair_material
{
	name		mat_guard
	eumelanin	eu_guard
	pheomelanin	ph_guard
	beta_m		0.26
	beta_n		0.32
	alpha		3.0
	medulla_ratio	0.50
	medulla_scatter	1.6
	medulla_g	0.35
}

hair_geometry
{
	name		groom_guard
	base_geometry	geo_head
	count		18000
	segments	7
	length		0.026
	width_root	0.00018
	width_tip	0.00007
	base_detail	80
	seed		19
	density		pnt_den
	length_painter	pnt_len
	gravity		0.70
	frizz		0.16
	clump		0.30
	clump_size	0.009
}

standard_object
{
	name		obj_guard
	geometry	groom_guard
	material	mat_guard
	position	0 0.020 0
}
```

## Deep references

Three shipped scenes carry the field-tested detail, and their headers
are written to be read:

* `scenes/FeatureBased/Hair/cottontail_dusk.RISEscene` -- the hero.  A
  `skeleton_geometry` creature body, a two-layer coat, regions and
  carve-outs in anatomy space, whiskers on a buried invisible base, and
  the same generator three more times as a meadow.
* `scenes/FeatureBased/Hair/variety_gallery.RISEscene` -- nine grooms on
  nine identical blocks under one unchanged light.  All three colour
  tiers, the full medulla range, the guided wave specimen, and the
  measured notes on what still falls short.
* `scenes/FeatureBased/Hair/dandelion_clock.RISEscene` -- the one
  framing where individual fibres resolve, and the count-is-a-look
  lesson.

`scenes/Tests/Hair/` holds the minimal one-axis fixtures
(`hair_furnace`, `hair_melanin_ladder`, `hair_backlit_tt`,
`fur_medulla`, `hair_styled`) when you want the smallest scene that
exercises one thing.  Related skills: `materials-and-media-basics`,
`procedural-textures` (the painter families the region and band fields
are built from), and `lighting-recipes`.
