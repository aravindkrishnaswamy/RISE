# DL-62 / DL-64: GGX glossy-filter roughness and zero-F0 selection weight

Status: **CLOSED 2026-09-13** — source repair `dfdd5ee1`, red proof
`a1db468d`. Independent residual DL-63 (specular-only furnace gain, a
separate G1-vs-G2 masking-model mismatch) was **CLOSED 2026-09-14** by a
follow-up debt-ggx2 slice — see "DL-63" section below. DL-65
(CookTorrance/Schlick glossy-filter sibling, opened by this doc's
original sibling audit) was also **CLOSED 2026-09-13** by that slice
(`0ad80d66`) — see the DL-65 ledger row.

**Review follow-up (2026-09-13, `a495a357`, P2-1/P2-2/P3-x)**: the
initial DL-64 fix called `GGXInterfaceFresnel::Mean()`/`MeanNM()`
unconditionally for the selection weight `ws` in ALL THREE Fresnel
modes; a transport review found this made every `Pdf()`/`Scatter()`
call in `eFresnelConductor` (the parser default) and
`eFresnelThinFilmConductor` pay an unconditional 21-tap
`MicrofacetEnergyLUT::ComputeFresnelAvg` or a 21-node×32-wavelength
Airy quadrature respectively, for a defect (raw F0 collapsing to
exactly zero) that is specific to `eFresnelSchlickF0` and does not
occur in the other two modes (a zero painter tint there already,
correctly, means zero specular energy). Fixed in `a495a357`: `ws`
derives from `Mean()`/`MeanNM()` ONLY in Schlick mode; conductor and
thin-film keep the pre-DL-64 raw-tint weight. See "P2-1: selection-
weight cost" below for the measured cost and "Sibling audit"
correction. This section (through "Test-design fallout") otherwise
describes the ORIGINAL `dfdd5ee1` fix as landed; read the P2-1/P2-2/
P3-x sections at the end for what changed since.

## DL-62: glossy-filter roughness mismatch

`GGXSPF::Scatter`, `ScatterNM`, `Pdf`, and `PdfNM` all widened alphaX/
alphaY by `ri.glossyFilterWidth` (`min(alpha + width, 1)`) before
sampling or evaluating density. `GGXBRDF::value` and `valueNM` floored
authored roughness but never read `ri.glossyFilterWidth` at all. At a
texture-filtered or mip-blurred hit (footprint-driven glossy filtering,
`PathTracingIntegrator.cpp` populates the field), NEE's direct evaluation
of the surface therefore used a SHARPER lobe than the one BSDF sampling
was actually drawing from and reporting density for — a real,
user-visible highlight-sharpness/energy mismatch between a filtered
continuation and its own NEE contribution at the same shading point.

An aggregate hemispherical-energy check cannot see this: Kulla-Conty
multiscatter compensation keeps GGX's total reflected energy close to 1
across nearly the whole alpha range, for both a narrow and a widened
lobe, so a naive "does the total energy change" comparison is nearly
insensitive to the bug. The fix is instead validated deterministically:
construct a second, independent `GGXBRDF` whose alpha painters already
hold the intended effective (pre-widened) roughness with
`glossyFilterWidth=0`, and assert the production object (raw alpha +
nonzero `glossyFilterWidth`) matches it EXACTLY at several probed
directions (isotropic, anisotropic, spec-only, single- and double-axis
saturation at 1.0). Pre-fix this diverges sharply (RGB `maxRelErr` up to
1.910e+01); post-fix every probe matches to `0.000e+00` (bit-identical,
since it is now literally the same formula being evaluated).

Fix: `GGXBRDF::value` and `valueNM` widen `alphaX`/`alphaY` by
`ri.glossyFilterWidth` with the identical `min(alpha+width,1)` convention
GGXSPF already uses, in lockstep at both sites.

## DL-64: zero-F0 Schlick sampling collapse

`GGXSPF::Scatter`/`ScatterNM` (and their `Pdf`/`PdfNM` twins) derived the
specular+multiscatter lobe-selection weight `ws` from
`MaxValue(pSpecular->GetColor(ri))` / `GuardedGetColorNM(*pSpecular,...)`
— the raw authored Schlick F0. At F0=0 this weight is EXACTLY zero, so
`pSpecSelect` and (since `wms = ws*(1-Ess_i)`) `pMSSelect` both collapse
to zero too: `pDiffuseSelect` is pinned to 1.0 (either directly, or via
the `total<1e-10` fallback when diffuse is ALSO authored 0), so every
single `Scatter()`/`ScatterNM()` draw takes the diffuse branch — the
specular `ScatteredRay` type (`eRayReflection`) is never emitted,
deterministically, regardless of RNG seed or sample count.

This is a real physical loss: `GGXBRDF::value`/`valueNM`'s Schlick term
`F0 + (1-F0)(1-cosTheta)^5` is nonzero at grazing incidence even when
F0=0 (a common configuration — a diffuse-free, F0=0 GGX lobe models a
pure Fresnel-edge dielectric-like reflection). BSDF-sampled continuations
can never reach that energy; only NEE (which evaluates `GGXBRDF::value`
directly, unaffected) sees it, producing an MIS imbalance and a visible
energy deficit on BSDF-sampled bounces.

Fix (as originally landed in `dfdd5ee1` — **superseded for two of the
three Fresnel modes by the P2-1 follow-up below**): derive the selection
weight from `GGXInterfaceFresnel::Mean()` / `MeanNM(nm)` — the lobe's
actual hemispherical Fresnel-weighted albedo — instead of raw F0. This
is nonzero even at F0=0 (`SchlickFresnelAvg(0) = 1/21 ≈ 0.048`), so the
specular/MS lobes remain reachable. No magic F0 floor was added — the
fix reuses the same closed-form hemispherical average already used
elsewhere in `GGXBRDF`/`GGXSPF` for the multiscatter tail and the DL-37
diffuse-interface model.

Two independent claims were bundled in the original writeup here and
must be told apart: (1) per-direction Fresnel evaluation for whichever
lobe IS sampled (the true throughput, `specColor`/`wsF0` in the code)
reads the exact painter F0/tint independently in every mode and was
genuinely never touched by this fix, in any mode; (2) the *selection
weight* `ws` — unlike claim (1) — was NOT left unchanged in conductor/
thin-film mode by the original fix: it moved from the raw tint to
`Mean()`/`MeanNM()` in ALL THREE modes, silently changing e.g. a
metal's `pSpecSelect` from 1.0 to its `Mean()`-derived value (~0.9) even
though conductor/thin-film never had DL-64's zero-collapse defect. A
transport review caught this as an untested behaviour change plus an
unconditional performance cost (P2-1 below); the weight computation is
now gated on `fresnelMode == eFresnelSchlickF0`, restoring the
pre-DL-64 raw-tint weight for conductor and thin-film.

`GGXSPF::ScatterNM` reuses the guarded per-wavelength F0 sample for two
purposes pre-fix (the selection weight AND the per-direction Fresnel
input); these had to be split into two variables (`ws` = the new
hemispherical weight, `wsF0` = the original raw F0) so the per-direction
Fresnel math (Schlick, thin-film, conductor branches) stays byte-for-byte
unchanged. `GGXSPF::Scatter` (RGB) and `Pdf`/`PdfNM` had no such reuse and
needed only the weight's source swapped.

