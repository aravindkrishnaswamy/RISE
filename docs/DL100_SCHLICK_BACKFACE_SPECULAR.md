# DL-100 — Schlick/Ward specular sampler built its half-vector in the wrong (unflipped) frame

Status: **CLOSED 2026-09-17**, branch `debt-dl100`.

## The bug pattern, one sentence

The specular half-vector sampler built `h` in the RAW, unflipped `ri.onb`
while `Scatter`'s own accept-check tested the FlipW'd `myonb.w()` — so on
any back-face hit (a single-sided surface struck from behind; double-sided
meshes pre-flip `vNormal` and never reach this branch) the sampled
direction landed on the wrong side of the accept-check and was rejected
every time, silently dropping the entire specular lobe.

## Where it lived

`SchlickSPF.cpp`'s `Scatter`/`ScatterNM` build `myonb` by copying `ri.onb`
and calling `FlipW()` whenever `dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO`,
then accept-check every scattered ray against `myonb.w()`. Before this fix,
`GenerateSpecularRay` had no frame parameter at all and its helper
`SchlickSampleHalfVector` read `ri.onb.u()/v()/w()` directly. On a
back-face hit the reflected direction it produced pointed opposite
`myonb.w()`, so `dot(dir, myonb.w()) > 0` failed for every draw — a
back-face `schlick_material` hit scattered diffuse-only, at full diffuse
weight, no specular energy at all.

`WardIsotropicGaussianSPF.cpp`'s `GenerateSpecularRay` carried the exact
same shape of bug, one step more subtle: it already took an `onb`
parameter (and every call site already passed `myonb`), but the function
body never used it — it read `ri.onb.u()/v()/w()` regardless, so the
parameter was dead. Same consequence: zero specular emission on a
back-face hit.

`WardAnisotropicEllipticalGaussianSPF.cpp` was audited and found ALREADY
CORRECT — its `GenerateSpecularRay` builds `h` from the `onb` parameter
it is handed, not from `ri.onb`.

## The fix

Thread the caller's sampling frame (`myonb`) through every function that
builds, inverts, or evaluates the density of the half-vector, so the
sampler and its `Pdf()` always agree on which frame is in use:

