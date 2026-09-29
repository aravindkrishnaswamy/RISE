# DL-320: a double-sided emitter emits from both faces, for every strategy

Slice `debt-dl320`, 2026-09-28.  Ledger row: DL-320 in
[DEBT_LEDGER.md](DEBT_LEDGER.md).  Red-proof and gate:
`tests/DoubleSidedEmitterTest.cpp`, `tests/BDPTStrategyBalanceTest.cpp`
topology Z.

## 1. The defect

A `doublesided` emitter -- the `clippedplane_geometry` default, and any
double-sided mesh -- was TWO-sided for every strategy that HIT it and
ONE-sided for every strategy that SAMPLED it.

- **Hits.**  A double-sided geometry reports a back-face hit with its
  normal flipped toward the ray (DL-70), and a Lambertian luminaire is
  one-sided about the normal it is handed.  So `EmissionShaderOp`, PT's
  BSDF-sampled emission, BDPT s = 0 and VCM `EvaluateS0Impl` all saw the
  back face emit.
- **Samples.**  `IObject::UniformRandomPoint` returns the WINDING normal.
  `LightSampler`'s NEE arms rejected `cosLight <= 0` against it,
  `LightSampler::SampleLight` drew light-subpath directions about it
  only, and the three photon tracers did the same.
- **The weights.**  The hit side's MIS partners still assumed the
  samplers could reach the back face: PT's emitter-hit weight uses
  `fabs(cosLight)`, BDPT's `emPdfDir` and VCM's `emissionDirPdfSA` use
  the ray-facing normal.  The back face's light reached the image only
  through hits, down-weighted by a partner that never delivered.

On a small back-facing quad over a floor the image was 7-14x dark under
PT and BDPT, 5x under VCM, and exactly black under the legacy direct-
lighting chain and the legacy photon map.

## 2. The contract

