# DL-381: the random-walk SSS model is not reciprocal

**Slice:** `debt-dl381`, 2026-10-02, branched from `master` `d83c87dd3`.
**Outcome:** root-caused, no library change. Both families estimate their
own model function correctly. The model itself is not reciprocal, and on the
row's fixture the light family is the closer of the two to exact physics.

## 1. The question

Fixture G-RW (DL-375 / DL-377 reviews): a `randomwalk_sss_material` sphere in
front of a Lambertian wall, lit by an omni that sits inside an index-matched
(ior 1.0) glass sphere. The glass blocks every eye-side strategy, so only the
LIGHT family reaches these paths. BDPT reads sphere +7.07 % / wall -3.13 %
against PT on the same scene without the glass, and VCM with merging off
reads +5.07 % / -2.31 %. The open question was which side is right.

## 2. The mechanism

`RandomWalkSSS::SampleExit` starts the walk along the REFRACTED direction of
the ray that reaches the surface. When the walk reaches the boundary it does
the internal Fresnel coin, then leaves along a cosine direction weighted
`Ft(cos)/c`. That makes the model exact at the end the walk starts from and
Lambertian at the other end:

| family | exact end | cosine (`Sw`) end |
|---|---|---|
| eye (PT; BDPT/VCM where DL-375's partition gives it the path) | camera | light (NEE through `RandomWalkEntryBSDF`) |
| light (BDPT/VCM/MLT, delta-lit through a delta interface) | light | camera (splat or connection through `Sw`) |

The two families therefore estimate two different functions,
`S_eye = Ft(w_o) R(x_o, refr(w_o) -> x_i) Sw(w_i)` and
`S_light = Ft(w_i) R(x_i, refr(w_i) -> x_o) Sw(w_o)`. The Fresnel factors are
symmetric; the difference is only which end's direction the walk remembers.

## 3. Independent reference

`tools/DL381RandomWalkReciprocityMC.cpp` is a standalone light tracer of the
DL-375 scene and uses no RISE code. It replaces the pinhole with an aperture
disk of radius 0.15 and bins each photon by the pinhole projection of its last
surface point, using RISE's pixel centres (`ndc = col/16 - 1`; the DL-375 mask
function assumes `col + 0.5`, see DL-368). The interior walk is
`SampleExit`'s own loop: one iteration per scatter or boundary event, capped
at 64 iterations. Only the two boundary ends vary between modes:

