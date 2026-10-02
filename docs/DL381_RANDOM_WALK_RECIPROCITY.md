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
  the light model reads +0.9 % and the eye model +3.9 %. **On this fixture
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
