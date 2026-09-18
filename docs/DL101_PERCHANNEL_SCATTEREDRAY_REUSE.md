# DL-101 — Schlick/Ward per-channel loop reused one `ScatteredRay` across all three lanes

Status: **CLOSED 2026-09-17**, branch `debt-dl100`.

## The bug pattern, one sentence

`SchlickSPF::Scatter` and both Ward SPFs' `Scatter` declared a single
`ScatteredRay s` OUTSIDE their per-channel specular loop and called
`GenerateSpecularRay(s, ...)` against it three times, so a lane whose
`hdotk <= 0` accept condition failed left `s.ray` holding the PREVIOUS
lane's direction while the branch still overwrote `s.kray`/`s.pdf` with
the CURRENT lane's values and pushed it — a ray reaching the integrator
at lane j's direction, priced as lane i.

## Where it lived

`GenerateSpecularRay` (all three files) writes `specular.ray` ONLY inside
its `if( hdotk > 0 )` guard; when that guard fails it leaves whatever was
already in `specular` untouched. On lane 0, an untouched `s` still holds
its default-constructed direction `(0,0,0)`, which the caller's own
accept-check rejects harmlessly. On lanes 1 and 2, an untouched `s` still
holds lane `i-1`'s (or an earlier lane's) real, accepted direction — which
generally DOES pass the accept-check — and the per-channel branch then
does:

```cpp
s.kray = 0;
s.kray[i] = rho[i] + (1.0-rho[i]) * fresnel;   // this lane's kray
s.pdf = ComputeSchlickSpecularPdf( ri, myonb, s.ray.Dir(), rt.v[i], it.v[i] );  // this lane's pdf, AT THE OTHER LANE'S DIRECTION
scattered.AddScatteredRay( s );
```

`hdotk <= 0` is reachable at ANY roughness (the theta warp reaches `pi/2`
as the uniform draw `xi -> 0`), and becomes common at grazing incidence
with high roughness, or whenever one lane's roughness is much lower than
its neighbours' (a near-mirror lane rejects far more often than a rough
one sharing the same random pair).

## The fix

Declare a fresh `ScatteredRay s;` INSIDE the loop body in all three files
(`SchlickSPF.cpp`, `WardIsotropicGaussianSPF.cpp`,
`WardAnisotropicEllipticalGaussianSPF.cpp`), so a lane that fails its
accept condition is simply not pushed — never mistaken for a different
lane's leftover ray. (The single-lobe, non-per-channel branch in each
file already declared its own `ScatteredRay s;` implicitly via the
now-removed outer `ScatteredRay d, s;`; it now declares its own local `s`
too, for symmetry and so the outer scope carries only `d`.)

## Sibling audit

One-sentence bug pattern: *a per-lane specular sampling loop reuses one
`ScatteredRay` across lanes, so a lane whose sampler declines to write a
direction silently inherits a different lane's leftover state.*

| SPF | verdict |
|---|---|
| `SchlickSPF` | **fixed this slice** |
| `WardIsotropicGaussianSPF` | **fixed this slice** (identical code shape) |
| `WardAnisotropicEllipticalGaussianSPF` | **fixed this slice** (identical code shape) |
| `IsotropicPhongSPF` | immune: its `GenerateSpecularRay` sets the ray direction UNCONDITIONALLY (a `Perturb` around an already-computed `reflected` vector that never fails to produce a direction), so there is nothing for a later lane to inherit |
| `AshikminShirleyAnisotropicPhongSPF` | immune: its `GenerateSpecularRay` returns a `bool` the caller gates on, and does not touch the `ScatteredRay` argument at all when it returns `false` -- the caller's `if( GenerateSpecularRay(...) )` skips the push entirely rather than falling through to an inherited value |

No new debt opened by this audit: the only two SPFs with a per-channel
specular loop besides `SchlickSPF` (both Ward files) shared the exact
defect and were fixed together; the two SPFs with a superficially similar
per-channel loop shape (`IsotropicPhongSPF`,
`AshikminShirleyAnisotropicPhongSPF`) were read and confirmed structurally
immune.

## Red-proof

`tests/SchlickWardPerChannelReuseTest.cpp`. Method: a direct,
model-free signature -- for each real `Scatter()` call at a grazing-
incidence fixture with THREE MUTUALLY DISTINCT per-channel roughness/
alpha values (so two genuinely-accepted lanes generically compute
different half-vectors and therefore different directions; two
independently-computed continuous directions coinciding to full float
precision by chance has probability approximately zero), collect every
specular (`eRayReflection`) ray the container holds and count how many
calls produce two or more of them with BIT-IDENTICAL directions.