| mode | entry | exit |
|---|---|---|
| 0 physics | refracted (`Ft` coin) | refracted (internal coin) |
| 2 light-family model | refracted | cosine, weight `Ft/c` |
| 3 eye-family model | Lambertian: cosine inward, weight `n^2 (1 - F_int)/c` (mean 1, the light-tracing adjoint of PT's NEE end) | refracted |
| 4 symmetric | Lambertian | cosine |

The boundary is smooth (roughness 0): the walk ignores roughness, and the MC
then needs no microfacet model. Tally A drops photons whose first interaction
is a specular reflection, which is the class PT reaches. Tally B keeps them
except the direct light→mirror→camera glint, which is the class the light
family reaches.

**Harness control.** For a Lambertian sphere (mode 1, 1.6e9 photons) the MC
reads sphere 0.001125 ± 0.000010 and wall 0.014637 ± 0.000038. RISE PT
(1024 spp × 4) reads 0.0011455 and 0.0146891. Before the pixel-centre
correction the sphere read 2.5× off. The sphere region is edge-dominated,
which is why the pixel convention matters so much there.

## 4. Results (roughness 0; MC 4e8 photons, ± is the standard error over 16 batches)

| | sphere | wall |
|---|---|---|
| physics, A / B | 0.11862 ± 0.00042 / 0.11864 | 0.018984 ± 0.00008 / 0.019436 |
| light model (mode 2), B | 0.10519 ± 0.00047 | 0.019601 ± 0.00012 |
| eye model (mode 3), A | 0.09740 ± 0.00064 | 0.019723 ± 0.00014 |
| symmetric (mode 4), A | 0.08589 ± 0.00055 | 0.019969 ± 0.00014 |
| RISE PT, no glass (n 4, 1024 spp) | 0.09842 (sd 0.00043) | 0.019664 (sd 0.00009) |
| RISE BDPT G-RW | 0.10390 (sd 0.0016) | 0.019260 (sd 0.00024) |
| RISE VCM merging off G-RW | 0.10575 (sd 0.0015) | 0.019151 (sd 0.00013) |

- **Each integrator matches its own model.** PT agrees with the eye-model MC
  to +1.0 % on the sphere and -0.3 % on the wall. BDPT and VCM agree with the
  light-model MC to -1.2 % / +0.5 % on the sphere. Their walls read 1.7 % and
  2.3 % low (see §6).
- **Physics is above both models on the sphere.** The light model reads
  -11.3 % and PT's eye model -17.9 %. On the wall, physics A is 0.018984;
  the light model reads +3.25 % against physics A (+0.9 % against physics B, its own tally) and the eye model +3.9 %. **On this fixture
  the light family is the closer one.** The light enters at grazing
  incidence, and the eye model drops the incidence-angle dependence at the
  light end.
- A reciprocal model that is Lambertian at both ends would make the families
  agree at -27.6 % on the sphere. That is further from physics than either
  current model, and it would change PT's look.

## 5. Ruling

DL-381 is not an estimator defect.

- Changing the light family toward PT's number would move BDPT/VCM away from
  physics.
- The light family cannot estimate PT's function exactly for a pinhole. That
  would need an exact refracted exit toward the camera from an interior vertex,
  which is a specular-manifold problem.
- The `n^2` bookkeeping is consistent: mode 3 needs `n^2` at its Lambertian
  entry to reproduce PT, and mode 2 needs none to reproduce BDPT. That matches
  DL-04's telescoping.

The material model's own error at its cosine end is filed separately as DL-384.
Improving the exit lobe at both ends would shrink both the physics error and
the non-reciprocity.

`BDPTStrategyBalanceTest --dl381-only` gates the following, all n = 4 salted
at 1024 spp:

- PT on the scene without glass, against the eye-model MC.
- BDPT and VCM (merging off) on G-RW, against the light-model MC.

The sphere band is 3 % and the wall band 4 %. If the light family reproduced
PT's function, its sphere would read -7.4 % and fail the band. The symmetric
model would read -18 % and fail too.

## 6. Unconfirmed residuals (not filed)

- BDPT and VCM walls read 1.3 % to 2.3 % below the light-model tally B in all
  four measurements (z of roughly 1 to 3.4).
- Removing the subsurface entirely (an absorbing interior) leaves a
  wall-only specular caustic. BDPT reads 0.000413 (sd 0.000018, n 8, 2048 spp)
  against the MC's 0.000432 ± 0.000006, which is -4.4 % at z of about 2.2.
- Both are within about 2σ. More renders are needed before calling either one
  a defect.


## Review correction (2026-10-02): the error's sign is scene-dependent

The fresh review re-ran the standalone tracer (cap lifted to 1e5 so the
default `max_bounces 64` truncation, ~1.8 % of the sphere's energy here,
does not enter) and showed the exit-lobe error is an ANGULAR
REDISTRIBUTION, not a loss: each exit keeps exactly the physical flux (the
Fresnel coin plus a mean-1 cosine lobe), and when the internal radiance is
isotropic the model equals exact transport. Its sign follows the light
geometry:

| fixture | PT (eye model) vs physics | light model vs physics |
|---|---|---|
| this doc's back-lit grazing fixture, sigma_s 10 | -17.5 % | -11.1 % |
| same sphere FRONT-lit (omni at (1.2, 0.8, 2.5)), sigma_s 10 | +8.5 % | +5.2 % |
| front-lit, sigma_s 100 (optically thick) | +1.5 % +/- 0.45 | +0.2 % |
| back-lit, sigma_s 100, sigma_a 0.1 | -3.0 % +/- 3.2 | -5.5 % +/- 3.1 |

So PT is not uniformly dark: thin / low-albedo translucent objects lit
from behind read dark, lit from the front read bright, and optically thick
objects are within a few percent. The Lambertian exit is a documented
design choice (`RandomWalkSSS.cpp`, "standard approach ... Chiang & Burley
2016"), not an oversight. A truly exact refracted exit is not available to
NEE-based PT for point lights (it is a delta lobe), so the buildable
options for DL-384 are: keep the status quo; Lambertian at both ends
(families agree, further from physics: -27.6 % back-lit, +15.3 %
front-lit); or a non-delta exit lobe shaped by the walk's arrival direction
at both ends (moves both families toward physics and each other; changes
PT's look in a scene-dependent direction).

**Ruling (user, 2026-10-02): status quo.** The Lambertian exit lobe stays; DL-384 is closed. Reopen only with a concrete scene where the scene-dependent error matters.

## DL-368 re-baseline (2026-10-02, `debt-dl354`)

The MC bins photons by RISE's pixel centres, and DL-368 moved them from
ndc `col/16 - 1` to `(col + 0.5)/16 - 1` (PBRT's convention; every render
shifted half a pixel).  The tool now uses the new centres and every
constant above moved, by more than the bands on the sphere (the region is
edge-dominated, section 3): with the old constants the gate read PT sphere
+9.5 % and BDPT sphere +8.6 % (bands 4 %).  Rerun, 4e8 photons:

| | sphere | wall |
|---|---|---|
| physics (mode 0), A / B | 0.12802 ± 0.00047 / 0.12804 | 0.019449 ± 0.00008 / 0.019891 |
| light model (mode 2), B | 0.11241 ± 0.00050 | 0.020129 ± 0.00012 |
| eye model (mode 3), A | 0.10568 ± 0.00061 | 0.020096 ± 0.00014 |
| symmetric (mode 4), A | 0.09156 ± 0.00047 | 0.020484 ± 0.00012 |
| RISE PT, no glass (gate run, n 4, 1024 spp) | 0.10669 | 0.020131 |
| RISE BDPT G-RW (gate run, n 4) | 0.11426 | 0.019951 |
| RISE VCM merging off G-RW (gate run, n 4) | 0.10890 | 0.020019 |
| RISE BDPT G-RW (`--dl381-probe`, n 6) | 0.11324 (sd 0.0045) | 0.020204 (sd 0.00034) |
| RISE VCM merging off G-RW (`--dl381-probe`, n 6) | 0.11152 (sd 0.0019) | 0.020435 (sd 0.00064) |

The conclusions stand: PT matches the eye model (+1.0 % / +0.2 %), BDPT
the light model (gate run +1.6 % / -0.9 %; probe +0.7 % / +0.4 %), VCM
reads -3.1 % / -0.5 % in the gate run and -0.8 % / +1.5 % in the probe
(per-render sd 2-4 %, so one n = 4 run moves by ~2 %), and the light model (-12 %) is closer
to physics than the eye model (-17 %).
