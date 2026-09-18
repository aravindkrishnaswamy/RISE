# DL-123: `GGXBRDF::hemisphericalAlbedo{,NM}` ignored roughness entirely

Status: **CLOSED** 2026-09-17 (slice `debt-dl123`, branched from `master` `e290fc64`).

## The bug, in one sentence

`GGXBRDF::hemisphericalAlbedo{,NM}` returned `interfaceFresnel.Mean() + diffuse*Transmission(mean,mean)`
-- the FLAT macro-interface Fresnel hemispherical average, with **zero**
dependence on `alphaX`/`alphaY` (roughness) -- when the true bihemispherical
specular albedo of a rough GGX surface is alpha-dependent through both its
single-scatter lobe shape and the Kulla-Conty multiscatter compensation
`value()` itself adds.

Measured (`tests/FabricMaterialChunkTest.cpp` gate 5(b), Schlick F0=0.04):
GGX alpha=0.2 substrate error +4.188% to +7.730% at alpha=0.5 -- the SAME
order of magnitude as the now-fixed DL-07 (`OrenNayarBRDF`) bias, but a
structurally different mechanism (`Mean()` is a genuine hemispherical-
Fresnel-average estimator, not "return the input verbatim").

## Why the bug was invisible for a long time

`hemisphericalAlbedo` is IBSDF's "reflectance under a uniform incident
field" -- a view-INDEPENDENT quantity consumed only by two wrappers'
energy bookkeeping (`fabric_material`'s sheen energy-subtraction,
`coated_material`'s Saunderson recycling denominator `1/(1 - r_i*R)`),
never by the render-visible `value()`/`Scatter()` paths directly. A GGX
substrate under either wrapper still looked plausible -- a few percent
too much recycled light at grazing/rough configurations reads as "a bit
brighter", not as an obviously wrong image -- and DL-07's own fix (which
targeted the exact same consumer shape for `OrenNayarBRDF`) surfaced GGX
as "now the dominant residual" only because it was independently gated
by the SAME `FabricMaterialChunkTest` gate 5(b) table.

## Derivation

Write `GGXBRDF::value()`'s specular contribution as three additive
pieces and integrate each one against IBSDF's bihemispherical measure

```
R_bi = (1/pi) INT_H INT_H f(wi,wo) (n.wi)(n.wo) dwi dwo
```

**1. Single-scatter, `R_ss`.** `specFactor = D(h)*G2(wi,wo)/(4*cosI*cosO)`,
so the `1/(4*cosI*cosO)` term cancels the measure's `(n.wi)(n.wo)` factor
exactly (the same cancellation `OrenNayarHemisphericalAlbedoGen.cpp`'s
header derives for its own BRDF), leaving

```
R_ss = (1/pi) INT_H INT_H F(cosThetaH) * D(h)*G2(wi,wo)/4  dwi dwo
```

a 1-D integral in `u = 1 - cosThetaH` against a fixed (Fresnel-independent)
GGX kernel `K(alpha, u)`:

```
R_ss = INT_0^1 F(1-u) * K(alpha, u) du
```

`tools/GGXSpecularBihemisphericalGen.cpp` bakes this NOT as the kernel
itself but as its first 8 MOMENTS against the monomial basis
`{u^0, ..., u^7}`, then solves ONE fixed (alpha-independent) Vandermonde
system to convert those moments into per-node WEIGHTS `w_i(alpha)` at 8
Chebyshev-Gauss-Lobatto nodes `u_i` -- so at runtime, for ANY Fresnel
function `F` (Schlick, conductor, thin-film; `F` need not be known at
bake time):

```
R_ss(alpha) ~= SUM_i F(1 - u_i) * w_i(alpha)
```

This is algebraically EXACT for Schlick (`F(u) = F0 + (1-F0)*u^5` is
degree 5, inside the baked degree-7 basis) and a degree-7-polynomial-fit
approximation for conductor/thin-film (measured error <0.04% in
`tests/GGXHemisphericalAlbedoTest.cpp`'s conductor rows -- see
"Measured results" below).