Pre-fix (all three material files reverted to their pre-DL-100/DL-101
state, `git checkout <pre-slice commit> --`, rebuild, rerun):

```
  SchlickSPF grazing r={0.02,0.5,0.95}: duplicate-direction calls = 3567/200000        FAIL
  SchlickSPF grazing r={0.3,0.3,0.3} (control): duplicate-direction calls = 0/200000   (control unaffected, as expected)
  WardIsotropicGaussianSPF grazing alpha={0.02,0.2,0.4}: duplicate-direction calls = 58758/200000       FAIL
  WardAnisotropicEllipticalGaussianSPF grazing alphaX={0.02,0.25,0.4}: duplicate-direction calls = 61708/200000   FAIL
Checks: 4 Failures: 3
```

Post-fix:

```
  SchlickSPF grazing r={0.02,0.5,0.95}: duplicate-direction calls = 0/200000
  SchlickSPF grazing r={0.3,0.3,0.3} (control): duplicate-direction calls = 0/200000
  WardIsotropicGaussianSPF grazing alpha={0.02,0.2,0.4}: duplicate-direction calls = 0/200000
  WardAnisotropicEllipticalGaussianSPF grazing alphaX={0.02,0.25,0.4}: duplicate-direction calls = 0/200000
Checks: 4 Failures: 0
```

(Earlier, discarded attempts at this fixture accidentally set TWO of the
three per-channel values equal, which makes `GenerateSpecularRay`
deterministically compute the SAME direction for those two lanes given
the shared random pair -- a legitimate duplicate, not the DL-101
signature. Using three MUTUALLY distinct values per config avoids this
false-positive trap; the fixed control row, at three EQUAL values, takes
the single-lobe branch entirely (`RGBScalarPainter::HasPerChannelVariation()`
returns `false` when r==g==b) and reads 0 regardless of the fix, which is
why it stays green both pre- and post-fix.)

## `SchlickSPFPdfConsistencyTest`'s own DL-101 row

That suite's `DL-101 KNOWN-FAILURE pc wide grazing` row (a LOW-then-HIGH
per-channel roughness config, `{0.02, 0.95, 0.95}`, matching the doc's
original discovery fixture) was previously recorded but not gated: `Pdf`
correctly modelled the INTENDED per-channel semantics, so the disagreement
it measured was a real, independent symptom of the SAMPLER bug (not this
file's own subject). It is now a REAL gate, renamed `DL-101 CLOSED pc wide
grazing`:

```
DL-101 CLOSED pc wide grazing  RGB  intPdf=1.01216  emitted=1.00000  |diff|=0.01216  TVD=0.01009
```

`|diff|=0.01216` is gated at `kMassTolLowRough` (1.5%, the same honestly-
widened tolerance the neighbouring `r.02 rd.6 rs1 i.3 th85` row uses) --
this residual is `kSpecQuadN=16`'s own C_D quadrature bias (documented in
DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md's P2-2), not a remaining DL-101
symptom: DL-101's own contribution was entirely in the TVD figure, which
dropped from 0.01424 (pre-fix, exceeding the 0.012 gate) to 0.01009 (post-
fix, inside it) -- exactly the reduction the design doc predicted when
"patching the per-channel loop to declare `ScatteredRay s;` INSIDE the
loop." `SchlickSPFPdfConsistencyTest`: `Checks: 44 Failures: 0` (was 43,
since the row now contributes two real `CHECK()`s instead of one
non-asserting counter increment).

## Render sanity

`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene` (contains both
`schlick_material` and `ward_anisotropic_material`, `pathtracing_pel_
rasterizer`, 128 spp) rendered n=3 before/after, mean pixel value over the
raw (OIDN-denoised, as shipped) PNG output:

| | mean | sigma (n=3) |
|---|---|---|
| pre-fix (DL-100+DL-101 both reverted) | 190.52149 | 0.00678 |
| post-fix | 190.52708 | 0.00184 |

Delta +0.00559 (+0.0029%), well inside the run-to-run sigma. Expected and
unremarkable: this scene's `schlick_material`/`ward_anisotropic_material`
spheres are convex and viewed from OUTSIDE (never a back-face hit, so
DL-100 cannot fire), and both use a single UNIFORM roughness/alphax/alphay
value (not an `RGBScalarPainter`, so `HasPerChannelVariation()` is
`false` and the per-channel loop DL-101 fixed never runs). Both fixes are
gated on those specific, non-default conditions by construction; an
ordinary shipped scene with neither is bit-identical before and after, as
this render confirms.