Already decided before this slice: docs/SCENE_CONVENTIONS.md says a
double-sided quad emits from both faces (PBRT's `twosided`).  The hit
side already implemented it, so the sampling side is what changed.  The
two-faced source every strategy now describes:

| quantity | one-sided (unchanged) | two-sided |
|---|---|---|
| face toward direction `w` | winding normal `n` | `n` if `w.n >= 0`, else `-n` |
| NEE area density | `1/A` | `1/A` (unchanged) |
| NEE emitter cosine | `w.n`, zero behind | `|w.n|` |
| light-subpath direction density | `cos/pi` on the front hemisphere | `|cos|/(2 pi)` (face chosen with probability 1/2) |
| total power (selection PMF, photon budget) | `M A` | `2 M A` |

One definition serves every consumer: `EmitterSides::{FaceToward,
CosineEmissionPdf, FaceCount}` in `src/Library/Interfaces/IEmitter.h`,
keyed on the new `IGeometry::IsDoubleSided()` (true for double-sided
`TriangleMeshGeometry{,Indexed}` and `DisplacedGeometry`, double-sided
`ClippedPlaneGeometry`, and `BezierPatchGeometry` -- exactly the
geometries whose back-face hits report a ray-facing normal; every
analytic primitive keeps the default `false`).

## 3. What changed, site by site

| site | change |
|---|---|
| `LightSampler::Prepare` | a two-sided luminary's selection power (alias table, light BVH, RIS target) is `2 M A`; `LightEntry::twoSided` caches the query |
| `LightSampler` NEE, RGB and NM arms | `lumNormal` is flipped in place to the face toward the receiver; the surface-payload probe keeps the winding normal |
| `LightSampler::SampleLight` | the face is picked by remapping the first direction coordinate (no extra Sobol dimension); `pdfDirection = |cos|/(2 pi)`; `sample.normal` is the emitting face |
| `BDPTIntegrator` `LuminaryRadiance` | the light root evaluated toward an arbitrary direction (s = 1, t = 1) uses the face that faces it |
| `BDPTIntegrator` s = 0, s = 1, t = 1 emission pdfs | `EmitterSides::CosineEmissionPdf` |
| `VCMIntegrator::EvaluateS0Impl` / `EvaluateNEEImpl` | the same density; NEE evaluates `Le` from the face toward the eye vertex |
| `PhotonTracer.h`, `SpectralPhotonTracer.h`, `SMSPhotonMap.cpp` | power counts both faces; each photon's face chosen with probability 1/2 by remapping the first direction coordinate |
| `ClippedPlaneGeometry::UniformRandomPoint` | a DOUBLE-sided plane's sample point is no longer pushed 1e-5 toward the front face (see below) |

Not changed, and why:

- PT's emitter hit and `EmissionShaderOp`: already two-sided, and their
  area-measure partner `pdfSelect d^2 / (A |cos|)` already describes the
  two-faced NEE density.
- The legacy `DirectLightingShaderOp`: it calls
  `LightSampler::EvaluateDirectLighting`, fixed above.
- SMS light-directed connections: they take `LightSample::Le` and a
  `fabs` emitter cosine, so they already treated every emitter as two-
  sided.  That is right for a double-sided emitter and WRONG for a single-
  sided one -- DL-347, below.
- MLT: shares BDPT's generator and connections.

**The clipped-plane offset.**  `ClippedPlaneGeometry::UniformRandomPoint`
pushed every sample point 1e-5 along the winding normal, "so when the
clipped plane is a back-facing luminary it still occludes anyone behind
it".  For a double-sided plane that put the point on the far side of the
plane from a back-face receiver, and every BDPT/VCM connection from
behind (epsilon `2 * BDPT_RAY_EPSILON = 2e-6`) crossed the emitter and
read as occluded -- BDPT stayed at 0.0702 of the closed form with every
other change in place.  PT's NEE shadow ray stops 0.001 short and cleared
it.  The push is kept for single-sided planes.

## 4. Measurements

All renders salted per render (`SobolSamplerTestHooks::ValueSalt`) and
libc-seeded per render; pre = the committed tree before the fix
(`1eaf3750`), post = the fix, both built separately.

### 4.1 Closed form (`DoubleSidedEmitterTest` row Z1)

A 3x3 quad at h = 2.6 over a 400 x 400 rho 0.5 floor; an orthographic
camera at z = 1 sees the floor square [-2, 2]^2.  Closed form: the
point-to-parallel-rectangle form factor (Howell), cross-checked against
brute-force quadrature to 2e-7, averaged over the footprint: image mean
0.328780.  n = 4, image-mean sd 0.01-0.12 %.

| integrator | face-down pre / post | BACK face pre | BACK face post |
|---|---|---|---|
| PT | 0.9991 / 0.9991 | 0.0702 | 0.9991 |
| BDPT | 0.9993 / 0.9993 | 0.0702 | 0.9992 |
| VCM, connections only | 0.9993 / 0.9994 | 0.2011 | 0.9991 |
| legacy `[DirectLighting]` | 0.9993 / 0.9993 | 0.0000 | 0.9993 |
| legacy `[DirectLighting, dt]` (NEE + BSDF-sampled emission MIS) | 0.9992 / 0.9992 | 0.0701 | 0.9994 |

The common 0.9991-0.9994 is the orthographic footprint's half-pixel
offset (section 5) acting on a centred, second-order-symmetric field.

Reference-free rows (back / face-down ratio, n = 4): PT spectral hero
0.0706 -> 0.9943, PT HWSS 0.0701 -> 0.9987, BDPT HWSS 0.0699 -> 1.0016,
VCM with merging 0.1869 -> 0.9997, legacy global photon map 0 -> 0.9958.
Mixed lights (row Z2: the back-lit quad plus a mirror pair of small
single-sided quads, closed form 0.335217): PT 0.0931 -> 0.9994, BDPT
0.0941 -> 0.9987, VCM 0.2136 -> 0.9879 (VCM's face-down control reads
0.9835 pre / 0.9880 post: DL-348).  Single-sided back face (row Z5):
exactly 0 under PT, BDPT, VCM, legacy direct and PT HWSS, pre and post.
Suite: 21/13 pre, 34/0 post.

`BDPTStrategyBalanceTest` topology Z (topology B's 1x1 quad at z = 4
wound away from the receiver): PT and BDPT back face read 0.034 % of the
face-down render pre-fix (1.97e-5 vs 0.05796) and match it to 1e-5
post-fix; red 6/3, full suite 229/0.

### 4.2 The DL-09 review fixture (camera inside a dielectric box)

The DL-09 review's scene S2 (camera inside a graded 1.4 box, a 3x3
double-sided quad above it wound away from the scene, a diffuse ball),
and its constant-1.4 twin S2c.  Ratios to PT from the same build, salted
n = 4 per cell, sd in parentheses.  PT itself does not move (behind a
delta interface NEE is blocked and PT's hit weight is 1): S2 0.095559 ->
0.095568, S2c 0.090363 -> 0.090373 at depth 32.

| scene, depth | BDPT/PT pre | BDPT/PT post | VCM/PT pre | VCM/PT post |
|---|---|---|---|---|
| S2 graded, 8 | 0.9188 (0.0029) | 0.9436 (0.0025) | 0.7972 (0.0016) | 0.9432 (0.0017) |
| S2 graded, 32 | 0.9736 (0.0029) | **0.9996** (0.0026) | 0.8453 (0.0014) | **0.9981** (0.0011) |
| S2c constant, 8 | 0.9458 (0.0034) | 0.9728 (0.0036) | 0.8144 (0.0032) | 0.9699 (0.0049) |
| S2c constant, 32 | 0.9697 (0.0033) | **0.9978** (0.0035) | 0.8363 (0.0039) | **0.9945** (0.0052) |

The pre-fix depth-8 cells reproduce the review's "BDPT 0.9175 / VCM
0.795".  At depth 32 both close to within 0.2-0.6 %; the depth-8
residual is the eye-depth truncation DL-308 documented (PT is
untruncated).  VCM's remaining -0.2 % (S2, 3.5 sd of the mean) / -0.55 %
(S2c, 2 sd) matches the flipped-winding measurement the DL-308 slice
recorded (0.9927) and is not sidedness.

### 4.3 Census of shipped scenes

176 scenes bind a luminaire to double-sided geometry -- almost every area
light in the tree, because `clippedplane_geometry` defaults to
`doublesided TRUE`.  Each was rendered pre and post under PT, BDPT and
VCM (the scene's own rasterizer stripped; 48 px wide, 16 spp, depth 8,
3 salted seeds per build, interleaved), a mover being |change| > 0.2 %
at > 4 sd.

Movers that are the fix (back faces that light something), re-measured
at 64 px / 64 spp / n = 4 (post/pre, then post/PT-post):

| scene | PT | BDPT | VCM |
|---|---|---|---|
| `FeatureBased/BDPT/bdpt_crystal_garden` | 1.917 | 2.187 (0.888) | 2.782 (0.898) |
| `FeatureBased/SDF/sdf_morph_torture` (census artifact: its rasterizer chunk carries its environment map, which the census stripped; with it the scene moves +1.4 %, section 7) | 1.868 | 1.897 (1.010) | 1.908 (1.001) |
| `Tests/MLT/mlt_torus_chain_atrium` | 1.600 | 1.735 (0.950) | 1.904 (0.946) |
| `Tests/Caustics/triplecaustic` (+ `_pt_sms`, `VCM/triplecaustic_vcm`, same lights) | 1.355 | 1.326 (0.970) | 1.430 (0.995) |
| `FeatureBased/Shaders/SSS/sss_gi_dragon` | 1.092 | 1.207 (0.588) | 1.310 (0.580) |
| `Tests/SubsurfaceScattering/pt_sss_wax_sphere` (+ `rwsss_sphere`, `rwsss_bdpt`, same light) | 1.021 | 1.023 (0.997) | 1.032 (0.999) |

Every one of them moves UP under all three integrators and the three
integrators move together (the bidirectional ones further, because they
were darker).  The remaining cross-integrator gaps are pre-existing and
narrowed: `bdpt_crystal_garden` BDPT/PT 0.778 -> 0.888 (glass-heavy),
`sss_gi_dragon` BDPT/PT 0.532 -> 0.588 (a pre-existing SSS gap on that
scene, not sidedness).

Movers that are NOT sidedness:

- **Energy-gaining spectral Cornell boxes under BDPT, -1.2 %**
  (`cornellbox_bdpt_spectral`, `hwss_cornellbox_*`, 6 scenes sharing one
  setup).  Their reflectance painters carry `scale 4` (albedo up to 2.8),
  so the light-transport series diverges and a depth-capped render is a
  truncation.  Halving the light-subpath emission density moves MIS mass
  between strategies, and `MISWeight` reserves mass for strategies the
  subpath caps cannot generate (DL-351), so the truncated sum moves.  The
  same mechanism, at 0.29 %, on the ordinary `cornellbox_bdpt` (white
  albedo 1.0) at depth 8 VANISHES at depth 40 (pre 0.47300, post 0.47290,
  z -0.4, n = 6); with the spectral box's painters at `scale 1` it is
  -0.19 % (z -1.5).
- **VCM on the `Tests/Volumes/pt_painter_*_grid` scenes, -4 to -5 %**
  (gabor, domainwarp, worley, voronoi, wavelet).  VCM read +17 to +19 %
  ABOVE PT and BDPT there before the fix (gabor: PT 5.108, BDPT 5.132,
  VCM 6.035) and reads +12 % after (5.717): toward PT, and the same with
  merging off (6.076 -> 5.807), so a pre-existing VCM medium bias, not
  this row.
- Two VCM caustic scenes (`cornellbox_vcm_caustics` -1.2 %,
  `pt_reflected_caustic` -1.0 %) now read BDPT's value instead of PT's;
  PT and BDPT themselves differ by 0.9-1.7 % there.
- Heterogeneous-medium scenes under PT (`pt_painter_curlnoise3d_fog`
  -0.46 % at n = 3) are noise: `HeterogeneousMedium`'s thread-local libc
  RNG makes those renders non-reproducible, and at n = 10 the change is
  +0.1 %.

**Bit identity.**  Single-sided emitters: BDPT, PT HWSS and PT spectral
renders are bit-identical to the pre-fix build (`DoubleSidedEmitterTest`
Z5 hashes); PT RGB and the legacy direct-lighting chain differ in the
last ulp on 19-25 % of pixels (max 1.6e-15 relative) -- the RGB NEE arm's
code moved and `-ffast-math` contracts it differently; three source forms
of the face flip were tried and none restored bitwise identity.  Face-on
double-sided emitters cannot be bit-identical: the light subpaths now
leave from either face, and the clipped-plane sample point moved 1e-5;
their means are unchanged (topology B, Z1 face-down, the census).

### 4.4 Cost

Interleaved, user CPU, 128 x 128 Cornell boxes with a face-down double-
sided panel 1e-4 below the ceiling, n = 5 per build, paired:

- PT 512 spp: +2.9 % +/- 2.7 % (sd; t = 2.4), on a loaded machine.
- BDPT 64 spp: **-21.8 % +/- 2.3 %**.  Half the light subpaths now leave
  the back face into the 1e-4 gap and end at once.  They are not free in
  variance: the image's per-region relative sd rose 0.176 % -> 0.243 %
  (n = 6), about 1.5x the variance per unit time for BDPT on a ceiling
  panel.  That is the price of emitting the power the contract says the
  back face has; a scene that wants a one-sided panel should say
  `doublesided FALSE` (as `rect_light` does).

## 5. Residuals

- **DL-347** (new): SMS light-directed connections ignore emitter
  sidedness.  `ManifoldSolver` prices a connection with `LightSample::Le`
  (evaluated along the light-subpath direction) and a `fabs` emitter
  cosine, never testing the side, so a SINGLE-sided emitter lights SMS
  caustics from its back face.  `sms_k1_refract` with the emitter made
  single-sided and turned face-up: PT + SMS 0.12421 against PT without SMS
  0.09671 (+28 %), identical pre and post.
- **DL-348** (new): a multi-light VCM (and, before this slice, BDPT)
  deficit.  Row Z2's face-down control -- one 3x3 double-sided quad and a
  mirror pair of 0.8 x 0.8 single-sided quads, closed form 0.335217 --
  reads PT 0.9999, BDPT 0.9944 pre / 0.9984 post, VCM 0.9835 pre / 0.9880
  post (n = 4-6).  Each light alone agrees across all three integrators.
- Orthographic camera footprint offset (no id reserved; for the
  supervisor): the ortho footprint sits about half a pixel toward world
  -x.  A lone off-axis quad at x = +2 / -2 (view 1.0, 32 px) reads 0.9794
  / 1.0208 of its closed form under PT and BDPT alike; the error scales
  with the pixel size.  Likely `OrthographicCamera::GenerateRay`'s
  `(W/2 - screenX)/W` against a pixel-centred `screenX`.
- A CLOSED double-sided mesh emitter (e.g. an emissive sphere tessellated
  with `double_sided TRUE`) now also emits inward: NEE samples on its far
  side are occluded by its near side (correct, just wasted), and half its
  light subpaths and photons start inside it and are trapped.  Unbiased;
  such an emitter should be authored single-sided.
- VCM's own merge radius is unaffected in practice (the auto pre-pass saw
  540 -> 242 segments and 0.774 -> 0.786 on a grid scene), but halving a
  face-down panel's useful light subpaths halves VCM's photon density
  for it.