- `SchlickSPF.cpp`: `SchlickSampleHalfVector`, `GenerateSpecularRay`,
  `ComputeSchlickSpecularPdf`, `SchlickInvertSpecular`,
  `SchlickReplaySpecular` (already had `myonb`, just wasn't passing it to
  `SchlickSampleHalfVector`), and `SchlickDiffuseSelectCoefficient`'s
  internal quadrature-to-world conversion (`eu/ev/ew`) all now take/use
  `myonb` (or the caller's `onb` parameter) instead of `ri.onb`. Every
  call site in `Scatter`, `ScatterNM`, `Pdf`, `PdfNM`, and
  `SchlickSpecularDensity` was updated to pass it through.
- `WardIsotropicGaussianSPF.cpp`: `GenerateSpecularRay`'s half-vector
  construction now reads the `onb` parameter it already had instead of
  `ri.onb`. `WardIsotropicPdf` needed NO change — it recovers `h` from
  `wi+wo` directly (never from onb-relative axes), and its `n = myonb.w()`
  was already correct, so it was never wrong about the DENSITY, only the
  SAMPLER's frame was wrong.
- `WardAnisotropicEllipticalGaussianSPF.cpp`: no change (already correct).
- `AshikminShirleyAnisotropicPhongSPF.cpp`: same one-sentence pattern
  (confirmed by reading), but a concurrent sibling slice (the "phongpdf"
  slice named in this session's brief) owns that file's fix; left
  untouched here to avoid a duplicate/conflicting edit.
- `IsotropicPhongSPF.cpp`, `CookTorranceSPF.cpp`, `GGXSPF.cpp`,
  `CoatedSPF.cpp`, `PolishedSPF.cpp`: audited, confirmed IMMUNE. The Phong
  file perturbs an already-correctly-oriented `reflected` vector rather
  than building `h` from onb axes; the other four select their one lobe
  internally and never hand a frame to a separate free function.

## Red-proof

`tests/SchlickWardBackfaceEnergyTest.cpp`. Method: two intersections that
are exact mirror images of each other under `z -> -z` — a FRONT-face hit
at incidence `theta` (every other Schlick/Ward test's convention:
`ri.onb.w() = (0,0,1)`) and a BACK-face hit at the SAME `theta` (same
unflipped normal, struck from behind, so `myonb.FlipW()` fires). For an
ISOTROPIC material the two are physically identical, so the measured
fraction of `Scatter()` calls that emit a specular (`eRayReflection`) ray
must agree within Monte-Carlo noise.

Pre-fix (both `SchlickSPF.cpp` and `WardIsotropicGaussianSPF.cpp` reverted
to their pre-slice state, isolated build via `git checkout HEAD --
<file>` + rebuild, no other tree changes):

```
-- SchlickSPF: front vs back-face specular emission rate --
  theta=10  front=0.767885  back=0  |diff|=0.767885   FAIL
  theta=30  front=0.742985  back=0  |diff|=0.742985   FAIL
  theta=45  front=0.70619   back=0  |diff|=0.70619    FAIL
  theta=60  front=0.651785  back=0  |diff|=0.651785   FAIL
  NM theta=45           front=0.70498   back=0  |diff|=0.70498   FAIL
  per-channel theta=45  front=0.76647   back=0  |diff|=0.76647   FAIL
  Pdf mass check (back-face, theta=30):  intPdf=1  emitted=1  |diff|=1.15463e-14   (passes: the sampler and Pdf were symmetrically wrong)

-- WardIsotropicGaussianSPF: front vs back-face specular emission rate --
  theta=10  front=1         back=0  |diff|=1          FAIL
  theta=30  front=0.999975  back=0  |diff|=0.999975   FAIL
  theta=45  front=0.997975  back=0  |diff|=0.997975   FAIL
  theta=60  front=0.968895  back=0  |diff|=0.968895   FAIL

Checks: 19 Failures: 10
```

Note the Schlick "Pdf mass check" row PASSES pre-fix: before the fix,
`ComputeSchlickSpecularPdf` read `ri.onb` directly (the same unflipped
frame the pre-fix sampler used), so `Pdf()` and `Scatter()` were
symmetrically wrong and agreed with each other on the wrong answer — a
reminder that a normalisation check alone cannot catch a whole-frame
error; only comparing against a known-physical reference (the mirrored
front-face configuration) can.

Post-fix, both files:

```
-- SchlickSPF: front vs back-face specular emission rate --
  theta=10  front=0.767885  back=0.767115  |diff|=0.00077
  theta=30  front=0.742985  back=0.741255  |diff|=0.00173
  theta=45  front=0.70619   back=0.70619   |diff|=0
  theta=60  front=0.651785  back=0.65198   |diff|=0.000195
  NM theta=45           front=0.70498   back=0.70809   |diff|=0.00311
  per-channel theta=45  front=0.76647   back=0.76555   |diff|=0.00092
  Pdf mass check (back-face, theta=30):  intPdf=0.999974  emitted=1  |diff|=2.5987e-05

-- WardIsotropicGaussianSPF: front vs back-face specular emission rate --
  theta=10  front=1         back=1        |diff|=0
  theta=30  front=0.999975  back=0.99996  |diff|=1.5e-05
  theta=45  front=0.997975  back=0.99806  |diff|=8.5e-05
  theta=60  front=0.968895  back=0.968385 |diff|=0.00051

Checks: 19 Failures: 0
```

`SchlickSPFPdfConsistencyTest` (43/0, unchanged — it exercises front-face
hits only, per its own documented scope) and `SchlickLobePairingTest`
(27/0) both stay green: this fix changes back-face behaviour only, so
every front-face configuration is bit-identical before and after.
`SPFPdfConsistencyTest` and `SPFBSDFConsistencyTest` (which cover Ward
via their own front-face-only fixtures) are also unaffected.

## What did NOT change

`SchlickSPFPdfConsistencyTest`'s `C_D` quadrature (`SchlickDiffuseSelectCoefficient`)
replays the sampler in whatever frame it is handed, so its normalisation
claim ("integral of Pdf equals the measured emission mass") is unaffected
by which frame that happens to be — front-face results are bit-identical
pre/post-fix (confirmed above). DL-101 (the per-channel `ScatteredRay`
reuse bug) is untouched by this fix and remains a `KNOWN-FAILURE` control
in that suite until it is closed separately in this same slice.
