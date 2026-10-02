# DL-386: negative reflectance channels read as 0

Slice `debt-dl386`, branched from `master` `67cf9337a`, 2026-10-02.

## 1. The defect and the ruling

A colour authored outside the Rec.709 working gamut carries NEGATIVE channels
once it is converted to `RISEPel`.  The shipped instance is
`uniformcolor_painter` with `colorspace ROMMRGB_Linear`, converted by matrix
only (`Job.cpp` `ColorSpaceNameToRISEPel`): the Cornell red
(0.57, 0.025, 0.025) becomes (1.134, -0.100, 0.020) and the green
(0.025, 0.38, 0.025) becomes (-0.233, 0.462, -0.029).  A negative albedo is
not a physical quantity, and PT and BDPT measurably disagreed on it (DL-386
row; on `master` `f03359223` the Lambertian dragon room read BDPT/PT +0.23 %).

User ruling (2026-10-02): clamp ONLY the negative channels to 0 where
materials read reflectance colours; channels above 1 stay as authored; do not
re-author the scenes.

## 2. The chokepoint

Two inline accessors in `src/Library/Interfaces/IPainter.h`:

- `ReflectanceColor( painter, ri )` -- `GetColor`, negative channels -> 0.
- `ReflectanceColorNM( painter, ri, nm )` -- `GuardedGetColorNM` (exact 1 at
  authored white), a negative sample -> 0.

Every material read of a reflectance-type slot under `src/Library/Materials`
goes through them (value, `albedo`/`hemisphericalAlbedo`, `kray`, `Pdf`
selection weights, HWSS `EvaluateKrayNM`/`EvaluateLobeFNM`).
`SourceHygieneTest` fails on any raw `GetColor(ri)` / `GuardedGetColorNM` /
`GetColorNM` read in that directory outside a short allowlist, so a new
material cannot silently re-open the slot.

Why here and not elsewhere:

- **Not at the colour-space conversion.**  Negative channels have other
  sources (a texture in ROMM space, an expression, a spectral painter's RGB
  view of a narrow-band SPD, an authored negative literal), and the same
  converted painter can legitimately be data (an expression input, a
  `perlin2d_painter` colour, `bench_pt_bigmesh`'s negative `disp_lo`).
- **Not inside `IPainter`/`Painter`.**  `GetColor` is per-painter and one
  painter can feed a reflectance slot on one material and an emission or data
  slot on another; routing is by SLOT (the same reason `GetRadianceNM` exists).
- **Not a wrapping decorator painter.**  It would change painter identity
  (introspection, keyframing, `IsSpectrallyDefined`) and still need one wrap
  per slot.

## 3. What is and is not clamped

Clamped (every reflectance-type colour slot): Lambertian, Oren-Nayar
reflectance; GGX diffuse and specular tint; Cook-Torrance, Schlick, Ward
(isotropic and anisotropic), isotropic Phong and Ashikhmin-Shirley diffuse and
specular; polished diffuse; perfect reflector reflectivity; perfect refractor
refractivity; translucent front reflectance and transmittance; sheen colour
(`SheenBRDF`/`SheenSPF`, `fabric_material`); weave yarn colours (already
clamped to [0, 1] there); coated coat tint; hair tier-3 colour (already
clamped there).

Not clamped, deliberately: emission (`LambertianEmitter`, `PhongEmitter`,
every `GetRadianceNM` source, radiance maps, `AreaLightShaderOp`); the GGX
`tangent_rotation` colour painter (an angle); `CoatedBRDF`'s coat normal map;
every `IScalarPainter` slot; normal-map modifiers; painter-as-data
(expression `sample()`, noise-painter colour inputs, volume accessors,
Donner-Jensen skin offsets); the scene editor's introspection (shows the
authored value).

Unsure, left raw: the legacy `TransparencyShaderOp` transparency colour and
`AlphaTestShaderOp` alpha painter (multiplicative fractions, but read by
legacy shader ops, not by a material; a negative transparency is as
non-physical as a negative albedo).

## 4. RGB and spectral now agree

`RGBAlbedoSpectrum::FromRGB` (the Jakob-Hanika albedo uplift) clamps its input
to [0, 1] before the LUT lookup, so the spectral pipe never saw a negative
channel from an RGB-sourced painter.  After this slice the RGB pipe reads 0
for a negative channel too: the signed colour and its clamped twin evaluate
identically on both pipes (`NegativeReflectanceClampTest` [5]).  Above 1 the
two pipes still differ by the ruling (RGB keeps the authored 1.134, the albedo
uplift saturates) -- pre-existing and unchanged.  `ReflectanceColorNM`'s own
clamp matters only for spectrally AUTHORED data that dips below 0
(`NegativeReflectanceClampTest` [6]).

## 5. Evidence

Red-proofs on committed state, library sources reverted to `67cf9337a` (A/B):