## 6. Scene-authoring consequence

A double-sided emitter now lights whatever is behind it, under every
integrator.  Ceiling panels wound face-down 1e-4 below a ceiling light the
ceiling behind them, harmlessly (the light is trapped); a panel with open
space behind it sends half its power there.  Use `doublesided FALSE`
(or `rect_light`) for a one-sided panel.  docs/SCENE_CONVENTIONS.md
section 4 "Lights" states the rule.

## 7. Scene re-authoring (user ruling, 2026-09-28)

Ruling: keep the two-faced convention, accept the brightening, and adjust
the shipped scenes so they look correct.  No `doublesided FALSE` flips.

**Which scenes.**  Every scene whose whole-image mean moved by more than
5 % under PT, BDPT or VCM in the section 4.3 census, plus a second census
of the 23 legacy-chain scenes (`pixelpel`, `pixelintegratingspectral`)
under their OWN rasterizer (it added none).  Re-checked under each
scene's own rasterizer before editing:

- `sdf_morph_torture` is NOT a mover: the census stripped its rasterizer
  chunk, which carries its environment map; with it the image moves
  +1.4 % (+7.8 % in one quadrant).  No edit.
- `sms_k2_glasssphere` (VCM -31 % at z -3.2, n = 3) and the
  `pt_painter_gabor3d_grid` / `domainwarp3d_grid` volumes (VCM -5.3 / -5.0 %)
  moved only under VCM, which none of them uses; their own PT did not
  move.  Those are VCM's own noise and medium bias (section 4.3), not a
  change of look.  No edit.