**(P3-4, 2026-09-13)** The multiscatter lobe's selection weight
`wms = ws*(1-Ess_i)` uses the SAME hemispherical `ws` as the specular
lobe (only proportioned down by `1-Ess_i`), which is a pre-existing,
unbiased-but-imprecise approximation independent of this fix: `ws`
(whichever formula it comes from) is a single scalar hemispherical
Fresnel value, while the multiscatter lobe's TRUE albedo is closer to
`Fms` (the Kulla-Conty compensation term actually multiplying the MS
BRDF value elsewhere in the same function) than to the raw single-
scatter Fresnel average. Using `ws` for `wms`'s *selection* probability
does not bias the estimator — `kray`/`pdf` still divide out whatever
probability was actually used, per the standard importance-sampling
identity — it only means the MS lobe can be over- or under-sampled
relative to its true contribution, trading some variance. At Schlick
F0=0 this was already true before DL-64 in the sense that the formula
is unchanged; what IS new is that the MS lobe is now reachable at all
in that configuration (before, `ws=0` forced `wms=0` too, so the MS
lobe was never sampled there regardless of how good or bad `wms`'s
approximation was).

### Why a naive ">cosine" bound is not a valid red proof

The straightforward-looking check "`GGXSPF::Pdf`/`PdfNM` at the exact
specular peak direction must exceed the bare cosine density" is
INSUFFICIENT and would pass even pre-fix in the NM path: `UniformColorPainter`'s
spectral path eagerly JH-uplifts even an authored `(0,0,0)`, and that
uplift is not exactly zero (measured `~2.4998e-5` at 450/550/650nm in this
tree — a separate "black guard" gap, independent of DL-62/DL-64 and out
of scope here, analogous to the documented white-guard epsilon). At
literal `diffuse=0.0` that epsilon contaminates BOTH `wd` and (pre-fix)
`ws`, giving a coincidentally nonzero, non-negligible `pSpecSelect` for
the wrong reason and defeating a naive threshold. The committed red proof
instead recomputes the exact expected `Pdf`/`PdfNM` value from the shared
public `MicrofacetUtils`/`MicrofacetEnergyLUT` primitives GGXSPF is built
from — substituting `GGXInterfaceFresnel::Mean()`/`MeanNM()` for the
weight — and asserts an EXACT match (not an inequality), which the
epsilon alone cannot satisfy (the fix's dominant term, `1/21`, is ~2000x
the epsilon). Test configurations additionally use diffuse values 0.01 /
0.3 (never literal 0.0) specifically to keep `wd` unambiguously dominated
by a real diffuse albedo rather than by that same epsilon.

## Red proof

`tests/GGXSampleEvaluationConsistencyTest.cpp`, committed `a1db468d`
against the unfixed library:

```
GGXSampleEvaluationConsistencyTest: 29 checks, 23 failures
```

Representative failures: `aniso mixed W=0.3 maxRelErr=1.118e+01`,
`asymmetric saturation W=0.7 (Y saturates) maxRelErr=1.910e+01` (DL-62,
RGB and NM); `F0=0 diffuse=0 theta=80 specular draws=0/20000` x4 (DL-64
sampling collapse, RGB, grazing and normal incidence, zero and nonzero
diffuse); `F0=0 diffuse=0 Pdf-at-peak theta=60 pdf=0.159155
expected=24.263931 relErr=9.934e-01` (DL-64 Pdf-at-peak mismatch, RGB and
NM).

After `dfdd5ee1`:

```
GGXSampleEvaluationConsistencyTest: 29 checks, 0 failures
```

Every check matches its independent reference to `relErr=0.000e+00`
(exact, not merely within a statistical bound) — including the
zero-filter control, the "SPF Pdf already glossy-filter-consistent"
control (proving the divergence was purely `GGXBRDF`-side), and the HWSS
companion control (`GGXBRDF::valueNM`'s Schlick term was never affected,
since `GGXSPF` does not override `EvaluateKrayNM`).