- `tests/NegativeReflectanceClampTest.cpp`: 17/0, red 9/8.  Lambertian value
  green channel -0.031831 -> 0; kray -0.100 -> 0; red kept at 1.134.  A
  two-lobe `IsotropicPhongSPF` with an all-negative diffuse: 4000/8000
  emitted rays carried a negative kray, 3295 `Pdf` evaluations were negative
  and `RandomlySelect` returned a probability outside [0, 1] on 3956/3956
  selections (a negative weight reverses its CDF, cf. DL-100) -> 0 / 0 / 0.
  Spectral authored-negative sample: Lambertian `valueNM` -0.079577 -> 0.
- `BDPTStrategyBalanceTest --dl386-only` (the dragon room, ROMM walls, the
  dragon a Lambertian sphere; 48x36, 512 spp, 4 salted replicates per
  integrator, depth 16): BDPT/PT +0.1865 % (se 0.0225 %) -> +0.0178 %
  (se 0.0213 %); band 0.10 %.  The mechanism of the original disagreement is
  still NOT attributed to either estimator; the clamp removes its input.
- `SourceHygieneTest`: 169/0, red 168/1 (189 raw reads flagged).

## 6. Appearance

Only three shipped scenes bind an out-of-gamut colour to a reflectance slot
(a scan of every `uniformcolor_painter` converted to Rec.709; no shipped image
painter is authored in ROMM/ProPhoto): `pt_sss_dragon`, `bdpt_sss_dragon`,
`vcm_sss_dragon`, all through the red and green walls.  The other ROMM
painters are either in gamut or unused (`pnt_absorption` -- the SSS materials
author their coefficients inline -- and `composite_wacky_creature`'s
`hdr_scatter_*`).

**Review correction (2026-10-02):** that scan covered ROMM-authored painters
only.  `scenes/FeatureBased/Parser/sombrero.RISEscene` authors 1796 of its
3721 `uniformcolor_painter` literals with a NEGATIVE blue channel (down to
-0.587) in the default colourspace, all bound to `lambertian_material`
reflectance (direct-lit `pixelpel_rasterizer`), so its blue channel changes
too: surfaces that subtracted blue light now reflect none.  Not measured.

Whole-frame mean per channel, 160x120, shipped spp (64), OIDN off, linear
Rec.709 EXR, n = 3 CLI renders per build (sd in parentheses):

| scene | channel | before | after | change |
|---|---|---|---|---|
| pt_sss_dragon | R | 0.25217 (0.00003) | 0.27825 (0.00023) | +10.34 % |
| | G | 0.20791 (0.00052) | 0.21534 (0.00053) | +3.57 % |
| | B | 0.19281 (0.00286) | 0.18455 (0.00101) | -4.29 % |
| bdpt_sss_dragon | R | 0.25402 (0.00014) | 0.27838 (0.00030) | +9.59 % |
| | G | 0.20749 (0.00022) | 0.21602 (0.00041) | +4.11 % |
| | B | 0.18901 (0.00080) | 0.19149 (0.00788) | +1.31 % |
| vcm_sss_dragon | R | 0.25676 (0.00013) | 0.27973 (0.00045) | +8.95 % |
| | G | 0.20892 (0.00048) | 0.21660 (0.00071) | +3.68 % |
| | B | 0.18819 (0.00109) | 0.18753 (0.00194) | -0.35 % |

This is larger than "a small loss of cross-channel tint": the green wall's red
channel was -0.233, so every bounce off it SUBTRACTED red light from the
room; clamped to 0 the room's red rises ~9-10 % and green ~4 % (the red
wall's -0.100 green).  The blue column is dominated by the dragons' blue SSS
firefly tail (DL-333 / DL-356 record the same tail; the sd's are n = 3 on a
heavy-tailed quantity) -- the walls' blue channel moves only through the green
wall's -0.029.

Scenes without a negative reflectance channel do not move.  A
single-threaded, pinned-seed pixel hash (`WeaveGapShadowTransmittanceTest`
scenehash mode, 4 spp) of every shipped scene binding a `spectral_painter` or
`blackbody_painter` (21), six ROMM-authored in-gamut scenes and the Cornell
materials box matches before and after on 22 of those 28; the two dragon
scenes in the same list change.  The other six (`planetary_survey`,
`spectral_dispersive_caustic`, `hwss_mlt_spectral_cornellbox`,
`hwss_cornellbox_pt_ref`, `cornellbox_pointlight_spectral`,
`dielectrics_changing_ior`) are not reproducible in that harness: rerunning
the last three on the SAME fixed binary gives hashes and means that differ from
the first fixed run as much as the base run does (e.g.
`cornellbox_pointlight_spectral` 0.30877 / 0.31485 fix, 0.31295 base).  None
of the six binds a negative reflectance: the spectral rasterizers read the
true SPD, `dielectrics_changing_ior`'s ROMM painters are in gamut, and
`planetary_survey`'s only spectral painter (a blackbody) is an emitter.