- The SSS wax / rwsss spheres (+2-3 %) are under the threshold.  No edit.

**Method.**  For each edited scene: pre / post / tuned renders with the
scene's own rasterizer (film 120 px wide, samples capped at 32 -- 8 for
the dragon, MLT at its own settings; OIDN off; salted n = 3-4), the mean
of the region the author composed for, and one uniform factor on the
scene's luminaire `scale`s (the only lights in these scenes whose output
matters -- the dragon and the triplecaustic trio have no other light, and
the two spot lights in crystal_garden / one in the atrium were measured
at 0.13 % / 0.02 % of the subject region with the panels off).  A region
is linear in a uniform scale, so the factor is `pre / untuned`.  Before /
after PNGs at the scenes' own tonemapping are in the slice's scratchpad
(`debtclean/dl320/png/`: `<scene>_pre`, `<scene>_post`,
`<scene>_tuned_post`).

**Classes.**  (a) the back face lights something that is legitimately lit
by a two-sided panel and the author's `scale` assumed one face: reduce the
scale to the subject's pre-fix level.  (b) the back face's light dies
harmlessly: no edit.  (c) the back face lights something the author did
not want lit: a geometric edit.  All six edited scenes are (a).

| scene (own integrator) | edit | class | subject region | subject pre / untuned / tuned |
|---|---|---|---|---|
| `FeatureBased/BDPT/bdpt_crystal_garden` (BDPT) | `warm_lum` 180 -> 84, `cool_lum` 140 -> 65 | (a) | glass + floor, x 0.1-0.9, y 0.2-0.95 | 1.0414 / 2.2352 / 1.0435 (+0.2 %) |
| `Tests/MLT/mlt_torus_chain_atrium` (MLT) | `warm_lum`, `cool_lum` 200 -> 55.6, `neutral_lum` 2000 -> 556 | (a) | torus chain, x 0.2-0.8, y 0.25-0.8 | 1.063 / 3.888 / 1.076 (+1.3 %) |
| `FeatureBased/Shaders/SSS/sss_gi_dragon` (legacy `pixelpel`) | `lum` 10 -> 8.2 | (a) | dragon, x 0.3-0.78, y 0.3-0.85 | 0.0641 / 0.0789 / 0.0664 (+3.4 %, sd 0.004) |
| `Tests/Caustics/triplecaustic` (legacy `pixelpel` + photon maps) | `white_lum` 90 -> 78.8 | (a) | spheres + caustic, lower half | 0.5295 / 0.6049 / 0.5296 |
| `Tests/Caustics/triplecaustic_pt_sms` (PT + SMS) | `white_lum` 90 -> 74.5 | (a) | spheres + caustic, lower half | 0.5987 / 0.7229 / 0.5981 |
| `Tests/VCM/triplecaustic_vcm` (VCM) | `white_lum` 90 -> 71.3 | (a) | spheres + caustic, lower half | 0.5936 / 0.7489 / 0.5929 |