**2. Multiscatter, `R_ms`.** `value()`'s `f_ms = (1-Ess_o)(1-Ess_i)/(pi*(1-Eavg))`
is SEPARABLE in `(wi,wo)` (`Ess_o` depends only on `wo`, `Ess_i` only on
`wi`), and `F_ms` is a bihemispherical constant (built from `F_avg` and
`Eavg` alone), so

```
INT_H (1-Ess_o) cosO dwo = INT_H (1-Ess_i) cosI dwi = pi*(1-Eavg)
```

(`Eavg`'s own definition -- `2 INT E_ss(alpha,mu)*mu dmu`), giving,
EXACTLY,

```
R_ms = (F_ms/pi) * (1/(pi*(1-Eavg))) * [pi*(1-Eavg)]^2 = F_ms * (1-Eavg)
```

**No new baking is needed for this term at all** -- it reuses the exact
same `LookupEavgG2Aniso` lookup and the exact same per-mode `F_avg`
construction (`SchlickFresnelAvg` / `ThinFilm::FresnelAvgConductor{,RGBSpectral}` /
`MicrofacetEnergyLUT::ComputeFresnelAvg`, then `ComputeFms`) that
`value()`/`valueNM` themselves use, so it cannot drift from what actually
renders. `ComputeGGXFms`/`ComputeGGXFmsNM` in `GGXBRDF.cpp` are literal
replays of `value()`'s own per-mode branches.

**3. Diffuse, `R_diff`.** Already exact pre-fix: `diffuse = c*INV_PI*
(1-A(nv))(1-A(nr))` is separable, so `R_diff = c*(1-Ā)^2` with
`Ā = interfaceFresnel.Mean()` the cosine-weighted average of the SAME
flat `Directional()` function `value()`'s own diffuse term uses.
Unchanged by this fix.

```
R_bi = R_ss + R_ms + R_diff
```

At `alpha -> 0` (mirror limit), `R_ss -> Mean()` (the moment-matched
quadrature's own smooth-limit identity -- see the generator's header)
and `R_ms -> F_ms*(1-Eavg) -> 0` (`Eavg -> ~1`), recovering the pre-fix
answer exactly: `tests/GGXHemisphericalAlbedoTest.cpp`'s
`TestSmoothLimit` pins this at alpha=0.01.

## Why a moment-matched fixed-node quadrature, not a direct bake of `K`

A first cut at the generator used plain deterministic midpoint
quadrature over `(mu_i, mu_o, deltaPhi)`, mirroring
`OrenNayarHemisphericalAlbedoGen.cpp`'s own style -- and it FAILED at low
alpha (moment 0 at alpha=0.01 measured 0.931 vs the trusted, independently-
baked `E_avg_TABLE_G2`'s 0.999, a 7% deficit) because GGX's `D` is a
sharply peaked function at low roughness that a uniform angular grid at
any practical resolution cannot resolve. The shipped generator instead
reuses the SAME VNDF (visible-normal-distribution) importance-sampling
estimator `GenerateMicrofacetEnergyLUT.cpp`'s own `E_ss`/`E_avg` bake
uses (see that file's `main()`): for a fixed `wi`, `VNDF_Sample_Local`
draws a microfacet normal `m` (== the half-vector `h`) from the visible-
normal distribution; `muH = dot(wi,m)` falls out of the sampler directly,
and the height-correlated per-sample weight `G2(wi,wo)/G1(wi)` (identical
to that file's own `sumG2` line) turns a VNDF-sampled average into an
unbiased estimator that resolves the `D` peak automatically regardless of
alpha. Re-running this estimator: moment 0 at alpha=0.01 now reads
0.999485 against `E_avg_TABLE_G2[0]`'s 0.99947210 (diff 1.3e-5), and the
worst disagreement across the whole 32-node alpha axis is 3.4e-4 absolute
-- this is the cross-check `tests/GGXHemisphericalAlbedoTest.cpp`'s
`TestMomentZeroCrossCheck` exercises.

## Construction chosen and its cost

- **One new baked table**: `kGGXSpecularQuadWeight[32][8]` (32 isotropic
  alpha nodes, matching `E_avg_TABLE_G2`'s own axis exactly; 8 Chebyshev
  nodes) plus the 8 fixed `kGGXSpecularQuadNodes`. ~1KB of floats,
  embedded directly in `GGXBRDF.cpp`'s anonymous namespace (same "paste
  the generator's output" idiom as `OrenNayarBRDF.cpp`'s `kOrenNayarA1Table`/
  `kOrenNayarA2Table`).
- **Runtime cost**: `hemisphericalAlbedo`/`hemisphericalAlbedoNM` now do
  8 calls to `interfaceFresnel.Directional{,NM}` (the SAME per-mode
  Fresnel evaluator `value()` itself uses) plus one `LookupEavgG2Aniso`
  lookup and one small `ComputeFms` call -- comparable to, and for
  Schlick/conductor *cheaper* than, `Mean()`'s own pre-existing GL_N=21-node
  quadrature for the thin-film/conductor branches. `hemisphericalAlbedo`
  is called once per BSDF energy-bookkeeping query (not inside any inner
  Monte-Carlo sampling loop), so this cost is negligible.
- **No new bake for `R_ms`** -- see derivation step 2. This is the reason
  the fix is cheap despite covering all three Fresnel modes uniformly.

## Residual: anisotropic `R_ss`

`R_ms`/`Eavg` use the EXACT anisotropic `LookupEavgG2Aniso(alphaX,alphaY)`
-- no approximation there. `R_ss`'s baked table is ISOTROPIC-only;
anisotropic configurations (`alphaX != alphaY`) evaluate it at
`alphaEff = sqrt(alphaX*alphaY)`, the same "collapse to the isotropic
effective roughness" approximation this codebase already uses in several
other places pre-DL-77 (and which DL-77 later extended to a full 3-axis
table for the *directional* `Ess`/`Eavg` energy-compensation lookups).
Measured (`tests/GGXHemisphericalAlbedoTest.cpp`'s `TestAnisotropicFallback`,
alphaX=0.05/alphaY=0.5): reported 0.52971 vs a brute-force
0.47353 -- an 11.9% relative residual. Filed as **DL-139** rather than
extending `kGGXSpecularQuadWeight` to a third (ratio) axis in this slice
(the same isotropic-first, aniso-follow-up sequencing DL-63 -> DL-77
already used for the directional energy-compensation LUTs).

## Measured results

`tests/GGXHemisphericalAlbedoTest.cpp` (red-proof against the unfixed
library: 16/28 checks failed, e.g. alpha=1.00 F0=0.50: reported 0.52381
vs true 0.30200, a 0.222 absolute miss; conductor alpha=1.00: reported
0.79742 vs true 0.61478). Post-fix: **28/28 checks pass**, worst residual
0.0067 absolute (alpha=0.05, F0=0.04 -- a moderate roughness/low-reflectance
corner well inside the 0.01-0.015 test tolerances).

`FabricMaterialChunkTest` gate 5(b) ("substr%" column, the GGX rows the
DL-123 ledger row was itself measured from):

| substrate | m | before (DL-123 open) | after (this fix) |
|---|---|---|---|
| GGX alpha=0.2 | 0.5 | (row not separately printed pre-fix; class measured 4.188-7.730%) | +0.026% |
| GGX alpha=0.2 | 1.0 | | +0.026% |
| GGX alpha=0.5 | 0.5 | | +0.007% |
| GGX alpha=0.5 | 1.0 | +7.730% (the ledger row's own cited worst case) | +0.007% |

All four rows move from the ledger's measured 4.2-7.7% down to
0.007-0.026% -- comfortably inside the 0.5% target this row's recipe
asked for.

`LayeredWhiteFurnaceTest` configs 14/15 ("Coated clearcoat / white
GGX-PBR" and "Coated clearcoat / red GGX-PBR") are documented MEASURED
REGRESSION PINS, re-measured the same way DL-37 previously moved them
(same 100k-draw driver, same 0.005 tolerance):

| config | before | after |
|---|---|---|
| 14. white GGX-PBR | {0.8398, 0.8409, 0.8328, 0.5791} | {0.8171, 0.8184, 0.8113, 0.5680} |
| 15. red GGX-PBR   | {0.5695, 0.5704, 0.5812, 0.4611} | {0.5571, 0.5580, 0.5692, 0.4547} |

Both move DOWN, exactly as expected: a smaller (more correct) substrate
hemispherical estimate means a smaller Saunderson recycling boost.
`LayeredWhiteFurnaceTest`: 0 of 57 configurations failed (was 2 of 57
with the stale pre-DL-123 pins left in place against the fixed library).

Real-render confirmation: `scenes/FeatureBased/Materials/lacquer_and_rain_still_life.RISEscene`'s
`mat_worn_lacquer_brass` (`coated_material` over `mat_brass_dry`, a
`ggx_material` alphaX=alphaY=0.24, `fresnel_mode schlick_f0`) rendered
before/after at production settings (`pathtracing_pel_rasterizer`,
samples=96, `oidn_denoise FALSE`, EXR `Rec709RGB_Linear`) -- see the
DL-123 ledger row closure text for the measured means (isolated to this
build; not re-run per subsequent unrelated commits).

Unaffected (audited, not fixed): `LambertianBRDF::hemisphericalAlbedo`
is exact by construction (a Lambertian BRDF's bihemispherical albedo is
its reflectance, trivially, at every direction). `WeaveBRDF::hemisphericalAlbedo`
implements a structurally different (D'Eon yarn-model) bihemispherical
estimator with its own already-reviewed "energy-bounded, not energy-
conserving" caveats and its own dedicated furnace rows (all already
passing, unchanged by this fix). `CookTorranceBRDF`, `SchlickBRDF`,
`TranslucentSPF`, `IsotropicPhongSPF` (and the Ward/Phong/Ashikmin/
Polished-family SPFs, under whatever file names they actually carry in
this tree) do not override `hemisphericalAlbedo` at all -- they inherit
`IBSDF`'s `return false` default, and are correspondingly absent from
`coated_material`'s and `fabric_material`'s substrate allowlists (`IBSDF.h`'s
own doc comment: "the FALSE path is unreachable" for the allowlisted
callers), so DL-123's specific bug pattern (a wrong VALUE) cannot occur
for them today.

## Verification recipe

```
make -C build/make/rise build-test/GGXHemisphericalAlbedoTest
./bin/tests/GGXHemisphericalAlbedoTest   # 28 checks, 0 failures

make -C build/make/rise build-test/FabricMaterialChunkTest
export RISE_MEDIA_PATH="$(pwd)/"
./bin/tests/FabricMaterialChunkTest      # gate 5(b) GGX substr% rows <0.03%

make -C build/make/rise build-test/LayeredWhiteFurnaceTest
./bin/tests/LayeredWhiteFurnaceTest      # 0 of 57 configurations failed
```

Regenerating the baked table (only needed if the generator's own
derivation changes):

```
c++ -O3 -std=c++17 -o /tmp/GGXSpecularBihemisphericalGen \
    tools/GGXSpecularBihemisphericalGen.cpp
/tmp/GGXSpecularBihemisphericalGen
# paste kGGXSpecularQuadNumNodes / kGGXSpecularQuadNodes /
# kGGXSpecularQuadWeight back into GGXBRDF.cpp's anonymous namespace
```