**(P3-1, 2026-09-13) This `relErr=0` claim overstates the DL-64 half of
the red proof specifically**: the DL-64 reference
(`ExpectedGGXPdfHemisphericalWeight` in the test) computes its expected
`ws` by calling the SAME `GGXInterfaceFresnel::Mean()`/`MeanNM()` helper
production code calls — the test's own comment on that function already
says this plainly ("recomputes the exact expected Pdf/PdfNM value from
the shared public primitives ... substituting `GGXInterfaceFresnel::
Mean()`/`MeanNM()` for the weight"). An exact match therefore proves
`GGXSPF::Pdf`/`PdfNM`/`Scatter`/`ScatterNM` all call `Mean()`/`MeanNM()`
in lockstep with each other and with the mixture-weight arithmetic
around them — a real and non-trivial CONSISTENCY property (and the DL-62
half of the same relErr=0 claim IS an independent check, since that
reference is built from pre-widened alpha painters, not by calling the
function under test) — but it is not independent evidence that `Mean()`/
`MeanNM()` themselves compute the correct hemispherical Fresnel average.
A hypothetical bug that made BOTH production code and this test call,
say, `2*Mean()` instead of `Mean()` would pass this file at `relErr=0`
and would instead be caught by `SPFPdfConsistencyTest`'s independent
Monte-Carlo cross-validation (full-sphere Pdf integral vs. sampled
histogram), not by this file.

## Sibling audit (docs/skills/audit-by-bug-pattern.md)

**DL-62 pattern** — "an SPF widens roughness by `ri.glossyFilterWidth`
for sampling/density; the paired BRDF's `value`/`valueNM` does not":
confirmed present, unfixed, in `CookTorranceSPF`/`CookTorranceBRDF` and
`SchlickSPF`/`SchlickBRDF` — recorded as new debt **DL-65** rather than
fixed here (different roughness/Fresnel parameterization per class, its
own red-proof and gate needed, out of this GGX-scoped slice).

**DL-64 pattern** — "a raw-F0-derived lobe-selection weight collapses to
zero although the evaluated lobe has real grazing energy": checked
against the same two classes and does **not** clearly replicate.
`CookTorranceSPF` has no Schlick-F0 branch at all — it always evaluates a
physically-computed conductor Fresnel (real IOR/extinction) and
multiplies by the `pSpecular` painter as a TINT, so an authored tint of 0
legitimately means "no specular energy," not "lost grazing energy" the
way GGX's Schlick-F0 approximation loses it. `SchlickSPF::Scatter`/
`ScatterNM` sample their Schlick half-vector lobe unconditionally
(`s.kray = rho + (1-rho)*fresnel`, no separate raw-F0-weighted
lobe-selection probability gating it), so there is no analogous
collapse-to-zero mechanism to fix. No new row was opened for this half of
the pattern.

**RGB/NM twins, other Fresnel modes (P2-1-corrected, 2026-09-13)**: this
paragraph originally claimed `eFresnelConductor` and
`eFresnelThinFilmConductor` selection weights "now go through the same
`Mean()`/`MeanNM()` call as `eFresnelSchlickF0` ... uniformly across all
three GGX Fresnel modes" and that this was "verified by the conductor/
thin-film RGB and NM probes in the red-proof test" — **both halves of
that sentence were wrong**: no such probes existed in
`tests/GGXSampleEvaluationConsistencyTest.cpp` (only `eFresnelSchlickF0`
fixtures were ever constructed there), and the uniform-`Mean()` behaviour
itself was reverted by the P2-1 review follow-up (see the "Review
follow-up" note at the top of this file) — calling `Mean()`/`MeanNM()`
for the selection weight in conductor/thin-film mode cost an
unconditional 21-tap LUT average or 21-node×32-wavelength Airy
quadrature on every `Pdf()`/`Scatter()` call for a defect (DL-64) that
does not occur in those two modes. As of `a495a357`, `ws` derives from
`Mean()`/`MeanNM()` ONLY in `eFresnelSchlickF0`; `eFresnelConductor` and
`eFresnelThinFilmConductor` use the raw painter tint, UNCHANGED from
before DL-64 — which is correct, not a regression, because a zero tint
in those two modes already legitimately means zero specular energy (the
same reasoning the DL-64-pattern paragraph above uses to rule out
`CookTorranceSPF`). `TestNonSchlickPdfMatchesExpectedWeight{,NM}` in
`tests/GGXSampleEvaluationConsistencyTest.cpp` (added P2-2) are the
conductor/thin-film RGB and NM probes this sentence should have cited —
they pin the CORRECTED (mode-gated) formula, not the uniform one.
`ThinFilmBRDFTest`/`ThinFilmFurnaceTest`/`ThinFilmProductionTest`/
`GGXConductorAmbientIORTest` staying green is still accurate evidence
that the reverted behaviour did not regress those suites.

## Test-design fallout: ThinFilmBRDFTest Test B

`tests/ThinFilmBRDFTest.cpp`'s Test B ("ScatterNM ≡ valueNM (twin /
HWSS-companion consistency)") drives a thin-film and a bare-conductor
`GGXSPF` with the SAME scripted sampler draws and ratios their sampled
`krayNM`, relying on the documented assumption that "the SAME scripted
sampler ⇒ identical half-vector, G2, G1, pSelect" so the ratio isolates
the pure single-scatter Fresnel term to ~1e-16. That assumption held
pre-fix only because `pSpecSelect` was derived from the SAME raw tint for
both fixtures (the DL-64 bug); post-fix, the thin-film and bare-conductor
hemispherical Fresnel averages legitimately differ at the same tint, so
`pSpecSelect` no longer cancels — the test's `maxRelErr` moved from
`9.821e-16` to `1.133e-04` (real, not a regression; both numbers were
re-derived, not copied). Repaired by adding
`ComputeGGXPSpecSelectNM` (mirrors `GGXSPF::PdfNM`'s formula using the
same public `GGXInterfaceFresnel`/`MicrofacetEnergyLUT` primitives) and
explicitly dividing each fixture's own `pSpecSelect` back out of its
`krayNM` before ratioing, restoring the exact ~1e-16 single-scatter pin
(measured `9.821e-16` after the repair). This is a test-methodology fix
matching the corrected model, not a loosened tolerance: the original
`< 1e-9` bound is unchanged and met.

**Superseded by P2-1 (2026-09-13)**: once the selection weight was
gated back to Schlick-only, `ws` in `eFresnelThinFilmConductor` and
`eFresnelConductor` is once again the SAME raw tint for both fixtures
(both derive it from `GuardedGetColorNM(*pSpecular,...)`, not
`MeanNM()`), so `pSpecSelect` cancels EXACTLY between the thin-film and
bare-conductor twins again — the pre-DL-64 assumption this test
originally relied on is restored. `ComputeGGXPSpecSelectNM` was updated
to mirror the CORRECTED (mode-gated) `PdfNM` formula rather than
removed, so a future regression to the unconditional-`MeanNM()` form
would be caught here too; in the current tree the division it performs
is an identity (`pSelTF == pSelCond`) and Test B's `maxRelErr` is back to
`1.179e-15`.

## P2-1: selection-weight cost (2026-09-13 review follow-up)

Measured with a throwaway timing harness (`tests/GGXSelectionWeightCostBenchTest.cpp`,
built and run against three checkouts of `GGXSPF.cpp`/`GGXBRDF.cpp` in the
`debt-ggx` worktree, then deleted — not part of the tree or the gate):
`GGXSPF::Pdf`/`GGXSPF::Scatter` called in a tight loop (3,000,000 / 1,000,000
iterations respectively) on a fixed conductor fixture (ior=2.74, ext=3.79,
tint=0.85, alpha=0.3) and a fixed thin-film fixture (same substrate, film
n=2.5/k=0/thickness=180nm), 3 runs each, mean ± population stddev in ns/call:

| Variant                                         | conductor Pdf     | conductor Scatter | thin-film Pdf         | thin-film Scatter    |
|--------------------------------------------------|-------------------|--------------------|------------------------|------------------------|
| `12c76e6a` (DL-62/64 landed, unconditional `Mean()`) | 207.20 ± 0.51 ns | 353.63 ± 5.52 ns  | 59923.90 ± 429.02 ns  | 67696.82 ± 82.11 ns  |
| `a495a357` (P2-1 fix, this follow-up)            | 42.87 ± 1.77 ns   | 188.81 ± 2.92 ns  | 41.98 ± 0.13 ns       | 8357.81 ± 14.19 ns   |
| `75f78ba5` (master, pre-DL-62/64 baseline)       | 42.70 ± 1.85 ns   | 186.06 ± 0.43 ns  | 41.84 ± 0.17 ns       | 8499.19 ± 166.03 ns  |

The P2-1 fix and the pre-slice master baseline agree within noise on all
four columns, confirming the fix fully reverts the unconditional-`Mean()`
regression rather than partially mitigating it. The unconditional-`Mean()`
version cost **~4.8x** on conductor `Pdf` (207 vs 43 ns), **~1.9x** on
conductor `Scatter` (354 vs 189 ns — smaller because `Scatter`'s specular
branch already does other per-call work), **~1430x** on thin-film `Pdf`
(59924 vs 42 ns — `Pdf` has no other thin-film-specific cost to amortize
against, so the added 21-node×32-wavelength Airy quadrature dominates
completely), and **~8.1x** on thin-film `Scatter` (67697 vs 8358 ns — large
in absolute terms but proportionally smaller since thin-film `Scatter`
already pays its own per-direction Airy evaluation regardless). Since
conductor is the parser DEFAULT Fresnel mode and `Pdf` is called on every
NEE MIS query at every bounce, the ~4.8x conductor-`Pdf` regression alone
would have been the dominant real-world cost of the unconditional version
on the vast majority of GGX materials in the wild.

## Gates

`GGXSampleEvaluationConsistencyTest` (48/0 post P2-2/P3-2/P3-3 additions;
was 29/0 at the original `dfdd5ee1` close), `GGXWhiteFurnaceTest`,
`GGXFresnelModeTest`, `GGXConductorAmbientIORTest`, `GGXMetalRoughGridTest`,
`GGXFilmTransmissionRangeTest`, `ThinFilmFurnaceTest`,
`ThinFilmProductionTest`, `ThinFilmBRDFTest` (25/0 — the P2-1 follow-up
restored the ORIGINAL simple-ratio cancellation Test B's methodology
relied on before DL-64, per the "Test-design fallout" section above),
`CoatedMaterialChunkTest`, `FabricMaterialChunkTest`,
`WeaveMaterialChunkTest`, `FabricRenderTest`, `CookTorranceMultiscatterTest`,
`SPFPdfConsistencyTest`, `SPFBSDFConsistencyTest` — all green.
`LayeredWhiteFurnaceTest`: `0 of 57 configurations failed` (unchanged).
`GGXDiffuseTransmissionTest`: `150 checks, 3 failures`, bit-for-bit
identical to the documented DL-63 values (`1.0772`/`1.0432`/`1.1467`) both
at the original `dfdd5ee1` close and after the `a495a357` P2-1/P2-2/P3-x
follow-up. Clean `make -C build/make/rise -j8 all` (full `make clean`
rebuild, both before and after the follow-up): zero compiler warnings.
**These three failures were CLOSED 2026-09-14 by DL-63 below —
`GGXDiffuseTransmissionTest` is now `150 checks, 0 failures`; the gate
list above stayed green throughout DL-63's fix, plus two files needed
their own reference-formula updates (see the DL-63 section's "Sibling-
audit collateral" note).**

## DL-63: height-correlated-G2 multiscatter compensation

**Status: CLOSED 2026-09-14** — source repair `052ec469` (debt-ggx2
slice, base `ddf05c6c`), red-prove baseline `150 checks, 3 failures`
(unchanged since DL-37, confirmed bit-for-bit identical before this fix).

**Root cause** (exactly as this doc's original "Independent residual
DL-63" note and the ledger row's recipe diagnosed): GGXBRDF/GGXSPF's
single-scatter specular term is `D * G2 / (4 cosWi cosWo)`, where `G2` is
Smith HEIGHT-CORRELATED masking-shadowing
(`MicrofacetUtils::GGX_G2`/`GGX_G2_Aniso`, Heitz 2014 JCGT 3(2) Sec. 5.2)
— see `GGXBRDF.cpp`'s own file-header comment, which already documents
this as deliberately MORE accurate than CookTorrance's separable
`G1(wi)*G1(wo)`. But `tools/GenerateMicrofacetEnergyLUT.cpp`'s VNDF-
sampling estimator for the Kulla-Conty compensation LUT computed the
per-sample directional-albedo weight as plain `G1(wo)` — the textbook-
correct estimator for the SEPARABLE model (the `G1(wi)` factor cancels
against the VNDF pdf's own `G1(wi)` term), but the WRONG estimator for
height-correlated `G2` (Heitz 2018, "Sampling the GGX Distribution of
Visible Normals": the correct per-sample weight under `G2` is
`G2(wi,wo)/G1(wi)`, which does not factor into `G1(wi)*G1(wo)` so no
cancellation applies). Compensating a `G2` render with a table calibrated
to the separable model under-states how much energy the single-scatter
term already carries, so the added multiscatter term over-shoots — a
specular-only furnace GAIN, visible only in the F0=1 (diffuse=0) rows
because a mixed lobe's diffuse term dilutes the effect below the test's
noise floor.

**CookTorrance is NOT affected** — `CookTorranceBRDF::ComputeFactor` calls
`MicrofacetUtils::GGX_G` (`= GGX_G1(wi)*GGX_G1(wo)`, that function's own
doc comment says so explicitly), i.e. the SEPARABLE model the original
LUT was always correctly calibrated for. This is why the fix ADDS a
second table rather than correcting the existing one: `CoatedBRDF`'s coat
lobe was found, during the required sibling audit, to share GGXBRDF's
pattern (its own single-scatter term also calls `GGX_G2_Aniso`) and was
fixed in the same commit; `CoatedSPF` does not sample a separate
multiscatter lobe (`CoatedLayer.h`'s own header comment: the coat/
substrate interreflection compensation is closed-form, not LUT-based) so
needed no change; `SheenDirectionalAlbedo.h` has its own independent
Charlie+Lambda-fit LUT (unrelated machinery); `ThinFilm.h` only reuses
the shared Gauss-Legendre quadrature nodes/weights (unrelated to
`E_ss`/`E_avg`).

**Fix**: `tools/GenerateMicrofacetEnergyLUT.cpp` gained a
`GGX_G2_HeightCorrelated`/`GGX_Lambda` pair mirroring
`MicrofacetUtils`, and a second accumulator inside the SAME per-
(alpha,cosTheta) sampling loop (same RNG draws, same `NUM_SAMPLES`)
computing the height-correlated weight alongside the unchanged separable
one. Regenerating with the tool reproduces `E_ss_TABLE`/`E_avg_TABLE`
bit-for-bit identical to the checked-in values (independently verified —
confirms the fix is purely additive, not a re-derivation of the existing
table). New `E_ss_TABLE_G2`/`E_avg_TABLE_G2` tables and
`LookupEssG2`/`LookupEavgG2` functions were spliced into
`MicrofacetEnergyLUT.h`, plus hand-written `MSLobeZG2`/
`SampleMSCosThetaG2`/`MSPdfG2` (reusing the already table-agnostic
`Segment`/`SegCDF`/`SegTotal`/`SegInvert` helpers behind a new
`BuildSegmentsG2` wrapper) so GGXSPF's multiscatter-lobe sampler, its
reported density, and its energy terms all stay calibrated to the same
model. `GGXBRDF.cpp`/`GGXSPF.cpp`/`CoatedBRDF.cpp` were repointed to the
G2 twins; `CookTorranceBRDF.cpp`/`CookTorranceSPF.cpp` are byte-for-byte
untouched.

**Independent verification** (`tests/GGXHeightCorrelatedEnergyLUTTest.cpp`,
new — shares no code with either the offline generator or the LUT
header): Monte-Carlo quadrature of the actual production
`MicrofacetUtils::GGX_Lambda`/`GGX_G2` primitives, with an independent
`std::mt19937_64` RNG stream and 4M samples per configuration, at the
three previously-failing (alpha,theta) pairs plus four spot checks:

```
GGXHeightCorrelatedEnergyLUTTest: 14 checks, 0 failures
```

`LookupEssG2` matches the independent quadrature within 8 standard errors
at every configuration (e.g. alpha=1.0 theta=80: `LookupEssG2=0.66805` vs
`quadrature=0.66802+/-0.00017`); `LookupEss` (the pre-existing, unchanged
separable table) measurably diverges from the same quadrature at those
same points (e.g. the same config: `LookupEss=0.52281` vs
`quadrature=0.66839+/-0.00017`, diff `0.14558` — that is **~830 standard
errors** (0.14558 / 0.00017 ≈ 834σ), not "~33 standard errors" as an
earlier draft of this doc stated; 33 is instead the ratio of the diff to
this check's PASS TOLERANCE (`8σ + 0.003 ≈ 0.0044`), i.e.
0.14558 / 0.0044 ≈ 33×, a different number answering a different
question (how far past the gate, not how many σ from the mean) — see
`GGXHeightCorrelatedEnergyLUTTest`'s printed `(834.2 sigma, 33.1x tol=...)`
for both figures side by side), confirming the two tables really are
calibrated to different physical models.

**P2-1 correction (2026-09-14, debt-ggx2 slice)**: the original closed-form
"Kulla-Conty identity" check described in this paragraph (`Ess_G2 +
(1-Ess_G2)*F_ms == 1` at Schlick F0=1, holding to `0.000000e+00` at all
four spot alphas) was **tautological**, not a regression pin — a review
caught that `ComputeFms(F_avg=1, Eavg)` collapses to exactly `1` by pure
algebra for ANY `Eavg` (`denom = 1 - 1*(1-Eavg) = Eavg`, so
`F_ms = 1*1*Eavg/Eavg = 1`), which forces the reported total to `1`
regardless of what `E_ss_TABLE_G2`/`E_avg_TABLE_G2` actually contain — the
`0.000000e+00` diff was the tell, not evidence of a correct table.  It was
replaced by two checks with real content: (a) re-deriving each
`E_avg_TABLE_G2` row from the checked-in `E_ss_TABLE_G2` values via the
SAME midpoint-rule discretization the generator used, blended across
alpha exactly as `LookupEavgG2` blends — matches to `~2-5e-9` (the
tables' own 8-decimal print precision), and would diverge measurably if
`E_avg_TABLE_G2` were stale or mis-baked; and (b) a furnace-style
evaluation at Schlick F0=0.9 (where `ComputeFms` does NOT collapse to a
fixed point) asserting the provable bound `0 <= total <= F0` and that
`F_ms` is measurably below 1 — both genuinely depend on the table
contents.  See `GGXHeightCorrelatedEnergyLUTTest.cpp`'s
`TestEssEavgConsistencyG2`/`TestFurnaceStyleAtF0Point9`.  A separate
review finding (P2-2) added `TestUniformHemisphereIndependentQuadrature`:
a THIRD independent estimator (its own re-implementation of the GGX `D`/
Smith `Lambda`/height-correlated `G2`, its own RNG stream, and uniform-
hemisphere — not VNDF — sampling) that agrees with `LookupEssG2` at
alpha=1.0/mu=0.0156, alpha=0.649/mu=0.1719, and alpha=0.808/mu=0.4844
(e.g. `LookupEssG2=0.93494` vs `uniformHemisphere=0.93486+/-0.00002`),
closing the gap that both `MonteCarloEssG2` above and the offline
generator share the identical VNDF `weight = G2/G1(wi)` derivation and so
could not, between them, catch a shared sign/identity error in it.
`GGXHeightCorrelatedEnergyLUTTest`: `23 checks, 0 failures` (was `14
checks, 0 failures` before this slice; the count grew from the P2-1/P2-2
additions, not from any change to the DL-63 fix itself).

`GGXDiffuseTransmissionTest` (the row's original evidence): `150 checks,
0 failures` (was `150 checks, 3 failures`) — the three configs now read
`{1.0013, 1.0015, 1.0013}` (Schlick iso F0=1, alpha=0.6 theta=80 / alpha=1
theta=60 / alpha=1 theta=80), matching the F0<1 mixed configs' MC-noise-
only deviation from 1.0.

**Sibling-audit collateral**: `GGXSampleEvaluationConsistencyTest`'s
DL-64 Pdf-at-peak and P2-2 conductor/thin-film sections, and
`ThinFilmBRDFTest`'s Test G (specColor-inside-Fms), each independently
RE-DERIVE what `GGXSPF::Pdf`/`PdfNM` and `GGXBRDF::valueNM` should
compute, and both called `LookupEss`/`MSLobeZ`/`MSPdf` directly — the now-
superseded functions for these two consumers. Both files were updated to
call the G2 twins their production counterparts now use
(`GGXSampleEvaluationConsistencyTest`: 24 failures introduced transiently
by this fix, then `48/0` after the update; `ThinFilmBRDFTest`: 2 failures
transiently, then `25/0`). `ThinFilmFurnaceTest`'s
`MultiscatterAlbedoErrorBound` helper (an approximation-QUALITY
measurement against the thin-film-vs-substrate F_avg question, not a
correctness pin) was also switched to `LookupEavgG2` so its reported
error bound reflects the model production actually renders with; its
`4/0` pass count is unchanged since its assertions are about its own
quadrature's internal self-consistency, not the absolute bound value.
This is exactly the "reachable via a different code path" trap
`docs/skills/audit-by-bug-pattern.md` step 5 (audit one hop deeper)
warns about: a test file's OWN reimplementation of a production formula
is a downstream consumer of that formula's identity, not just of its
numeric output.

**User-visible impact (plain statement, both review rounds)**: DL-63
affects the `ggx_material` and `coated_material` scene chunks, in EVERY
Fresnel mode (Schlick/conductor/thin-film), whenever their multiscatter
compensation term is active (`(1-Eavg) > 1e-10`, i.e. any roughness above
the LUT's `alpha=0.01` floor). Head-on incidence is essentially unchanged
(the compensation term itself is small there). The user-visible effect is
a DARKENING at the grazing rim of rough metals/coated surfaces (the
opposite direction from DL-37's earlier grazing-gain fix): recomputed
this pass, directly from the checked-in tables, for the isolated
multiscatter-albedo term `(1-Ess_i)*F_ms` at Schlick F0=0.9 (i.e. how much
of the total reflectance the compensation lobe alone contributes, before
vs. after this fix) --
`alpha=0.3 mu=0.20`: `0.1464 -> 0.1284` (-12.3%);
`alpha=0.3 mu=0.05`: `0.1098 -> 0.0543` (-50.6%);
`alpha=0.6 mu=0.05`: `0.1894 -> 0.0760` (-59.9%)
(near-identical to, and confirming, the review's own independently cited
figures of -12%/-51%/-60% at the same three configurations). DL-65 (the
CookTorrance/Schlick glossy-filter parity fix, closed in the same slice)
is unrelated to DL-63's Fresnel/roughness scope and affects a narrower
surface: **only** renders that have the `filter_glossy` stability knob
enabled (`StabilityConfig::filterGlossy > 0`, a per-bounce roughness
widening applied to `ri.glossyFilterWidth` for variance/firefly control —
`PathTransportUtilities.h` ~:190-192) on `cook_torrance_material` or
`schlick_material` surfaces past their first bounce; scenes that leave
`filter_glossy` at its default 0, or that hit these materials only on the
camera ray, are numerically identical before and after DL-65.

**Known residual: LUT left end-cap (isotropic, small; tracked as DL-86,
NOT fixed)**: `LookupEssG2`/`LookupEss` both flat-clamp `cosTheta` below
the first bin center (`c0 = 0.5/32 ≈ 0.0156`) to that bin's value — a
deliberate, cheap design choice (see `MSLobeDetail::BuildSegmentsFromRow`'s
"left flat end-cap" comment), not a bug in the clamp mechanism itself, but
it does mean `LookupEssG2` under-reads the TRUE (continuing-to-rise)
`Ess` right at the grazing limit. Recomputed this pass with an
independent 8M-sample VNDF quadrature of the real
`MicrofacetUtils::GGX_Lambda`/`GGX_G2` primitives (same methodology as
`GGXHeightCorrelatedEnergyLUTTest`, fresh seed): at `alpha=1.0,
cosWi=0.008` (just inside the first bin), brute-force `Ess=0.9613` vs
`LookupEssG2=0.9349`, diff `0.0263` — since the lookup UNDER-reads the
true single-scatter energy here, the Kulla-Conty compensation adds
slightly too much multiscatter energy back, an isotropic furnace GAIN of
roughly 2.6% confined to incidence angles beyond ~89 degrees (`cosWi`
below the first bin center). This is small, confined to an extreme
grazing sliver, and independent of the anisotropic DL-77 deficit below
(this one persists even for `alphaX==alphaY`). The debt-ggx3 slice (see
"DL-77" section below) promoted this from doc prose to ledger row DL-86,
per the DL-77 recipe's instruction, but deliberately did NOT fix it in
that slice: DL-77's own gate required isotropic behaviour to stay
byte-identical, and any end-cap change touches isotropic numerics at
extreme grazing — see DL-86's row for the recipe.

## DL-77: anisotropic Kulla-Conty compensation (ratio + azimuth)

**Status: CLOSED 2026-09-14** — debt-ggx3 slice, base `a3aa5b8d`.

**Root cause** (confirmed exactly as the ledger row diagnosed): DL-63's
`LookupEssG2`/`LookupEavgG2`/`MSLobeZG2`/`SampleMSCosThetaG2`/`MSPdfG2`
are calibrated to an ISOTROPIC Smith height-correlated model at a single
`alphaEff=sqrt(alphaX*alphaY)` — exact only when `alphaX==alphaY`.
`GGXBRDF`/`GGXSPF`'s single-scatter term uses direction-dependent per-axis
Lambda (`MicrofacetUtils::GGX_G2_Aniso`), so for `alphaX != alphaY` the
isotropized lookup under-compensates. Independently re-measured this pass
against the ledger row's own evidence: `alphaX=.02/alphaY=1.0 mu=0.5`
`0.533` true vs `0.972` isotropized lookup (an ~45% relative gap).

**Fix design decision**: the ledger recipe's option (a) — a fuller table
resolved by anisotropy ratio and (optionally) the incident azimuth — was
implemented in two stages, because the first stage alone proved
insufficient:

1. **Ratio-only (insufficient alone).** Added
   `E_ss_TABLE_G2_ANISO`/`E_avg_TABLE_G2_ANISO`, resolved by
   `ratio = max(alphaX,alphaY)/min(alphaX,alphaY)` (log-spaced `[1,100]`,
   8 steps) in addition to `alphaEff` (16 steps) and `cosTheta` (16
   steps), AZIMUTHALLY AVERAGING wi's azimuth relative to the tangent
   axes at bake time (matching the existing H6 multiscatter-lobe
   sampler's own azimuth-uniform treatment). This closed the AVERAGE-case
   deficit (the ledger's worst config moved from `0.929` to `0.998` on
   the furnace-mean metric) but **regressed** an existing anisotropic
   `GGXDiffuseTransmissionTest::TestSchlickSweep` row into an energy
   GAIN: `alphaX=.05/alphaY=.5 theta=80 az=90` read `1.0376+/-0.0036`
   against the standard limit `1.0266`. Root cause of the regression: an
   independent per-azimuth VNDF quadrature showed the TRUE single-azimuth
   `E_ss` at that config spans `0.789` (wi azimuth=0, aligned with the
   smooth `alphaX=.05` axis) to `0.879` (azimuth=90, aligned with the
   rough `alphaY=.5` axis) against a `0.841` azimuthal average — once the
   average-case deficit closed, the az=90 direction (whose true `Ess` is
   ABOVE the average, needing LESS compensation) over-shot.

2. **Per-azimuth refinement (closes the regression).** Added a second
   table, `E_ss_TABLE_G2_ANISO_PHI` (ratio x alphaEff x phi x cosTheta),
   resolving wi/wo's actual azimuth `phi` relative to the tangent axes on
   an endpoint-inclusive grid over `[0,90]` degrees (7 steps: 0, 15, ...,
   90) — endpoint-inclusive specifically so `phi=0`/`phi=90` (the
   azimuths `TestSchlickSweep`'s rows actually probe) are EXACT table
   entries, not interpolated. This exploits the ellipse's quarter-period
   mirror symmetry (`Lambda_Aniso`'s `alphaX^2*vx^2 + alphaY^2*vy^2` term
   is invariant under `phi -> -phi` and `phi -> 180-phi`), so one quarter
   period with reflective boundaries covers the full circle.
   `LookupEssG2AnisoDirectional(cosTheta, localX, localY, alphaX, alphaY)`
   reads this table and is used at every ENERGY-COMPENSATION call site
   (`Ess_i`/`Ess_o` in `GGXBRDF::value`/`valueNM`, `GGXSPF::Scatter`/
   `ScatterNM`'s `wms` selection weight and MS-lobe `kray`, `GGXSPF::Pdf`/
   `PdfNM`'s `wms`), passing the queried direction's actual tangent-space
   x,y (`wi_local.x/y` or `wo_local.x/y` — already computed at every call
   site for the single-scatter `D`/`G2` terms, or, in `GGXSPF`'s
   MS-lobe branch, algebraically identical to `sinTheta*cos/sin(phiMs)`
   before the `myonb.Transform` call, avoiding a redundant projection).
   The pre-existing azimuth-AVERAGED `LookupEssG2Aniso`/`MSLobeZG2Aniso`/
   `SampleMSCosThetaG2Aniso`/`MSPdfG2Aniso` remain in place, but ONLY for
   the H6 multiscatter-lobe outgoing-direction SAMPLER (an
   importance-sampling proposal shape — `MSPdfG2Aniso` always reports the
   density of what `SampleMSCosThetaG2Aniso` actually samples, so the
   estimator stays unbiased regardless of how good the proposal is;
   precision there is an efficiency concern, not correctness).

**Isotropic byte-identity**: every new lookup function (`LookupEssG2Aniso`,
`LookupEssG2AnisoDirectional`, `LookupEavgG2Aniso`, `MSLobeZG2Aniso`,
`SampleMSCosThetaG2Aniso`) FORWARDS to the exact pre-existing isotropic
function when `alphaX==alphaY` (`fabs(alphaX-alphaY) < 1e-9`), verified
`1e-15`-tight for all five via a standalone probe. `GGXHeightCorrelatedEnergyLUTTest`
(23/0, unchanged) and `GGXDiffuseTransmissionTest`'s isotropic rows are
therefore unaffected.

**Note (debt-ggx3, review round 2): the Provenance and Red-proof
paragraphs immediately below describe the table's FIRST-stage
implementation** — the `(ratio,alphaEff)` parametrization at
`ANISO_ALPHA_SIZE=16`/`ANISO_COS_SIZE=16`, ~54s bake — which the SAME-DAY
P1 follow-up (further down this section) superseded with a direct
`(alphaX,alphaY)` grid, and which review round 2 (also further down, see
"P2 root-cause correction") then raised to `ANISO_PHI_SIZE=13`. The
CURRENT shipped state is `ANISO_ALPHA_SIZE=24` x `ANISO_COS_SIZE=32` x
`ANISO_PHI_SIZE=13`, direct `(alphaX,alphaY)` axes, ~292s (4m52s) bake —
see `docs/DEBT_LEDGER.md`'s DL-77 row and the "P2 root-cause correction"
subsection below for the authoritative final numbers; the two paragraphs
below are kept for their still-accurate architectural description
(two-stage table design, VNDF-sampling bake, hand-derived lookup
machinery) but their SIZES and TIMING are historical.

**Provenance**: `tools/GenerateMicrofacetEnergyLUT.cpp` gained anisotropic
Lambda/G1/G2/VNDF-sampling helpers (double precision, mirroring
`MicrofacetUtils`) and the fixed-phi-grid MC bake
(`NUM_SAMPLES_ANISO=150000`/cell, `ANISO_RATIO_SIZE=8` x
`ANISO_ALPHA_SIZE=16` x `ANISO_PHI_SIZE=7` x `ANISO_COS_SIZE=16` =
14,336 cells, ~2.15e9 samples, ~54s). `E_ss_TABLE_G2_ANISO`/
`E_avg_TABLE_G2_ANISO` are DERIVED from the phi-resolved bake by
trapezoidal-averaging over phi (not separately generated), so the two
tables cannot drift out of sync. The `LookupEssG2AnisoDirectional`/
`AnisoPhiIndex`/`AnisoAlphaIndex`/`AnisoRatioIndex`/`BuildSegmentsFromRowN`
machinery is hand-derived (not baked) and embedded verbatim in the
generator (`kHandMaintainedDL77AnisoBlock`), following the same
provenance discipline as the pre-existing `kHandMaintainedH6Block`.
Regenerating reproduces `MicrofacetEnergyLUT.h` byte-for-byte.

**Red-proof**: `GGXDiffuseTransmissionTest`'s `TestAnisotropicKnownFailureDL77`
known-failure control was promoted to a real, TWO-SIDED gating check
(`TestAnisotropicFurnaceDL77`/`CheckAnisotropicFurnaceBound`) — the
standard one-sided upper energy bound (`mean <= 1+6SE+.005`) PLUS a
`kAnisoFloor=0.90` lower floor, specifically so a regression of this fix
(which used to read as low as ~0.57-0.93 on these configs) fails loudly
here instead of silently passing a gain-only check again. Two more
independent `(alphaX,alphaY,theta,azimuth)` rows were added: ratio=10 at
theta=75/az=45 (deliberately BETWEEN phi grid points, exercising the
quadrilinear phi interpolation) and ratio=9 at theta=70/az=90. All three
pass comfortably (`0.9982+/-0.0033`, `1.0061+/-0.0038`, `1.0049+/-0.0029`
at the time this paragraph was written — **stale, see the P2 correction
below**: re-taken on the tree immediately before review round 2's fix,
these three read `0.9995`/`1.0020`/`1.0013`; after the `ANISO_PHI_SIZE`
7->13 fix, `0.9990+/-0.0033`/`1.0027+/-0.0038`/`1.0009+/-0.0029`).
`GGXDiffuseTransmissionTest: 153 checks, 0 failures` (was `151/0`, with
the deficit invisible to the one-sided known-failure control; `155/0`
after the P1 follow-up added 2 more rows, unchanged by review round 2's
fix).

**Gate** (12 tests, all pass, clean warning-free rebuild):
`GGXDiffuseTransmissionTest` 153/0, `GGXHeightCorrelatedEnergyLUTTest`
23/0, `GGXSampleEvaluationConsistencyTest` 48/0, `GGXWhiteFurnaceTest`,
`LayeredWhiteFurnaceTest` (0 of 57 configs fail), `ThinFilmBRDFTest` 25/0,
`ThinFilmFurnaceTest` 4/0, `SPFPdfConsistencyTest`, `SPFBSDFConsistencyTest`,
`CookTorranceMultiscatterTest` 17/0, `GGXMetalRoughGridTest`,
`GGXFresnelModeTest`.

**Sibling audit** (docs/skills/audit-by-bug-pattern.md): `CoatedBRDF`'s
coat lobe CONFIRMED exempt — its `CoatLobeValue` always calls
`GGX_G2_Aniso(alpha, alpha, ...)` with a single scalar `alpha` (isotropic
by construction), so it has no anisotropic case to mismatch against;
unchanged. `grep`'d the whole tree for `GGX_G2_Aniso`/`LookupEssG2`/
`LookupEavgG2` consumers: only `GGXBRDF.cpp`, `GGXSPF.cpp`,
`CoatedBRDF.cpp` (exempt), and `MicrofacetEnergyLUT.h` itself. Three test
files re-derive the isotropic formula directly for their own independent
consistency checks (`ThinFilmBRDFTest.cpp`, `ThinFilmFurnaceTest.cpp`,
`GGXSampleEvaluationConsistencyTest.cpp`'s DL-64 sections) — all three
exercise ONLY isotropic `alpha` configurations (no `alphaX`/`alphaY`
pair), so none needed updating.

**User-visible impact**: affects `ggx_material` in every Fresnel mode
wherever `alphaX != alphaY` (anisotropic roughness) and the multiscatter
term is active. Before this fix, anisotropic rough metals/coated surfaces
were measurably UNDER-bright at grazing incidence (11-76% `E_ss`-level
deficit depending on ratio/azimuth, per the ledger row's and this
section's measurements); after, they render close to energy-conserving
(1-2% residual, per the three furnace rows above). Rendered
`scenes/Tests/Materials/ggx_anisotropy_sweep.RISEscene` (brushed-metal
silver spheres, 3x3 alphaX/alphaY grid, `pathtracing_pel_rasterizer`
64spp, `oidn_denoise FALSE`, `pixel_filter box`, EXR output in
`Rec709RGB_Linear`, same RNG seed before/after via
`git checkout -- <production files>` / re-apply) before and after: mean
luminance per 3x3 grid region rose `+0.9%` to `+4.8%` (brightening,
consistent with closing an energy DEFICIT — the opposite direction from
DL-63's grazing-rim darkening fix); no NaN/Inf introduced in either
render.

**Residual, NOT closed by this fix**: the per-azimuth table itself still
resolves phi on a coarse 7-point grid and interpolates quadrilinearly —
a config that lands between ratio/alphaEff/phi/cosTheta grid points
carries residual interpolation error (observed up to ~1% on the furnace
metric at the tested configs; not exhaustively swept across the full
`(alphaX,alphaY,theta,azimuth)` space). The pre-existing isotropic LUT
left end-cap residual (see above, now tracked as DL-86) is independent
and unaffected by this fix.

**P2 root-cause correction (debt-ggx3, review round 2)**: the P1
follow-up's own residual pass below ("P2-2") attributed its worst-case
`3.25%` interpolation residual (at `alphaX=0.9353,alphaY=0.0752,
cos=0.1211,phi=85.5`) to "the same grazing end-cap DL-86 already tracks".
That attribution was WRONG for the cited point. A profile at that exact
configuration — nowhere near DL-86's `cos<0.0156` flat-clamp region —
found table-vs-truth is `0.1-0.7%` at every phi grid NODE (the then-grid's
`60`/`75`/`90` degree nodes) but peaks `3.2-3.3%` MID-INTERVAL
(`85-87.5` degrees, between the `75` and `90` degree nodes): snapping phi
alone to the nearest node dropped the residual `3.43%->0.79%`, while
snapping cos alone (keeping phi at the off-grid `85.5`) left `3.24%`
unchanged. The actual dominant driver is the 15-degree-coarse
`ANISO_PHI_SIZE=7` azimuth grid against strong curvature near `phi=90`
at high anisotropy ratios; a SECONDARY driver is the LINEAR alpha axis
(the first two nodes, `alpha=0.01` and `alpha=0.053`, are a `5.3x` ratio
apart in one cell, so bilinear interpolation near the low-alpha diagonal
mixes strongly anisotropic corner cells — also the source of a
pre-existing `1.27e-3` `E_ss` seam and `3.6%` `MSLobeZ` seam noted
elsewhere in the codebase). DL-86's cosTheta end-cap IS a real, separate
driver — it dominates OTHER sweep points (e.g. `2.79%` at `cos=0.0062`)
— it simply was not what the cited point was measuring.

Fix: `ANISO_PHI_SIZE` raised `7->13` (7.5-degree steps instead of
15-degree; the endpoint-inclusive `[0,90]` grid and the exact node-mirror
symmetry `phiDeg[N-1-pi] = 90-phiDeg[pi]` both hold for any `N`, verified
directly). The alpha axis was measured and deliberately left LINEAR: a
log-spaced axis would require re-deriving the P2-1 isotropic-diagonal
seeding, the pass-2 mirror-symmetry fill, and every runtime call site's
index math, to address a driver this pass measured as SECONDARY to the
phi grid — out of scope here, recorded rather than silently dropped.

Re-measured (independent scratch program — own `mt19937_64` RNG stream,
its own transcription of the anisotropic Lambda/G1/G2/VNDF-sampling
formulas, not sharing code with the generator or the lookup under test;
not checked into the tree): the SAME cited point, re-evaluated at 4e6
samples for a clean (low-MC-noise) number, now reads `2.58%` (was
`3.25%`). A fresh 4000-point sweep (own RNG stream, 200k VNDF
samples/point, matching the original P2-2 methodology) restricted to
`cos>=0.03` — isolating this residual from DL-86's separately-tracked
end-cap — gives a worst case of `2.69%` at
`alphaX=0.0411,alphaY=0.6853,cos=0.0742,phi=2.9` (mean `0.118%` over
3908 points, 6 `>1%`, 2 `>2%`, 0 `>5%`), down from the pre-fix
`3.25%`/`0.15%`/35/7/0. The UNRESTRICTED sweep's worst case is `17.89%`
at `alphaX=0.0361,alphaY=0.9627,cos=0.0024,phi=5.0` — a genuine DL-86
end-cap point (`cos=0.0024` sits deep inside the first `ANISO_COS_SIZE`
bin), not a regression introduced by this fix.

Still NOT fully closed to the `<=1%` target — the residual's root cause
is now correctly attributed to (a) phi-interpolation curvature near
extreme-anisotropy azimuths, narrowed but not eliminated by the 13-point
grid, and (b) the low-alpha linear-grid coarseness noted above; both are
left open (a denser/non-uniform phi grid or a log-spaced alpha axis would
address them, and are out of scope for this pass). The separate cosTheta
end-cap (`cos<0.0156`) remains tracked as DL-86 and is unaffected by
this fix, isotropic or anisotropic. `GGXDiffuseTransmissionTest`'s two-sided
furnace rows (seeds 9001-9005) all still pass comfortably post-fix:
`0.9990+/-0.0033`, `1.0027+/-0.0038`, `1.0009+/-0.0029`,
`0.9991+/-0.0029`, `0.9950+/-0.0032`. Bake wall time at the final
`24x24x13x32` grid: `~292s` (`~4m52s`), single-threaded (the generator
has no internal parallelism).

**P3 (debt-ggx3 review, limitation of the render evidence above)**: the
`ggx_anisotropy_sweep.RISEscene` per-region MEAN luminance comparison
above is a poor detector for an azimuth-mirroring bug specifically (the
class of bug the P1 follow-up below fixed). A sphere under a fixed
tangent-frame convention presents every azimuth around its silhouette;
mirroring phi (swapping which axis reads "smaller-alpha") REDISTRIBUTES
energy between symmetric points on the sphere rather than changing the
region's aggregate mean by much, particularly under indirect/ambient
lighting where many incident azimuths get integrated together at each
pixel. Concretely: this render comparison shipped in the CLOSED section
above with the P1 axis-swap bug still present in the code (P1 was found
and fixed in a same-day follow-up, see below) and did not catch it — the
+0.9%/+4.8% brightening it reports is real (it reflects the ratio-and-
average-case E_ss fix) but is NOT evidence the azimuth convention was
correct. The actual regression coverage for azimuth-mirroring is the
unit-level `GGXHeightCorrelatedEnergyLUTTest::TestRelabelSymmetry` rows
added by the P1 follow-up (exact `LookupEssG2AnisoDirectional(aX,aY,phi)
== (aY,aX,90-phi)` identity, tight to `~1e-16`) plus
`GGXDiffuseTransmissionTest::TestAnisotropicFurnaceDL77`'s two
`alphaX>alphaY` furnace rows — not a re-render. A genuinely azimuth-
resolved render check (e.g. per-longitude-band luminance on a single
sphere with a strong directional key light, so azimuth-mirrored energy
would show up as a left/right asymmetry) was not built this slice; this
is recorded as the honest scope limitation rather than an unbuilt
diagnostic.

**Review round 2 (debt-ggx3, same day): `inline constexpr` -> `inline
const`, and a table-size correction**: the 7 large `Scalar` tables in
`MicrofacetEnergyLUT.h` (previously converted from `static const` to
`inline constexpr` earlier this slice) were changed again to `inline
const` — identical C++17 external-linkage/one-definition-rule dedupe,
but without obligating compile-time constant evaluation, which avoids
MSVC's default `/constexpr:steps 100000` budget failing on the largest
table (`E_ss_TABLE_G2_ANISO_PHI`, `24*24*13*32 = 239,616` elements at the
final `ANISO_PHI_SIZE=13` grid — an earlier note in this codebase cited
`129,024`, the count at the pre-bump `ANISO_PHI_SIZE=7`; corrected
debt-ggx3 review round 3). `GGXDiffuseTransmissionTest`: `155 checks, 0
failures` (was `153/0`); `GGXHeightCorrelatedEnergyLUTTest`: `30 checks,
0 failures` (was `23/0`).

**Review round 2 fix-pass (debt-ggx3, same day): phi-grid regression
guard**: added one tight two-sided regression-guard row to
`GGXDiffuseTransmissionTest.cpp` at the exact cited residual point
(`alphaX=0.9353,alphaY=0.0752,theta=83.0441,az=85.5`), using a 400k-sample
local measurement to get the standard error small enough to discriminate:
this tree (post `ANISO_PHI_SIZE=13`) reads `mean=0.98434, se=0.00087`.
The row's band is `[mean - 4*se, mean + 4*se]` computed from the RUN'S
OWN runtime `se` (not a copied-in literal), so it tracks its own
statistics if the sample count or RNG ever changes. **Round 3
correction**: the row originally shipped with a `6*se` band, which does
NOT red-proof — an isolated rebuild with the pre-fix (`ANISO_PHI_SIZE=7`)
header, at the SAME seed/sample count, measures `0.97933+/-0.00088`,
which lies INSIDE a `6*se` band (`[0.97912,0.98956]`) built from this
run's `se`. Tightened to `4*se` (band `[0.98086,0.98782]` at this run's
`mean`/`se`): the pre-fix `0.97933` now falls `~1.75*se` below the lower
bound, a genuine, verified FAIL — see the isolated pre-fix-header
rebuild's console line in the fix commit message.
Also added one `alphaX>alphaY` material-level row (`ax=0.8,ay=0.1`) to
`GGXWhiteFurnaceTest.cpp`'s Test 6/Test 7 (`TestMaterialPointwiseConsistency`,
conductor and schlick_f0) — every prior row there had `alphaX<=alphaY`,
the exact P1 blind spot; both new rows pass.