Why (a) in each:

- **crystal_garden, mlt_torus_chain_atrium.**  Their ceiling panels are
  wound face-UP (normal `+y`), so the scene below has always been lit by
  the BACK face.  Before DL-320 that light reached the image only through
  MIS-weighted hits (the `pre` column is that partial count); now it
  arrives in full.  The panel's other face lights the ceiling, which it
  always did.  The subject is what the down-facing face should light, so
  the scale comes down.  In the atrium the side walls now read about 4x
  brighter than before (0.19 -> 0.77) with the torus chain matched: they
  were dark only because the panels' downward light was missing.
- **sss_gi_dragon.**  The panel stands close to the left wall and faces
  into the room; its back face now lights that wall (before, a dark patch
  sat directly behind the panel), and the bounce brightened the dragon by
  23 %.  A lit wall behind a two-sided panel is the physics the convention
  asks for.
- **triplecaustic trio.**  The three small panels' back faces light the
  back and side walls.  The PT variant ALREADY showed that glow before
  DL-320 (PT counted most of the back-face light through hits), so the
  glow is part of the look the author accepted, not something to hide.
  A (c) edit was tried and dropped: a black "lamp housing" 0.002 behind
  each panel removed the glow, but (1) it framed the centre panel in black,
  (2) under VCM the merges gathered the housing's light vertices through
  the emitter's own reflective surface (`white_lum` carries `material
  white`) and read +25 % at a fixed merge radius, and (3) the housing's
  0.002 segments shrank VCM's auto radius from 0.0107 to 0.0046 (DL-319's
  mechanism).
- **The trio no longer shares one scale** (78.8 / 74.5 / 71.3).  Before
  DL-320 each integrator counted the back faces differently -- the
  photon-mapped direct chain not at all, PT and VCM in part through
  MIS-weighted hits -- so the three scenes' "pre-fix look" differed, and
  a single scale would have left the direct-chain scene 5.4 % darker than
  it was, which the ruling forbids.

Whole-image means at the census settings (48 px, 16 spp, the scene's own
rasterizer stripped, salted n = 3; post / pre):

| scene | PT untuned / tuned | BDPT untuned / tuned | VCM untuned / tuned |
|---|---|---|---|
| bdpt_crystal_garden | 1.901 / 0.887 | 2.200 / 1.026 | 2.785 / 1.292 |
| mlt_torus_chain_atrium | 1.617 / 0.454 | 1.759 / 0.487 | 1.970 / 0.534 |
| sss_gi_dragon | 1.090 / 0.894 | 1.210 / 0.989 | 1.314 / 1.077 |
| triplecaustic | 1.349 / 1.182 | 1.324 / 1.159 | 1.414 / 1.250 |
| triplecaustic_pt_sms | 1.350 / 1.112 | 1.327 / 1.098 | 1.446 / 1.187 |
| triplecaustic_vcm | 1.349 / 1.068 | 1.321 / 1.047 | 1.433 / 1.133 |

The whole image is NOT the tuning target.  It stays above pre-fix where
the back face lights a backdrop (the triplecaustic walls), and falls well
below it in the atrium, whose pre-fix mean was dominated by the visible
panels themselves (at 55.6 instead of 200 they still clip to white in the
PNG).  The ratios also differ by integrator because each integrator's
pre-fix image counted the back face differently.

**Regenerated.**  `tests/data/cst_derive_golden.txt`: exactly the six
edited scenes' digests changed (`--generate`; 456 MATCH / 0 DRIFT after).
None of the six is an `AgentEvalCheckTest` fixture.
`CausticReachHarnessTest` renders `bdpt_crystal_garden` but only reports
scale-free ratios; it runs green (rc 0).
