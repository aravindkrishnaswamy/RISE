# DL-348: one light-selection density per strategy family

**Status:** fixed 2026-10-02 (`debt-dl348`), pending review.
**Symptom (row):** VCM, and to a lesser degree BDPT, read low in scenes with
several luminaries; the deficit grew with the number of lights, not their
size, and the same quads merged into ONE luminary were clean.

## 1. What master looked like at the start of this slice

The row's suspected main term -- `pdfSelect` divided out of
`EvaluateS0Impl`'s `wCamera` and out of `InitLight`'s `dVC` while the MIS
partners kept it -- had already been removed by DL-346 (`8f84d713a`,
`docs/DL346_ENVIRONMENT_MIS_MEASURE.md` §"VCM and light selection"): both
now carry the selected joint densities exactly as SmallVCM's
`GenerateLightSample` / `GetLightRadiance` do (`directPdfA *= lightPickProb;
emissionPdfW *= lightPickProb`).  A deficit nevertheless remained on master
`a74c400b2` (§3 table, "master" column): four equal quads read VCM 0.922 /
BDPT 0.956 of the closed form under an orthographic camera.

## 2. Derivation

Index a path `x_0 ... x_k` with `x_0` on emitter `L`.  Every BDPT/VCM
strategy that ROOTS a light subpath -- NEE (s = 1), light tracing (t = 1),
the s >= 2 connections and VCM merging -- selects `L` with
`LightSampler::SampleLight()`, a single env-vs-alias roll whose pmf is
`q(L) = (1 - q_env) a(L)`, independent of any shading point.  The light
endpoint therefore carries the area density `q(L) p_A(x_0)` in every one of
those strategies, and the balance (VCM) or power (BDPT) weight of the
eye-hits-emitter strategy (s = 0) is

    w_0 = p_0^b / ( p_0^b + p_1^b + ... ),   p_1 / p_0 = q(L) p_A(x_0) / p_bsdf->A(x_0 | x_{k-1}) .

The s = 0 estimator cannot draw `p_1`; it must reconstruct it, and it did so
through `LightSampler::PdfSelectLuminary` (VCM `EvaluateS0Impl`, BDPT's
s = 0 `eyeEnd.pdfRev`).  That query returned the LIGHT BVH's
shading-point-dependent pmf `q_B(L | x_{k-1})` whenever the BVH was built --
and `light_bvh` defaults TRUE (`StabilityConfig`) and the BVH is built for
two or more lights.  The s = 1 estimator, meanwhile, evaluates its own
weight with the `q(L)` it actually sampled.  With the balance heuristic and
just those two strategies, `r = p_A / p_bsdf->A`:

    w_0 + w_1 = 1/(1 + q_B r) + q r/(1 + q r)  !=  1   unless q_B = q .

The BVH concentrates mass on the lights that matter at `x_{k-1}`, i.e. on
the light a BSDF-sampled continuation actually hits, so `q_B > q` on the
paths that carry energy and the sum is below one: the render reads low,
more so with more lights, and not at all with one light (no BVH) or with
one multi-quad mesh luminary (one table entry) -- the row's discriminator.
The two reference conventions agree with the fix: SmallVCM's
`lightPickProb` is the same uniform pmf in `GenerateLightSample`,
`DirectIllumination` and `GetLightRadiance`, and PBRT-v4's BDPT evaluates
its MIS light-selection PMF with the same context-free light sampler that
roots its light subpaths.  PT is a different family: its NEE samples the
BVH (`EvaluateDirectLighting`), and its BSDF-hit partner,
`CachedPdfSelectLuminary`, correctly stays BVH-based.

**Fix.** `PdfSelectLight` / `PdfSelectLuminary` return `SampleLight()`'s own
pmf, `(1 - q_env) a(L)`, and never consult the BVH (the shading-point
arguments are kept, unused, so callers still state where the alternative
would have selected from).  They have exactly two callers, VCM
`EvaluateS0Impl` and BDPT's s = 0 branch; MLT inherits BDPT.  No density
in the recurrence, the NEE weight or the light subpath changed.

A side effect worth knowing: the BDPT comment about a BVH pmf of zero for
an in-set emitter (an orientation-zeroed cluster) no longer applies -- the
alias pmf is strictly positive for every in-set light.

## 3. Evidence (`VCMStrategyBalanceTest` topology Y)

Lambertian floor rho 0.5 under N equal, single-sided, face-down 0.8 x 0.8
quads (exitance 6) at height 1.4; orthographic camera over [-2, 2]^2, so
the image is `rho/pi * mean(E)` with the analytic rectangle form factor
(cross-checked against quadrature in the test).  32 x 32, 64 spp, n = 4
salted renders per estimator (`SobolSamplerTestHooks::ValueSalt`);
ratio to the closed form (pinhole and environment rows: to PT).  The
master column is an isolated A/B (`LightSampler.{h,cpp}` reverted to
`a74c400b2`, everything else at the fix, committed test).

| row | estimator | master | fix |
|---|---|---|---|
| 1 luminary | VCM / BDPT | 1.0002 / 0.9994 | 0.9998 / 0.9995 |
| 2 luminaries | VCM / BDPT | **0.9725** / **0.9917** | 0.9980 / 0.9989 |
| 4 luminaries | VCM / BDPT | **0.9215** / **0.9565** | 0.9982 / 0.9988 |
| 4 quads, ONE mesh luminary | VCM / BDPT | 0.9976 / 0.9984 | 0.9981 / 0.9983 |
| 4 luminaries, pinhole vs PT | VCM / BDPT | **0.9801** / **0.9918** | 1.0010 / 1.0006 |
| env + 4 luminaries vs PT | VCM / BDPT | 0.9972 / 1.0062 | 0.9974 / 1.0052 |

PT reads 0.9987-0.9995 of the closed form throughout (a constant ~0.1 %,
identical in both builds).  Per-render sd <= 0.2 % (closed-form rows),
<= 1.2 % (BDPT environment row).  Bands: 0.6 % (closed form / pinhole),
2 % (environment).  The suite reads 13/6 on the master library and 19/0
on the fix.  The environment row is a non-regression control, not a red
row: the environment takes most of `SampleLight`'s selection mass, NEE to a
quad is rare, and the eye-hit strategy carries the quads at weight ~1 under
either pmf; it pins the `(1 - q_env)` share the fix keeps.

**BDPT** carried the same defect through the same query (it is the row's
"BDPT -2.0 % with four lights"), smaller because its power heuristic
saturates `w_0` toward 1 sooner than VCM's balance heuristic.  The row's
"-3.5 % unexplained remainder" after its `pdfSelect` patch is this term
(measured on the reviewer's own fixture, not re-run here).  DL-368's
half-pixel offset is not involved: pinhole and orthographic rows close
alike.

## 4. Shipped scene

`scenes/Tests/VCM/triplecaustic_vcm.RISEscene` (three separate emitter
panels): see the ledger row for the before/after salted means.  Its
luminaire `scale` was matched in DL-320 to its pre-DL-320 VCM look, which
carried this bias; the scene now renders brighter by the amount recorded
there and was not re-tuned.
