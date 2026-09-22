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

**~~Known residual: LUT left end-cap~~ -- CLOSED 2026-09-14 (debt-dl86
slice); see "DL-86" section below.** (Original prose, struck: isotropic,
small; tracked as DL-86. `LookupEssG2`/`LookupEss` both flat-clamped
`cosTheta` below the first bin center (`c0 = 0.5/32 ≈ 0.0156`) to that
bin's value — a deliberate, cheap design choice (see
`MSLobeDetail::BuildSegmentsFromRow`'s "left flat end-cap" comment), not
a bug in the clamp mechanism itself, but it meant `LookupEssG2`
under-read the TRUE (continuing-to-rise) `Ess` right at the grazing
limit. Measured at the time with an independent 8M-sample VNDF
quadrature: at `alpha=1.0, cosWi=0.008` (just inside the first bin),
brute-force `Ess=0.9613` vs `LookupEssG2=0.9349`, diff `0.0263` — since
the lookup under-read the true single-scatter energy here, the
Kulla-Conty compensation added slightly too much multiscatter energy
back, an isotropic furnace GAIN of roughly 2.6% confined to incidence
angles beyond ~89 degrees.)

## DL-86: grazing-limit left end-cap (isotropic LookupEssG2/LookupEss,
anisotropic LookupEssG2AnisoDirectional)

**Status: CLOSED 2026-09-14** — debt-dl86 slice, base `943d353b`. **The
first closure (`8a66d5b6`, same slice) was DEFECTIVE and is superseded by
the fix described here; its numbers are kept below only as the "straight
line" column of the residual tables.**

**Root cause** (confirmed exactly as the ledger row diagnosed):
`LookupEssG2`/`LookupEss`/`LookupEssG2AnisoDirectional` all flat-clamped
`cosTheta` below the first LUT bin center `c0=0.5/32≈0.0156` (isotropic)
or `c0=0.5/ANISO_COS_SIZE` (anisotropic-directional, same numeric value)
to that bin's own value, mis-reading the single-scatter directional
albedo right at the grazing limit. Measured against an independent
20M-sample VNDF quadrature (own `mt19937_64` stream, RISE's own
`MicrofacetUtils` primitives): `alpha=0.01, cosTheta=0.0001`, truth
`0.99789` against the clamp's `0.89927` — a **9.88%** under-read, which
inflates the Kulla–Conty compensation weight `(1-Ess)` by a factor of
`(1-0.89927)/(1-0.99789) = 48`.

### Why a straight line to the boundary is not enough (review finding P1-1)

The boundary value itself is exactly derivable for the height-correlated
model: the VNDF per-sample weight is `G2(wi,wo)/G1(wi) = (1+Lambda(wi)) /
(1+Lambda(wi)+Lambda(wo))`, and Smith `Lambda(wi) ~ alpha/(2*cosTheta)`
diverges as `cosWi -> 0`, so the ratio `-> 1` for ANY finite
`Lambda(wo)`. `Ess_G2(cosTheta=0) = 1` is therefore a proved boundary
condition, not a fit — and the first closure used it, interpolating a
single straight line from `(0, 1)` to `(c0, E_ss_TABLE_G2[alpha][0])`.

That is still wrong, because **`E_ss_G2(cosTheta)` is NOT monotone on
`[0, c0]` at low roughness.** At `alpha=0.01` the 20M-sample quadrature
reads `0.97959` at `cos=0.001`, DIPS to `0.89196` near `cos=0.010`, and
comes back up to `0.89914` at `c0` — a minimum strictly inside the
interval. A straight line from `(0,1)` cannot follow that, and
over-reads by up to **5.7%** (`alpha=0.01, cos=0.008`: line `0.94843` vs
truth `0.89734`) — an error of the OPPOSITE sign, and at that point
LARGER, than the flat clamp it replaced (`0.89927`, 0.22% off at the
same point, by coincidence).

The separable table is worse still (review finding P1-4): `LookupEss`'s
row 0 RISES with `cosTheta` (`0.89475` at `c0`, `0.97310` at the next
bin), so a bin0→bin1 secant extrapolates DOWNWARD, away from the true
limit. At `alpha=0.01, cos=0.002` truth is `0.91744`; the flat clamp read
`0.89475` (2.47% low) and the secant read `0.86059` (**6.20%** low) —
i.e. the first closure made CookTorrance's compensation weight `(1-Ess)`
error grow from `+27.5%` to `+68.8%`.

### Fix: bake the interval

The end-cap is now a **baked sub-grid**, not any extrapolation model
(ledger recipe option (a)). `SUB_SIZE = 8` uniform sub-intervals span
`[0, c0]`; `SUB_SIZE-1 = 7` interior nodes are stored per row (the
`k=SUB_SIZE` node IS bin 0 itself, so the model is continuous with the
ordinary bilinear interior at `c0` by construction), and the `k=0`
boundary is the exact `cosTheta -> 0` limit:

  * `E_ss_SUB_TABLE_G2[32][7]` — height-correlated G2, boundary exactly 1.
  * `E_ss_SUB_TABLE[32][7]` + `E_ss_LIMIT_TABLE[32]` — separable model.
    Its limit is NOT 1 and has no closed form here (the per-sample VNDF
    weight is plain `G1(wo)`, with no cancellation against the proposal's
    `G1(wi)`), so it is BAKED per alpha row at `cosTheta = 1e-7`
    (converged: an independent quadrature reads `0.93436251` at `1e-6`
    and `0.93436415` at `1e-7` for `alpha=0.05`). Values range from
    `0.936088` at `alpha=0.01` to `0.613669` at `alpha=1.0` (review
    finding P2-3).
  * `E_ss_TABLE_G2_ANISO_PHI_SUB[24][24][13][12]` — the per-azimuth
    anisotropic twin, consumed by `LookupEssG2AnisoDirectional`. The
    boundary argument holds per-direction (`Lambda_Aniso(wi) -> infinity`
    as `cosWi -> 0` at every azimuth), so the `n=0` node is again exactly
    1 and is not stored. Its 12 (not 7) nodes are the round-2
    refinement — see "Round 2" below.
  * `E_ss_TABLE_G2_ANISO_SUB[24][24][12]` — DERIVED from the above by the
    same trapezoidal phi average that already produces
    `E_ss_TABLE_G2_ANISO`, consumed by `LookupEssG2Aniso` and
    `BuildSegmentsFromRowN` (review finding P2-2/P3; see
    "Sampler/pdf self-consistency" below).

`SUB_SIZE=8` was chosen by measurement, not by taste: reconstructing the
alpha-row-blended model against an independent quadrature on
`alpha in {0.01,0.015,0.02,0.05,0.3,1.0} x cosTheta in
{1e-4,...,1.56e-2}`, `SUB_SIZE=4` leaves 0.41% worst-case at
`alpha=0.01`, `SUB_SIZE=8` leaves 0.11%, `SUB_SIZE=16` leaves 0.02%. 8 is
an order of magnitude inside the 1% target; 16 would double the aniso
sub-table for no measurable gain.

**The pre-existing tables are byte-for-byte unchanged.** The sub-grid
samples are drawn from a SECOND, independently-seeded LCG
(`rng_state_sub`, generator-side), so adding them did not shift a single
draw of the original stream — verified by extracting every
`inline const Scalar` table from the pre- and post-change regenerations
and comparing element-wise: `E_ss_TABLE` (1024), `E_ss_TABLE_G2` (1024),
`E_avg_TABLE`/`E_avg_TABLE_G2` (32 each), `E_ss_TABLE_G2_ANISO` (18432),
`E_ss_TABLE_G2_ANISO_PHI` (239616) and `E_avg_TABLE_G2_ANISO` (576) all
IDENTICAL. The round-2 re-bake preserves that, and extends it to the
three ISOTROPIC sub-tables it does not touch: `E_ss_SUB_TABLE` (224),
`E_ss_SUB_TABLE_G2` (224) and `E_ss_LIMIT_TABLE` (32) are byte-identical
too, because the isotropic sub-grid is baked from `rng_state_sub` BEFORE
the aniso one and its draw count did not change.

### Round 2: the anisotropic end-cap's FIRST sub-interval (review P1)

Round 1's uniform sub-grid is enough for the ISOTROPIC tables and not for
the anisotropic one, and the reason is structural rather than a matter of
resolution. The approach to `Ess = 1` needs `Lambda(wi) >> Lambda(wo)`.
For an isotropic surface both grow together and the deficit is already
essentially LINEAR in `cosTheta` by the first sub-node: at `alpha=0.01`
the true curve reads `0.958` at `h = c0/8 = 1.953e-3` and `0.9796` at
`1e-3` (deficits `0.042` and `0.0204` across a factor of `1.953`), so the
straight ramp from the exact anchor is accurate to **0.016%** at
`cos=1e-4` — measured, `GGXHeightCorrelatedEnergyLUTTest`.

For an anisotropic pair viewed along the SMALL axis, `Lambda(wi)` only
takes over at `cos << alphaX*sinTheta`, while the scattered lobe is
spread over the LARGE axis and keeps `Lambda(wo)` large. At
`(alphaX=0.01, alphaY=1.0, phi=0)` the true curve is still at `0.735` at
that same first node — a drop of `0.265` across one straight ramp from the
anchor. The round-1 lookup therefore read **+2.49%** at `cos=1e-4`,
**+4.34%** at `2.5e-4`, **+5.86%** at `5e-4` and **+5.91%** at `1e-3`,
against `-0.08%` at the first baked node and `<=0.75%` above it: the
BAKED NODES were accurate and the one un-bracketed interval was not.

Round 1's own probe set could not see this — its smallest anisotropic
`cosTheta` was `0.002`, just ABOVE `h`, so the whole first sub-interval
went unprobed and every document quoted `<=0.72%`, a figure that
isolated exactly the part of DL-86 that was already right.

**Construction.** The aniso sub-grid's lowest interval is refined by
`ANISO_SUB_FINE = 5` GEOMETRIC octaves — nodes at `h*2^-j` for
`j = 1..5`, i.e. down to `6.1e-5` — so the one segment that is not
bracketed by two baked nodes now lies entirely below the smallest
`cosTheta` any production shading direction reaches (`1e-4` is
`theta = 89.994` degrees). Node indices run `0..ANISO_SUB_TOTAL` with
`ANISO_SUB_TOTAL = SUB_SIZE + ANISO_SUB_FINE = 13`; index 0 is the exact
anchor, `1..5` the octaves, `6..13` the unchanged uniform nodes, and the
last IS bin 0 — so 12 values are stored per row (was 7). `AnisoSubNodeCos`
is the single definition of the abscissae, and the generator carries a
byte-identical twin (`anisoSubNodeCos`) so the bake and the lookup cannot
disagree about where a node sits.

`ANISO_SUB_FINE = 5` was chosen by measurement (1M-sample-per-point
independent quadrature at the node-exact configurations, same 8-value
probe set): `J=3` leaves 0.81% — its anchor ramp still reaches `2.44e-4`,
above the smallest probe — `J=4` leaves 0.60% and `J=5` leaves 0.54%. From
`J=4` on, the worst residual is no longer in the end-cap at all: it sits
at `cos=5e-3`, inside the UNIFORM part of the grid, and does not move
with further refinement. `J=5` is taken because it is the first value
whose anchor ramp lies entirely below the smallest probe.

Interpolation stays **linear in cos** on every interval. Log-cos measured
better on the uniform part (0.13% vs 0.54% at `cos=5e-3`, the curve being
near a power law there), but it cannot express the interval that touches
`cos=0`, and every consumer of these nodes — `MSLobeDetail::SegTotal` /
`SegInvert`, which integrate and invert `(1-Ess)*c` in closed form for the
H6 sampler — is built on the piecewise-LINEAR model. 0.54% is already
inside the 1% target.

**The ISOTROPIC sub-grid is deliberately NOT refined** (its residual at
`alpha=0.01, cos=1e-4` is 0.016%, i.e. there is nothing to gain), and
`E_ss_SUB_TABLE`, `E_ss_SUB_TABLE_G2` and `E_ss_LIMIT_TABLE` are
byte-for-byte identical across the round-2 re-bake, along with all seven
pre-existing tables — re-verified element-wise. The aniso sub-tables' own
`alphaX==alphaY` diagonal is still seeded from the isotropic MODEL (now
evaluated at the new nodes' `cosTheta` rather than by node index), so the
isotropic limit stays continuous.

### Residual table

Worst relative error against an independent 20M-sample-per-point VNDF
quadrature, over `cosTheta in {1e-4, 1e-3, 2e-3, 5e-3, 8e-3, 1e-2,
1.56e-2}` (all strictly below `c0`). "flat" = pre-slice `943d353b`,
"line" = the defective first closure `aff019de`, "baked" = this fix.

| lookup | alpha | flat % | line % | baked % |
|---|---|---|---|---|
| `LookupEssG2` | 0.005 | 9.69 | 8.49 | **5.10** (DL-105) |
| `LookupEssG2` | 0.01 | 9.88 | 5.71 | **0.12** |
| `LookupEssG2` | 0.02 | 8.92 | 2.57 | **1.59** (DL-105) |
| `LookupEssG2` | 0.05 | 6.07 | 0.25 | **0.26** |
| `LookupEssG2` | 0.3 | 2.72 | 0.18 | **0.02** |
| `LookupEssG2` | 1.0 | 6.42 | 0.59 | **0.07** |
| `LookupEss` | 0.005 | 5.51 | 8.40 | **5.52** (DL-105) |
| `LookupEss` | 0.01 | 4.32 | 8.48 | **0.07** |
| `LookupEss` | 0.02 | 4.12 | 6.64 | **1.58** (DL-105) |
| `LookupEss` | 0.05 | 3.22 | 2.01 | **0.17** |
| `LookupEss` | 0.3 | 0.82 | 0.04 | **0.02** |
| `LookupEss` | 1.0 | 1.49 | 0.06 | **0.06** |

Independently re-measured post-fix on the review's own requested grid
(`alpha in {0.005, 0.01, 0.015, 0.02, 0.05, 0.3, 1.0}` x `cosTheta in
{1e-4, 5e-4, 1e-3, 2e-3, 5e-3, 8e-3, 1e-2, 1.56e-2}`, 20M samples per
point, a standalone program with its own RNG stream — worst relative
error per alpha, G2 / separable): `0.005` **5.11 / 5.52**, `0.01`
**0.12 / 0.07**, `0.015` **1.46 / 1.60**, `0.02` **1.59 / 1.57**, `0.05`
**0.25 / 0.17**, `0.3` **0.02 / 0.02**, `1.0` **0.07 / 0.04**. The
review's `<=1%` target is therefore met for every alpha the LUT's alpha
axis actually resolves, and missed only where that axis itself is the
error (`0.005`, below the table's range; `0.015`–`0.02`, mid-way through
the 4.2x-wide first cell) — DL-105.

Anisotropic `LookupEssG2AnisoDirectional`. The first group is EXACT on
all three interpolated grid axes (`alphaX`, `alphaY` at
`0.01 + 0.99k/23`; `phi` at multiples of 7.5 degrees), so `cosTheta` is
the only interpolated axis — that group is what isolates this end-cap.
The second group is the typical near-node configurations, and the third
is the single corner debt-ggx3 cited, which is off-node on ALL THREE
axes.

**Probe protocol** (this is the part round 1 got wrong, so it is stated
explicitly): worst relative error over
`cosTheta in {1e-4, 2.5e-4, 5e-4, 1e-3, 2e-3, 5e-3, 1e-2, 1.56e-2}`,
against `MonteCarloEssG2AnisoDirectional` at 20M samples per point —
`GGXHeightCorrelatedEnergyLUTTest`'s own printed rows, one seed per row.
The `flat`/`line` columns were measured over round 1's set, which began
at `0.002`; `r1` and `r2` are both measured over the set above, so they
compare like with like. `r1` = the uniform sub-grid (`03656e6c`),
`r2` = the geometrically refined aniso sub-grid.

| (alphaX, alphaY, phi) | flat % | line % | r1 % | r2 % |
|---|---|---|---|---|
| node-exact (0.01, 1.0, 0) | 28.90 | 38.69 | 5.91 | **0.34** |
| node-exact (0.01, 1.0, 45) | 3.81 | 0.40 | 0.04 | **0.05** |
| node-exact (0.01, 1.0, 90) | 2.91 | 0.29 | 0.03 | **0.04** |
| node-exact (1.0, 0.01, 0) | 2.91 | 0.29 | 0.03 | **0.04** |
| node-exact (1.0, 0.01, 90) | 28.90 | 38.72 | 5.93 | **0.32** |
| node-exact (0.87087, 0.09609, 90) | — | — | 0.48 | **0.22** |
| node-exact (0.22522, 0.48348, 7.5) | 4.26 | 0.53 | 0.09 | **0.09** |
| node-exact (0.95696, 0.09609, 82.5) | 12.23 | 2.26 | 0.27 | **0.13** |
| (0.9, 0.1, 0 / 45 / 90) | 2.67 / 3.46 / 16.42 | 0.25 / 0.36 / 4.01 | 0.03 / 0.04 / 0.47 | **0.02 / 0.01 / 0.16** |
| (0.05, 0.5, 0 / 45 / 90) | 12.20 / 2.21 / 1.69 | 2.63 / 0.22 / 0.15 | 0.63 / 0.04 / 0.02 | **0.57 / 0.04 / 0.02** |
| (0.0361, 0.9627, 5), off-node on all 3 axes | 17.99 | 2.42 | 3.97 | **4.04** (DL-105) |

Three things to read out of that. First, the straight line was
catastrophic exactly where the curve is least linear: at the 100:1
anisotropy node it is **38.7%** off, WORSE than the flat clamp's 28.9%.
Second, the round-1 baked grid left **5.9%** at those same two nodes — the
whole of it inside the single un-bracketed first sub-interval, which
round 1's probe set skipped (see "Round 2" above); the geometric
refinement closes them to **0.34%**, and the worst node-exact residual
anywhere in the table is that same 0.34%, at `cos=1.56e-2` where it is
the ordinary bin-0 blend and the sub-table's own Monte-Carlo noise, not
the end-cap. Third, the `(0.0361, 0.9627, 5)` corner — the one the DL-77
record quotes — is not measuring this end-cap at all: it sits between
nodes on `alphaX`, `alphaY` AND `phi`, and its residual is the aniso
grid's own interpolation error (DL-105 / DL-77's tracked `ANISO_PHI` and
low-alpha residual); the `3.97 -> 4.04` move is that re-baked sub-table's
noise, not a change in kind. The straight line happening to read 2.42%
there is a coincidence of two errors pointing opposite ways, not evidence
for it.

**Control**: at `cosTheta = 0.03` — the first ORDINARY interpolation
span, just above `c0` — all three headers agree to every printed digit
(e.g. `alpha=0.02` G2: 3.32% / 3.32% / 3.32%). The fix touches nothing
above `c0`, and `GGXHeightCorrelatedEnergyLUTTest` now asserts that as a
labelled control group rather than leaving it implicit. Round 2 keeps
that property EXACTLY: every anisotropic `cos=0.03` control row prints
the identical residual before and after the refinement (`0.190382`,
`0.239462`, `2.225795`, `2.420756`, `0.168196`, `0.121612` — to the last
printed digit), because nothing at or above `c0` reads a sub-node.

### Production-BRDF furnace (independent of the LUT harness)

`GGXDiffuseTransmissionTest` integrates the REAL `GGXBRDF`/`CookTorranceBRDF`
over the outgoing hemisphere with its own 50/50 cosine+VNDF mixture
estimator, F=1 specular-only (diffuse=0), 400k samples. Selected rows
(SE `0.0004`–`0.0021`):

| row | flat | line | baked |
|---|---|---|---|
| GGX Schlick alpha=1.0 theta=89.80 | 1.04585 | 0.99539 | **1.00049** |
| GGX Schlick alpha=0.3 theta=89.60 | 1.01264 | 0.99739 | **0.99915** |
| GGX Schlick alpha=0.05 theta=89.40 | 1.02054 | 1.00035 | **1.00315** |
| GGX Schlick alpha=0.01 theta=89.60 | 1.00375 | 0.94668 | **0.99962** |
| GGX Schlick alpha=0.01 theta=89.80 | 1.03575 | 0.95739 | **0.99682** |
| GGX Schlick alpha=0.02 theta=89.60 | 1.02830 | 0.97817 | 1.01496 (DL-105) |
| GGX Schlick alpha=0.005 theta=89.80 | 1.00619 | 0.92756 | 0.96729 (DL-105) |
| CT conductor alpha=0.02 theta=89.89 | 1.03127 | 1.05222 | **1.00585** |
| CT conductor alpha=0.05 theta=89.89 | 1.02487 | 1.01488 | **0.99868** |
| CT conductor alpha=0.01 theta=89.80 | 1.01464 | 1.04726 | **1.00425** |
| GGX aniso (0.9, 0.1) theta=89.89 az=90 | 1.15718 | 0.97388 | **1.00043** |
| GGX aniso (0.1, 0.9) theta=89.89 az=0 | 1.15650 | 0.97302 | **0.99960** |

The last two rows are the anisotropic grazing case the review named
specifically, written both ways round (`alphaX>alphaY` and the
axis-swapped `alphaX<alphaY`, since DL-77's own P1 was an axis-swap bug
that every same-signed row was blind to). They carry the section's
tightest band (`3*SE + 0.005`, i.e. +/-0.0088 at this SE) because the
baked aniso sub-grid closes them to within 0.05% of 1.

Round 2 adds the 100:1 pair at `theta=89.99` (`cosView ~ 1.745e-4`),
which lands in the first sub-interval the round-1 grid left un-bracketed,
plus the two DL-105 rows the round-2 review measured. Same estimator,
400k samples:

| row | r1 | r2 |
|---|---|---|
| GGX aniso (0.01, 1.0) theta=89.99 az=0 | 0.96506 +/- 0.00147 | **1.00208 +/- 0.00142** |
| GGX aniso (1.0, 0.01) theta=89.99 az=90 | — | **0.99757 +/- 0.00142** |
| GGX Schlick alpha=0.005 theta=89.89 | — | 0.96970 +/- 0.00151 (DL-105) |
| CT conductor alpha=0.005 theta=89.40 | — | 1.03432 +/- 0.00217 (DL-105) |

The first row is the review's material-level finding: a **3.5% deficit**,
outside this section's own band, at a configuration no material row
drove. The last two are DL-105, not DL-86 — pinned at their MEASURED
values with the cause named, like the three DL-105 rows already in that
test. The round-2 review measured them at `0.96835 +/- 0.00152` and
`1.03649 +/- 0.00219`; the values pinned in the test are this slice's own
re-measurement at its own seed, and each band (`3*SE + 0.010`) contains
the other reading.

The `line` column is the reviewer's central finding restated at material
level: the first closure turned a bounded GAIN into a **7.2% DEFICIT** at
`alpha=0.005, theta=89.80` (`1.00619 -> 0.92756`), larger than the
`1.04585` gain it was fixing at `alpha=1.0`.

The `CT conductor` rows are new coverage (review finding P1-2). Before
this pass NO test drove `LookupEss` — the separable table
`CookTorranceBRDF`/`CookTorranceSPF` actually render with — through a
material at all; `GGXHeightCorrelatedEnergyLUTTest` used it only as a
NEGATIVE control against the height-correlated quadrature. The separable
path could therefore have been "fixed" for GGX and left broken for
Cook-Torrance with no row going red, which is precisely what the first
closure did.

### Sampler/pdf self-consistency

`MSPdf`/`MSPdfG2` call `LookupEss`/`LookupEssG2` DIRECTLY, while
`SampleMSCosTheta`/`SampleMSCosThetaG2` draw from
`MSLobeDetail::BuildSegmentsFromRow`'s own segment model — the file's own
header comment guarantees the two "cannot drift apart". The left end-cap
is now `SUB_SIZE` linear pieces reading the SAME baked nodes through the
SAME `SubNodeEss`/`SubNodeEssG2` accessors the lookups use, so the two
descriptions are the same function by construction rather than by two
matching formulas. Segment arrays grew `LUT_SIZE+1` → `LUT_SIZE+SUB_SIZE`
(33 → 40) at every call site. All three inputs (`essRow`, `subRow`,
`essLimit`) are LINEAR in the alpha blend, so `MSLobeZ`/`MSLobeZG2`'s
documented "Z is an exact affine function of `af`" per-row-then-blend
optimization is preserved unchanged.

Round 2 keeps that by construction for the ANISO pair too:
`BuildSegmentsFromRowN` reads its node abscissae from the same
`AnisoSubNodeCos` that `LookupEssG2Aniso`/`LookupEssG2AnisoDirectional`
locate `cosTheta` with (`AnisoSubNodeIndex`), and the aniso segment
arrays grew `ANISO_COS_SIZE+SUB_SIZE -> ANISO_COS_SIZE+ANISO_SUB_TOTAL`
(40 -> 45) at every call site. `GGXHeightCorrelatedEnergyLUTTest` pins it
directly rather than by argument: `2*PI * int_0^1 MSPdfG2Aniso dc` is
`1.00000000` at four `(alphaX, alphaY)` pairs (composite Simpson, refined
separately on `[0,c0]`), and a 40M-draw histogram of
`SampleMSCosThetaG2Aniso` at `(0.01, 1.0)` matches that density in every
sub-grid-scale bin with an expected count above 50 (worst `|z| = 1.65`).
Both checks are CONSISTENCY PINS — green before and after — and exist
because the refinement changes a node list two independent pieces of code
read.

The same obligation binds the anisotropic pair, and the first closure
left it unmet in the other direction: it fixed
`LookupEssG2AnisoDirectional` but deliberately left `LookupEssG2Aniso`
(azimuth-averaged, sampler-only) and `BuildSegmentsFromRowN` flat — which
also left `LookupEssG2Aniso` with a STEP in its proposal shape as
`alphaX -> alphaY`, since its own `alphaX==alphaY` fallback forwards to
the now-non-flat `LookupEssG2`. Both now use the derived phi-averaged
sub-table, so the aniso sampler and `MSPdfG2Aniso` read one model and the
isotropic limit is continuous (review findings P2-2/P3). The `[0,1]`
clamp asymmetry the review also flagged is resolved the same way: every
sub-node value is a baked directional albedo in `[0,1]` and the `k=0` G2
boundary is exactly 1, so a convex combination of them cannot leave
`[0,1]` — the lookups' defensive `r_max`/`r_min` is a documented no-op and
the segment form is describing the identical function, not a laxer one.

### The generator regenerates this header (review finding P1-3)

`tools/GenerateMicrofacetEnergyLUT.cpp` was NOT updated by the first
closure: regenerating on `aff019de` produced a header differing from the
checked-in one by 226 lines / 197 changed lines — i.e. the next
regeneration would have silently REVERTED the entire fix. The generator
now bakes and emits all five new tables, and carries the updated lookup
code in its `printf` blocks and in both verbatim hand-maintained blocks
(`kHandMaintainedH6Block`, `kHandMaintainedDL77AnisoBlock`). Verified by a
full regeneration: `diff` against the checked-in header is **0 lines**.
Round 2 was made GENERATOR-FIRST for the same reason — the node layout,
the bake, the emitted table dimensions and the emitted lookup code are
all edited in `tools/GenerateMicrofacetEnergyLUT.cpp`, and the checked-in
header is that generator's output verbatim.

### Residual, tracked as DL-105

DL-105's own summary used to say "up to 5% in `Ess` (and 3% in a furnace
mean)". The furnace half is understated: the SEPARABLE path reaches
**+3.43%** (`CookTorranceBRDF` conductor `alpha=0.005, theta=89.40`,
`1.03432 +/- 0.00217`; the round-2 review read `+3.65%` at its own seed).
The sign flips with angle at a fixed alpha below the table's range: it is
a GAIN below roughly 89.5 degrees and a DEFICIT beyond it (GGX Schlick
`alpha=0.005`: `+2.73%` at `theta=89.40`, `-3.27%` at `89.80`, `-3.03%`
at `89.89`), which is why a one-sided furnace gate cannot see half of it.

What remains at the low end is the ALPHA axis, not the cosTheta end-cap,
and it does not move with `SUB_SIZE` (measured at 4 / 8 / 16: `alpha=0.01`
moves 0.41% → 0.11% → 0.02% while `alpha=0.015`/`0.02` sit at ~1.4–1.7%
throughout). `alpha` below 0.01 is clamped to row 0 outright — and
`GGXBRDF.cpp` clamps roughness only at `1e-4`, so `alpha=0.005` really
reaches the table — while row 0 (`alpha=0.01`) to row 1 (`alpha=0.0419`)
is a 4.2x ratio inside a single linear interpolation cell. Opened as
**DL-105** with the measurements above as its evidence.

## DL-105: low-alpha sub-grid (isotropic E_ss/E_avg, both models)

**Status: CLOSED 2026-09-17** — debt-dl105 slice, base `e1772631`.

**Root cause** (confirmed exactly as the ledger row diagnosed): every
isotropic alpha lookup (`LookupEss`, `LookupEssG2`, `LookupEavg`,
`LookupEavgG2`, `MSLobeDetail::BuildSegments{,G2}`, `MSLobeZ{,G2}`) maps
`alpha` via `a = clamp((alpha-0.01)/0.99, 0, 1) * (LUT_SIZE-1)`, so (a)
`alpha < 0.01` clamps to row 0 outright — and `GGXBRDF.cpp`/`GGXSPF.cpp`
only floor authored roughness at `1e-4`, so `alpha=0.005` really reaches
the table — and (b) row 0 (`alpha=0.01`) to row 1 (`alpha=0.0419`) is a
4.2x ratio inside ONE linear interpolation cell.

**Fix: the SAME construction DL-86 used, applied to the alpha axis.**
DL-86 baked a sub-grid on `[0, c0]` (the cosTheta axis) anchored at a
PROVABLE exact boundary (`Ess_G2(cosTheta->0) = 1`). The alpha axis has
an even cleaner boundary: Smith `Lambda(v) = (-1+sqrt(1+alpha^2*tan^2
theta))/2 -> 0` as `alpha -> 0` for ANY FIXED `cosTheta > 0` (ordinary
bin or DL-86 sub-node), so `G1 -> 1` and `G2 -> 1` UNCONDITIONALLY —
`Ess(alpha->0, cosTheta) = 1` for BOTH the height-correlated G2 model
AND the separable model (unlike DL-86's cosTheta boundary, which needed
a baked constant for the separable model's limit). No new limit table
is needed for this boundary at all.

Two zones are baked below/between the main table's row 0 and row 1,
covering both (a) and (b) above in one construction:
  * `ALPHA_SUB_FINE = 7` geometric octaves on `(0, 0.01)`: node `j`
    stores `alpha = 0.01/2^(8-j)` for `j=1..7`, i.e. `0.005` down to
    `7.8e-5` — comfortably past `GGXBRDF.cpp`'s `1e-4` roughness floor.
  * `ALPHA_MID_SIZE - 1 = 3` geometric nodes on `(0.01, 0.0419]`,
    bridging the coarse first cell.

Each of the 10 stored low-alpha nodes gets its own FULL ordinary
cosTheta row (32 cells, matching `E_ss_TABLE`/`E_ss_TABLE_G2`) AND its
own DL-86-style grazing sub-row + separable-model limit constant
(matching `E_ss_SUB_TABLE{,_G2}`/`E_ss_LIMIT_TABLE`) — necessary because
the DL-105 furnace rows this closes (`theta=89.40..89.89`) land INSIDE
the DL-86 grazing cosTheta sub-grid (`cos < c0 = 0.0156`), so a query
combining a low alpha AND a grazing cosTheta needs a value baked AT that
alpha, not one blended in from row 0/row 1's already-resolved grazing
sub-rows. Interpolation is LINEAR IN ALPHA on every interval (node
PLACEMENT is geometric, the BLEND between two adjacent nodes is not —
matching `ANISO_SUB_FINE`'s precedent). New tables:
`E_ss_ALPHA_LOW_TABLE{,_G2}[10][32]`, `E_ss_ALPHA_LOW_SUB_TABLE{,_G2}
[10][7]`, `E_ss_ALPHA_LOW_LIMIT_TABLE[10]` (separable only — the G2
model's boundary is exactly 1 at every alpha), `E_avg_ALPHA_LOW_TABLE
{,_G2}[10]` (same midpoint-rule discretization over the 32 ordinary
bins the main `E_avg_TABLE{,_G2}` already uses). Drawn from a THIRD
independently-seeded LCG stream (`rng_state_lowalpha`), so every
pre-existing table — including the DL-86 sub-grid, already fully baked
before this stream is touched — stays byte-for-byte identical.
Regenerating `tools/GenerateMicrofacetEnergyLUT.cpp` reproduces the
header with every pre-existing line UNCHANGED and only NEW lines added
(`diff` against the pre-fix header: 0 removed / changed lines, 398
added).

**MS-sampler consistency**: `BuildSegments`/`BuildSegmentsG2` (the H6
sampler's segment builder) and `MSLobeZ`/`MSLobeZG2` (the cached Z
normalization) gained the identical `alpha < ALPHA_LOW_A1` branch,
sourcing the SAME `BuildLowAlphaRow{,G2}` helper `LookupEss`/`LookupEssG2`
use — so the sampler, its reported density, and the lookup cannot drift
apart in the low-alpha regime, the same "cannot disagree" contract
`SubNodeEss`/`SubNodeEssG2` established for the cosTheta axis. One
deliberate exception: `MSLobeZ{,G2}`'s cached-affine-per-row optimization
(precomputing `Z` once per of the 32 main rows and blending) is NOT
extended to the 10 new low-alpha nodes — below `ALPHA_LOW_A1` these
functions recompute `Z` directly via `BuildSegmentsFromRow`+`SegTotal`
every call, a deliberate perf/complexity trade-off for a rare regime
(very smooth surfaces, `alpha < 0.0419`) rather than precomputing and
affine-blending `Z` at 13 additional virtual node positions. Correctness
does not depend on the cache — `MSPdf`/`MSPdfG2` call the (low-alpha-
aware) `LookupEss`/`LookupEssG2` directly, and `SampleMSCosTheta`/
`SampleMSCosThetaG2` call the (low-alpha-aware) `BuildSegments`/
`BuildSegmentsG2` — both read the SAME `BuildLowAlphaRow{,G2}` source.

**Red-proof** (`GGXHeightCorrelatedEnergyLUTTest`, `TestDL86IsotropicEndCap`/
`TestDL86SeparableEndCap` rows re-tightened from their DL-105-loose
tolerances): worst relative error against an independent 20M-sample
VNDF quadrature over `cosTheta in [1e-4, c0]`:

| lookup | alpha | pre-fix % | post-fix % |
|---|---|---|---|
| `LookupEssG2` | 0.005 | 5.10 | **0.42** |
| `LookupEssG2` | 0.02 | 1.59 | **0.03** |
| `LookupEss` | 0.005 | 5.52 | **0.18** |
| `LookupEss` | 0.02 | 1.58 | **0.03** |

The above-`c0` control row (`cos=0.03`) at `alpha=0.005` — previously
untracked because that alpha clamped to row 0 regardless of cosTheta —
now reads through the SAME low-alpha branch and measures `~1.5%`
relative, the same order as the OTHER alphas' pre-existing (unrelated,
DL-105-adjacent but out of this fix's scope) ordinary-interior-grid
residual at that span; tightened from a `0.060` placeholder to the
`0.040` band the other alphas already carry.

**New independent cross-check** (review requirement, `TestDL105IndependentVNDFCrossCheck`):
a THIRD estimator, sharing no code with `LookupEssG2`, the offline
generator, OR `MonteCarloEssG2`/`UniformHemisphereEssG2` above — its own
`D_Isotropic`/`Lambda`/`G2_HeightCorrelated` re-implementation (the
pre-existing `IndependentGGX` namespace) PLUS an own from-scratch
Heitz-2018 VNDF sampling routine (`IndependentGGX::VNDFSample`), needed
because `UniformHemisphereEssG2`'s own comment already disqualifies it
below `alpha=0.01` ("much worse importance sampler ... for peaked
(low-alpha) configurations"). 9 points: 7 at mid-range `cosWi` (`alpha`
in `{0.005, 0.0025, 0.00125, 0.0008, 0.0003, 0.0001, 0.008}` — three
exact sub-grid nodes, three off-node, one in the widest sub-interval
`[0.005, 0.01]`), 20M samples each — worst diff `0.00155` absolute
(`alpha=0.008`), every other point within `2e-5` — `Ess(alpha, mu>=0.3)`
is essentially exactly 1 in this regime, confirming the boundary-
condition derivation directly rather than only through the furnace/LUT-
harness rows above; plus 2 points combining a low alpha WITH a grazing
`cosWi` (`alpha=0.005,mu=0.001` and `alpha=0.02,mu=0.005`) — the
configuration that actually discriminates this fix from the pre-fix
clamp-to-row-0 behavior (the alpha boundary alone reads close to 1
whether it clamps to the true row or to row 0, since row 0's own
`alpha=0.01` is already small; only a grazing `mu` exposes the alpha
AXIS defect) — reading `0.42%`/`0.007%` diff respectively, matching the
LUT-table residuals above measured through an entirely independent
estimator.

**Production-BRDF furnace** (`GGXDiffuseTransmissionTest`, the row's
original evidence, F=1 specular-only): the five DL-105-pinned rows all
return to `expected = 1.0`:

| row | pre-fix (pinned) | post-fix (measured) |
|---|---|---|
| GGX Schlick alpha=0.02 theta=89.60 | 1.01496 | **1.00080** |
| GGX Schlick alpha=0.005 theta=89.40 | 1.02734 | **1.00640** |
| GGX Schlick alpha=0.005 theta=89.80 | 0.96729 | **1.00526** |
| GGX Schlick alpha=0.005 theta=89.89 | 0.96970 | **1.00186** |
| CT conductor alpha=0.005 theta=89.40 | 1.03432 | **1.00681** |

All five pass the same two-sided `3*SE + 0.010` band every other row in
that table uses (no widened tolerance needed).

**Cost**: bake time grew from the DL-86-slice's baseline to `~6m35s`
total (single-threaded, no internal parallelism) — the new low-alpha
bake is `10 stored nodes * (32 ordinary + 7 grazing) cosTheta positions
* 1,000,000 samples * 2 models ~= 7.8e8 extra samples`, roughly 30-40%
more than the pre-existing isotropic bakes combined. Table footprint:
10*32*2 (ordinary) + 10*7*2 (grazing) + 10 (limit) + 10*2 (Eavg) = 820
new `Scalar` values — negligible next to the DL-77 aniso tables'
hundreds of thousands. Runtime lookup cost is unchanged for
`alpha >= ALPHA_LOW_A1` (the pre-existing branch, byte-identical);
below it, `LookupEss`/`LookupEssG2` do a 13-element linear scan
(`AlphaLowIndex`) instead of an O(1) formula, and `MSLobeZ{,G2}` pay a
full segment rebuild instead of the cached-row blend — both scoped to
the rare `alpha < 0.0419` regime.

**Sibling audit** (`docs/skills/audit-by-bug-pattern.md`): the SAME
`clamp((alpha-0.01)/0.99, 0, 1)` pattern, on the SAME `[0.01, 1.0]`
range, was found in two more places, both left OPEN as new debts rather
than folded into this slice (out of scope: extending either needs its
own bake, its own boundary-condition derivation, or both):
  * **DL-160** — `tools/GGXSpecularBihemisphericalGen.cpp`'s
    `kGGXSpecularQuadWeight[32][8]` table (consumed by
    `GGXBRDF::hemisphericalAlbedo{,NM}`, DL-123) resolves its OWN,
    independently-baked alpha axis with the identical uniform
    `[0.01,1.0]` 32-row mapping and the identical clamp, at
    `GGXBRDF.cpp:753`. Confirmed present; not measured or fixed here.
  * **DL-161 (CLOSED 2026-09-18, debt-dl161 slice — see the dedicated
    "DL-161" section below)** — the DL-77 ANISOTROPIC alpha axis (`AnisoAlphaIndex`,
    consumed by `LookupEssG2Aniso`/`LookupEssG2AnisoDirectional`/
    `LookupEavgG2Aniso`/`MSLobeZG2Aniso`/`SampleMSCosThetaG2Aniso`) has
    the SAME defect, independently on both `alphaX` and `alphaY` (node0
    `0.01` to node1 `0.0530` is a 5.3x ratio, worse than the isotropic
    table's 4.2x) — deliberately NOT extended by this slice. The
    isotropic fix improves the aniso ALPHA-DIAGONAL case for free
    (`LookupEssG2AnisoDirectional`/`LookupEssG2Aniso`/`MSLobeZG2Aniso`/
    `SampleMSCosThetaG2Aniso` all already forward to their isotropic
    twin when `fabs(alphaX-alphaY) < 1e-9`, confirmed unchanged and
    still forwarding), but the OFF-diagonal low-alpha case
    (`alphaX != alphaY`, one or both `< 0.01`) is untouched — confirmed
    by re-running `GGXHeightCorrelatedEnergyLUTTest`'s aniso rows
    (identical numbers before/after, e.g. the debt-ggx3-cited
    `(0.0361, 0.9627, phi=5)` corner still reads `2.99-4.04%`). Unlike
    the isotropic case, the anisotropic `alphaX->0` (fixed `alphaY`)
    boundary is NOT simply `Ess=1`: `Lambda_Aniso`'s effective alpha for
    a direction `v` is `sqrt((alphaX*cosPhiV)^2+(alphaY*sinPhiV)^2)`,
    which stays finite (hence `G1<1`, masking persists) whenever `v`'s
    azimuth is not exactly aligned with the vanishing axis — a genuine,
    non-constant boundary FUNCTION of `(alphaY, phi, cosTheta)`, not a
    constant, so extending this needs a bake of comparable scope to the
    DL-77 slice itself, not an additive sub-grid the isotropic
    boundary's simplicity allowed here.

**Gate**: `GGXHeightCorrelatedEnergyLUTTest` 261/0 (was 252/0 pre-fix,
with looser DL-105 tolerances), `GGXDiffuseTransmissionTest` 190/0 (was
190/0 pre-fix with the five rows pinned at their measured non-1.0
values), `GGXSampleEvaluationConsistencyTest` 48/0, `GGXWhiteFurnaceTest`
pass, `GGXMetalRoughGridTest` pass, `LayeredWhiteFurnaceTest` 0/57,
`CookTorranceMultiscatterTest` 17/0, `CookTorranceSchlickGlossyFilterConsistencyTest`
22/0, `ThinFilmFurnaceTest` 4/0, `GGXHemisphericalAlbedoTest` 28/0
(unaffected — DL-123's own table has the DL-160 residual, untouched),
`FabricRenderTest` 60/0, `CstDeriveGoldenTest` (0 drift), `SourceHygieneTest`
165/0; clean warning-free rebuild (incremental + full).

**Render sanity**: `scenes/Tests/Materials/ggx_anisotropy_sweep.RISEscene`
does not exercise `alpha < 0.02` on any of its 9 cells, so a scratch
copy of `scenes/Tests/Materials/ggx_roughness_sweep.RISEscene` (its
`alpha=0.01` gold sphere is the lowest alpha this fix touches) was
rendered instead, patched with `oidn_denoise FALSE`, `pixel_filter box`
and EXR `Rec709RGB_Linear` output. Before/after (isolated build via a
temporary header swap + targeted recompile of the 6 dependent `.cpp`
files, same PT seed both times, 64spp): whole-image luminance mean
`1.009019 -> 1.008971` (-0.0048%), top-left quadrant (containing the
`alpha=0.01` sphere) `1.451777 -> 1.451567` (-0.014%), no NaN/Inf either
render. This is EXPECTED to be small and is reported as a sanity check,
not a strong regression signal: the fix's effect is confined to extreme
grazing incidence (`theta > ~89.1` degrees, i.e. a thin band of pixels
right at the sphere's silhouette) on an alpha the pre-existing DL-86
sub-grid had already brought to <=0.11% residual at `alpha=0.01`
specifically (the row least affected by this slice — `alpha=0.005` and
the `0.01<alpha<0.0419` cell, this fix's main targets, are BELOW this
scene's smallest authored alpha). The furnace-level and LUT-level tables
above are the decisive evidence for this fix; the render confirms no
corruption, not a visible before/after difference at this alpha.

## DL-160: low-alpha sub-grid (GGXBRDF's own bihemispherical quadrature weights)

**Status: CLOSED 2026-09-18** — debt-dl160 slice, base `50dbc4d8`.

**Root cause**: `GGXBRDF.cpp`'s `LookupGGXSpecularQuadWeight` (the
lookup for `kGGXSpecularQuadWeight[32][8]`, DL-123's own baked
moment-matched bihemispherical quadrature, `tools/GGXSpecularBihemisphericalGen.cpp`)
resolved its alpha axis with the SAME
`a = clamp((alpha-0.01)/0.99, 0, 1) * 31` mapping DL-105 fixed on
`E_ss_TABLE`/`E_ss_TABLE_G2`/`E_avg_TABLE{,_G2}` — `alpha < 0.01`
clamped to row 0 outright, and row 0 (`0.01`) to row 1 (`0.0419`) was
the same 4.2x-in-one-cell first interpolation interval.

**Measured pre-fix (red-proof)**: `tests/GGXHemisphericalAlbedoTest.cpp`'s
new `TestLowAlphaVNDF` — an independent VNDF-importance-sampled
bihemispherical estimator (own D/Lambda/G1 + Heitz-2018 VNDF sampling,
own RNG) calling the REAL `GGXBRDF::value()` directly, needed because a
uniform angular grid (as `TestBihemispherical`'s own `Bihemispherical()`
uses) cannot resolve a GGX lobe this narrow — measured, at Schlick F0=0
(worst case; the defect is a nearly F0-independent ABSOLUTE weight-sum
error, so the SMALLER the true reflectance the LARGER the relative
error): `alpha=0.002` reads **1.10%** high (`0.047097` vs `0.047620`);
`alpha=0.005` **0.80%**; `alpha=0.008` **0.35%**; `alpha=0.015`
**0.34%**; `alpha=0.03` **0.49%**. At F0=1 the SAME absolute defect
(~5e-4) reads as a much smaller relative error (~0.05%) because the
true value there is near 1 — this is why the pre-existing
`TestBihemispherical`/`FabricMaterialChunkTest` gates, which only ever
exercised alpha>=0.03 or F0 near 1 or both, never caught it.

**Boundary condition — derived, not guessed** (the ledger row
explicitly flagged this: "re-derive before assuming", since this
table's `alpha->0` limit is a QUADRATURE WEIGHT VECTOR, not a
directional albedo, so DL-105's own "`Ess=1`" boundary does not carry
over unchanged). A first, plausible-looking guess — "`alpha->0` makes
the lobe a delta at the mirror direction (`muH->1` for every `wi`), so
the answer is a weight delta at `kGGXSpecularQuadNodes[0]`" — is WRONG.
For a FIXED `wi` at polar angle `theta_i`, the `alpha->0` limit
mirror-reflects `wi` about the MACROSCOPIC normal `n`, not about `wi`
itself, so `h->n` and `muH=dot(wi,h)->dot(wi,n)=mu_i`, NOT 1, unless
`wi` is ALSO at normal incidence. Since this table's target quantity
integrates over ALL `wi` (isotropic, weighted `mu_i*dmu_i`), the
`alpha->0` bihemispherical single-scatter reflectance of ANY Fresnel
function `F` is the well-known perfect-mirror result
`R_ss(alpha->0) = 2*INT_0^1 F(mu)*mu dmu` (the same reduction
`E_avg`'s own definition uses, `F` in place of `Ess`), giving boundary
MOMENTS (in `u=1-cosThetaH`) `Moments_k(alpha->0) = 2/((k+1)(k+2))` for
ANY `F` — verified numerically: the max `|weight|` difference between
this closed form and the actual baked `alpha=0.01` row is only `~0.0015`
(row 0 is already close to the true limit), against the `~0.28`
difference the delta-at-node-0 guess would have predicted at node 2.
Solved via the same fixed Vandermonde system every other row uses.

A second, smaller correction on top of that: the boundary must be
baked through the SAME `numMuI=48`-bin midpoint discretization every
other row in this table uses, not the raw continuum integral — an
exact continuum closed form measurably DISAGREES with where the
MC-baked `idx=1..7` rows already converge (`~0.0012` at node 0, purely
from the outer `wi` quadrature's own `O(dmu^2)` truncation error, an
artifact unrelated to the alpha axis this fix targets, confirmed by
hand: the discretized closed form matches the MC-baked `idx=1`
row — `alpha=7.8e-5` — to `<3e-7`).

**Fix**: the SAME `ALPHA_SUB_FINE=7`/`ALPHA_MID_SIZE=4` low-alpha
sub-grid construction DL-105 used, applied to `kGGXSpecularQuadWeight`'s
own bake in `tools/GGXSpecularBihemisphericalGen.cpp` (7 geometric
octaves below 0.01 down to `7.8e-5`, 2 geometric mid-nodes bridging
`(0.01, 0.0419]`, each row solved through the same fixed Vandermonde
system as every ordinary row), plus the closed-form `alpha->0` anchor
above. `GGXBRDF.cpp`'s `LookupGGXSpecularQuadWeight` now routes
`alpha < MicrofacetEnergyLUT::ALPHA_LOW_A1` through the new
`kGGXSpecularQuadWeightAlphaZero`/`kGGXSpecularQuadWeightAlphaLow`
tables via the SHARED `MicrofacetEnergyLUT::AlphaLowIndex`/
`AlphaLowSlot`/`ALPHA_SUB_FINE`/`ALPHA_LOW_TOTAL` bracket — the exact
same axis and helpers DL-105 already ships and tests, reused rather
than reimplemented, so this table's bracketing cannot drift from
DL-105's. Regenerating `tools/GGXSpecularBihemisphericalGen.cpp` and
diffing its output against the pasted tables in `GGXBRDF.cpp` is a
clean 0-line diff (verified).

**Residual (post-fix, same VNDF estimator)**: `alpha=0.002` F0=0 error
drops **1.10% -> 0.01%**; `alpha=0.005` **0.80% -> 0.01%**;
`alpha=0.03` **0.49% -> 0.19%** (the residual at the top of this
table's own fixed range is expected — `alpha=0.03` sits inside the
2-node geometric mid-zone bridging to row 1, the same coarsest part of
DL-105's own construction). All under the test's 4e-4 absolute
tolerance.

**Gate**: `GGXHemisphericalAlbedoTest` 40/0 (was 34/6 with this slice's
own new rows; 28/0 unaffected on the pre-existing rows), `GGXHeightCorrelatedEnergyLUTTest`
261/0, `GGXDiffuseTransmissionTest` 190/0, `GGXWhiteFurnaceTest` pass,
`LayeredWhiteFurnaceTest` 0/57, `FabricMaterialChunkTest` 178/0 (was
170/0; +8 checks from a new low-alpha GGX row added to gate 5(b) —
its informational "substr%" column is unaffected by this fix, `8.682%
-> 8.743%`, because that gate's own 64x128 uniform-grid
`BruteForceIntegrals` reference cannot resolve alpha this low either,
the same limitation this fix's own red-proof needed VNDF sampling to
get past), `CoatedMaterialChunkTest` 85/0 (no low-alpha GGX row
present), `FabricRenderTest` 60/0, `GGXSampleEvaluationConsistencyTest`
48/0, `CstDeriveGoldenTest` 452 MATCH/0 DRIFT, `SourceHygieneTest`
167/0; clean warning-free rebuild (incremental + full).

**Sibling audit**: re-ran the `(alpha-0.01)/0.99`-style grep across
`src/Library` and `tools/`. Nothing new: `MicrofacetEnergyLUT.h`'s four
remaining occurrences of the raw formula are the DL-105-fixed
functions' own `alpha >= ALPHA_LOW_A1` fallthrough branch (guarded,
confirmed by reading each call site); `SheenDirectionalAlbedo`'s
`kNumAlphaBins` axis is LOG-spaced, a structurally different
construction with no analogous low-end coarseness; **DL-161** (the
anisotropic `AnisoAlphaIndex` off-diagonal case) is the only other live
instance, already filed by the DL-105 slice, confirmed still open and
correctly out of scope here — its boundary is a non-constant function
of `(alphaY, phi, cosTheta)`, not the simple closed form this slice
used, and closing it needs a bake of comparable scope to the DL-77
slice itself. No new debt row opened.

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
end-cap (`cos<0.0156`) was tracked as DL-86 and is unaffected by
this fix, isotropic or anisotropic. **Status correction, 2026-09-14
(debt-dl86 slice):** DL-86 is now CLOSED (see its own section above), and
its closure re-attributes this passage's own cited worst case. The
`alphaX=0.0361,alphaY=0.9627,cos=0.0024,phi=5.0` point is off-node on all
THREE interpolated axes (alphaX between 0.01 and 0.0530, alphaY between
0.9570 and 1.0, phi between 0 and 7.5 degrees), so it is NOT a clean
DL-86 end-cap measurement: after DL-86's baked sub-grid it still reads
`3.97%`, while NODE-EXACT configurations — where cosTheta is the only
interpolated axis, which is what actually isolates the end-cap — drop
from up to `28.90%` to at most `0.72%`. The residual at that corner is
(a) and (b) above, now tracked as **DL-105**. `GGXDiffuseTransmissionTest`'s two-sided
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

## DL-161: anisotropic low-alpha sub-grid (both axes)

**Status: CLOSED 2026-09-18** — debt-dl161 slice, base `50dbc4d8`.

**Root cause** (confirmed exactly as the ledger row and DL-105's own
sibling audit diagnosed): the DL-77 aniso alpha axis (`AnisoAlphaIndex`)
maps `alphaX`/`alphaY` independently via the SAME
`clamp((alpha-0.01)/0.99,0,1)*(ANISO_ALPHA_SIZE-1)` pattern DL-105 fixed
on the isotropic tables — `alpha<0.01` clamps to row 0 outright, and row
0 (`alpha=0.01`) to row 1 (`alpha=0.053043`) is a 5.3x ratio inside one
linear interpolation cell (worse than the isotropic axis's 4.2x, since
`ANISO_ALPHA_SIZE=24` is coarser than `LUT_SIZE=32`). DL-105's own
`fabs(alphaX-alphaY)<1e-9` isotropic-diagonal shortcut already fixes the
`alphaX==alphaY` case for free; the OFF-diagonal case (one or both axes
low, the two unequal) was untouched.

**Why this needed a bake, not a derivation**: unlike the isotropic
axis's `alpha->0` boundary (provably `Ess=1` for any fixed `cosTheta>0`,
since Smith `Lambda(v)->0`), the anisotropic `alphaX->0` boundary (fixed
`alphaY`) is NOT simply 1: `Lambda_Aniso`'s effective alpha for a
direction `v` is `sqrt((alphaX*cosPhiV)^2+(alphaY*sinPhiV)^2)`, which
stays FINITE whenever `v`'s azimuth is not exactly aligned with the
vanishing axis, so masking persists and the true limit is a
non-constant function of `(alphaY, phi, cosTheta)`. The ledger row's own
recipe therefore prescribed baking that boundary rather than assuming a
closed form, "a bake of comparable scope to the DL-77 slice itself."

### Fix

Two new bake families, reusing DL-105's existing 10-node `AlphaLowNode`/
`AlphaLowSlot` machinery (generic over `A0`/`A1`, so no new generator-side
node function was needed) re-anchored to the ANISO grid's own row1
(`ANISO_ALPHA_LOW_A1 = 0.01 + 0.99/(ANISO_ALPHA_SIZE-1) = 0.053043...`,
not the isotropic axis's `0.041935`):

  * `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[10][24][13][32]` (+ grazing
    sub-grid twin `_X_SUB[10][24][13][12]`) — a low alphaX (the 10
    stored virtual nodes) paired against the FULL ordinary 24-node
    alphaY grid, at full phi/cosTheta resolution. A genuine rectangular
    bake (no symmetry between the low-node set and the ordinary grid).
  * `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY[10][10][13][32]` (+
    `_XY_SUB[10][10][13][12]`) — the corner where BOTH axes are low.
    Relabel-symmetric exactly like the main DL-77 table
    (`Ess(a,b,phi)==Ess(b,a,90-phi)` holds for the low-alpha nodes too),
    so only the canonical `ix<=iy` half is Monte-Carlo baked and the
    rest mirrored (phi -> 90-phi), zero extra noise.
  * `E_avg_TABLE_G2_ANISO_ALPHALOW_X[10][24]` and `_XY[10][10]` extend
    `LookupEavgG2Aniso` — a REAL energy-compensation call site (the
    `F_ms` multiscatter term in `GGXBRDF.cpp`/`GGXSPF.cpp`), not merely
    a sampler input, so it needed the same fix. Derived from an
    EPHEMERAL phi-trapezoidal average of the corresponding Ess table's
    cosTheta rows at bake time (no separate phi-averaged Ess table is
    stored for the low-alpha regime — an economy matching DL-77's own
    derivation of `E_avg_TABLE_G2_ANISO` from `E_ss_TABLE_G2_ANISO`).

A "low alphaY, ordinary alphaX" query is deliberately NOT separately
baked: `LookupEssG2AnisoDirectional`'s dispatch derives it from the SAME
`ALPHALOW_X` table by swapping `(alphaX,alphaY,localX,localY)` before
calling `LookupEssG2AnisoDirectionalLowX` — exactly what the relabel
symmetry means physically, and it halves the bake cost the naive
four-case cross product would need. `LookupEavgG2AnisoLowX` needs no
phi swap at all (Eavg has no azimuth dependence by construction).

The exact `alphaX->0` virtual node (index 0 in `AnisoAlphaLowIndex`) is
never baked. `AnisoAlphaLowIndex`'s dead-zone (`idx<=0`) folds to
index 1's own baked row in every `*Ordinary`/`*Grazing`/`*Eavg`
accessor. This is a documented, deliberate simplification, not a
silently-dropped case: (a) no closed-form limit exists here (that is
this row's whole root-cause finding), and (b) it is PROVABLY
UNREACHABLE from any production roughness — `GGXBRDF.cpp`/`GGXSPF.cpp`
floor authored alpha at `1e-4`, comfortably inside the interval
`[AnisoAlphaLowNode(1)=7.8e-5, AnisoAlphaLowNode(2)~1.56e-4)`, the same
argument DL-105 already relied on for its own isotropic idx-0 boundary
(there, the boundary IS exactly 1 and this is moot; here the boundary
has no closed form, so avoiding it matters more, and the same
unreachability argument licenses skipping it).

**The H6 multiscatter-lobe SAMPLER is deliberately NOT extended.**
`E_ss_TABLE_G2_ANISO`/`E_ss_TABLE_G2_ANISO_SUB` (phi-averaged),
`LookupEssG2Aniso`, `MSLobeZG2Aniso`, `SampleMSCosThetaG2Aniso`,
`MSPdfG2Aniso`, and `MSLobeDetail::BuildSegmentsFromRowN` are BYTE-FOR-
BYTE UNCHANGED — they keep reading the pre-existing `AnisoAlphaIndex`
clamp/coarse-cell behavior below `ANISO_ALPHA_LOW_A1`. This matches the
DL-86 precedent explicitly: the sampler's own precision is an
EFFICIENCY concern, not a correctness one, because `MSPdfG2Aniso`
always reports the density of whatever `SampleMSCosThetaG2Aniso`
actually draws — the estimator is unbiased regardless of how accurate
the proposal shape is. Only the two direct ENERGY-COMPENSATION call
sites (`LookupEssG2AnisoDirectional`, `LookupEavgG2Aniso`) needed the
fix.

### Two bugs found and fixed in-slice (own red-proof, not a review round)

**Bug 1 — the ordinary-path clamp swallowed the low-alpha check.** The
first implementation computed `xLow = aX < ANISO_ALPHA_LOW_A1` from
`aX = r_max(0.01, r_min(1.0, alphaX))` — the SAME `[0.01,1.0]` clamp the
pre-existing ORDINARY interpolation path applies — so every alphaX/
alphaY below 0.01 was silently rounded UP TO 0.01 before the low-alpha
branch ever saw it. `alphaX=0.002` and `alphaX=0.005` both dispatched
as if `alphaX==0.01` and returned IDENTICAL results (caught directly by
this slice's own new test rows: both read `0.880751` at `aY=1.0, phi=0,
cos=0.0005`, against independently-computed reference values of
`0.703040` and `0.816008` respectively). Fixed by checking `xLow`/`yLow`
against the RAW alpha (floored only at 0, not at 0.01) BEFORE the
ordinary path's clamp — matching `LookupEssG2`'s own pre-existing
`if(alpha<ALPHA_LOW_A1){...}` structure, which checks first and clamps
only in the untaken branch.

**Bug 2 — missing boundary dispatch in the new grazing-sub-grid
accessors.** `AnisoPhiSubCellMixedX`/`AnisoPhiSubCellBothLow` (the new
accessors backing the `_SUB` tables) were missing the `k<=0` /
`k>=ANISO_SUB_TOTAL` boundary handling the pre-existing
`AnisoPhiSubNodeEssG2` has (`k<=0` is the exact `cosTheta->0` boundary,
exactly 1 for the G2 model; `k>=ANISO_SUB_TOTAL` IS the cell's own
ordinary bin 0, read via the ordinary accessor, NOT a `_SUB`-table
index — the `_SUB` tables only store the `ANISO_SUB_TOTAL-1` INTERIOR
nodes). Without it, a query at `k=ANISO_SUB_TOTAL=13` (reachable at
`cosTheta` near `c0`, e.g. the test suite's own `cos=0.0156` probe) read
one element past the end of the `_SUB` array. This corrupted several
PRE-EXISTING DL-86/DL-77 gate rows that happened to query an alpha now
routed through the low-alpha branch (e.g. `alphaX=0.01`, which is `<
ANISO_ALPHA_LOW_A1`): `node-exact aX=0.01 aY=1 phi=0 cos=0.0156` read
`0.996626` against an independent quadrature of `0.519115`. Caught by
re-running the FULL existing gate suite before declaring the fix done
(not by a downstream review) — 37 pre-existing rows failed, immediately
pointing at the new accessors rather than the bake. Fixed by adding the
identical two-branch boundary dispatch, forwarding `k>=ANISO_SUB_TOTAL`
to the corresponding ordinary-cell accessor at `ci=0`.

### Red-proof

`GGXHeightCorrelatedEnergyLUTTest`'s new "DL-161" section reuses the
pre-existing `TestDL86AnisoDirectionalEndCap` helper verbatim (it is
already generic over `(alphaX,alphaY,phi,cosTheta)`) at 17 low-alpha
configurations: `alphaX` in `{0.002,0.005,0.01,0.015}` crossed with
`alphaY` in `{0.05,0.2,0.5,1.0}`, `phi` in `{0,45,90}` degrees, several
`cosTheta` down to `1e-4` (both ordinary and grazing), plus dedicated
swapped-role (low-Y, ordinary-X) and both-low-axis rows. Worst relative
error against an independent 20M-sample-per-point VNDF quadrature:

| configuration | pre-fix | post-fix |
|---|---|---|
| aX=0.002 aY=1.0 phi=0 cos=0.0005 | 25.28% | 0.95% |
| aX=0.002 aY=1.0 phi=0 cos=0.0001 | 9.34% | 0.51% |
| aX=0.005 aY=1.0 phi=0 cos=0.0005 | 7.93% | 0.10% |
| aX=1.0 aY=0.005 phi=90 cos=0.0005 (swapped) | 7.94% | 0.11% |
| aX=0.002 aY=0.009 phi=0 cos=0.0005 (both low) | 3.96% | 0.36% |

Every other probed cell in the 17-row set reads under 0.1% post-fix.
`GGXHeightCorrelatedEnergyLUTTest`: 278 checks, 0 failures (was 261/0
pre-slice; 4 of the 17 new rows red at a 5% band pre-fix, tightened to
2% post-fix with 0 failures once both bugs above were fixed).

`GGXDiffuseTransmissionTest`'s `TestGrazingFurnaceDL86` aniso section
gained two rows at `theta=89.60` (the exact "low alpha AND grazing
cosTheta" corner the ledger row named), F0=1 spec-only:

| row | pre-fix | post-fix |
|---|---|---|
| Schlick aniso(.005,.5) az=0 | 0.97306+/-0.00110 | 0.99853+/-0.00107 |
| Schlick aniso(.5,.005) az=90 (swapped) | 0.97104+/-0.00110 | 0.99662+/-0.00107 |

Both rows red-proof at a `3*se + 0.015` two-sided band (pre-fix diff
from 1.0 is ~0.027-0.029, outside the ~0.018 band; post-fix diff is
~0.0015-0.0034, comfortably inside). `GGXDiffuseTransmissionTest`: 192
checks, 0 failures (was 190/0 pre-slice).

### Byte-identity of the pre-existing tables and code

Regenerating `tools/GenerateMicrofacetEnergyLUT.cpp` reproduces the
checked-in `MicrofacetEnergyLUT.h` with a **0-line diff**. Verified two
ways, since a naive line-diff over a 31000-line file with heavy internal
repetition (many identical rows at the isotropic diagonal) can hide a
real change inside diff-alignment noise: (1) MD5 of each of the 19
pre-existing table literals (`E_ss_TABLE`, `E_ss_TABLE_G2`,
`E_ss_TABLE_G2_ANISO_PHI`, `E_ss_TABLE_G2_ANISO_PHI_SUB`,
`E_ss_TABLE_G2_ANISO`, `E_ss_TABLE_G2_ANISO_SUB`, `E_avg_TABLE_G2_ANISO`
and all 12 DL-105 isotropic low-alpha tables), extracted independently
from the pre-fix and post-fix headers, all identical. (2) A line-diff
restricted to the non-table runtime-code region (from `AnisoSubNodeCos`
to end of file) contains ONLY insertion hunks (`NNNaNNN,NNN`) — zero
change/delete hunks — confirming every pre-existing line of code is
untouched, not merely textually similar. This also confirms the ledger
row's own "byte-identical at both axes >= 0.042" claim was imprecise
(that is the ISOTROPIC threshold): the correct, verified claim is
byte-identical whenever BOTH axes are `>= ANISO_ALPHA_LOW_A1 =
0.053043`.

### Render sanity

A scratch copy of `scenes/Tests/Materials/ggx_anisotropy_sweep.RISEscene`
with its `alphax`/`alphay = 0.05` cells lowered to `0.005` (`oidn_denoise
FALSE`, `pixel_filter box`, EXR `Rec709RGB_Linear`, 64spp,
`pathtracing_pel_rasterizer`, same PT seed via an isolated pre-/post-fix
header rebuild) moved by `+0.009%` to `-0.303%` per 3x3 grid region (no
NaN/Inf in either render). This is EXPECTED to be small, matching
DL-105's own render-sanity finding: `ggx_anisotropy_sweep.RISEscene` (and
this scratch variant) is a full-GI aluminum-conductor reflection scene
under environment/area lighting, not an isolated grazing-incidence
furnace, so the correction — which is largest at extreme grazing angles
along a specific azimuth — is heavily diluted by everything else the
pixel integrates. The LUT-level and furnace-level tables above are the
decisive evidence for this fix; the render confirms no corruption, not a
visible before/after difference at whole-image scale.

### Gate

`GGXHeightCorrelatedEnergyLUTTest` 278/0 (was 261/0), `GGXDiffuseTransmissionTest`
192/0 (was 190/0), `GGXSampleEvaluationConsistencyTest` 48/0,
`GGXWhiteFurnaceTest`/`GGXMetalRoughGridTest` pass, `LayeredWhiteFurnaceTest`
0/57 (1 pre-existing, unrelated `KNOWN-FAIL` row, unchanged),
`CookTorranceMultiscatterTest` 17/0, `FabricRenderTest` 60/0,
`CstDeriveGoldenTest` 452 MATCH/0 DRIFT, `SourceHygieneTest` 167/0; clean
warning-free rebuild, both incremental and full (`make -C build/make/rise
-j8 all`).

### Cost

Bake time for the two new families (`ALPHALOW_X`/`_SUB` fully baked at
150000 samples/cell, matching the existing DL-77 aniso sample count;
`ALPHALOW_XY`/`_SUB` baked upper-triangle-plus-mirror): measured ~3.5
additional minutes on top of the pre-existing isotropic+aniso bake
(~6m35s per the DL-105 entry), for a total single-threaded generator run
of ~10 minutes. Table footprint: `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X`
99,840 `Scalar`s, `_X_SUB` 37,440, `_XY` 41,600 (includes the mirrored
half), `_XY_SUB` 15,600, `E_avg_TABLE_G2_ANISO_ALPHALOW_X` 240,
`_XY` 100 — 194,820 new stored values total, roughly comparable to the
pre-existing `E_ss_TABLE_G2_ANISO_PHI`'s own 239,616.

### Residual, deliberately not closed by this fix

The H6 multiscatter-lobe sampler's own proposal shape (`E_ss_TABLE_G2_ANISO`/
`LookupEssG2Aniso`/`MSLobeZG2Aniso`/`SampleMSCosThetaG2Aniso`/`MSPdfG2Aniso`)
remains coarse below `ANISO_ALPHA_LOW_A1`, matching the DL-86 precedent
that this is an efficiency-only concern (never a bias, since `MSPdfG2Aniso`
and `SampleMSCosThetaG2Aniso` always describe the same distribution as
each other, whatever its shape). Extending it would need a low-alpha
twin of `BuildSegmentsFromRowN` sourcing the new `ALPHALOW_X`/`ALPHALOW_XY`
tables, plus re-deriving `MSLobeZG2Aniso`'s per-corner cached-affine
optimization for the low-alpha nodes (the isotropic DL-105 fix made the
identical deliberate trade-off for the same reason: "a documented
perf/complexity trade-off for a rare regime, correctness does not depend
on it"). Not opened as a new debt — it is a documented, intentional scope
boundary inherited from DL-86/DL-105, not a newly-discovered defect.

## DL-206: `ALPHA_LOW_STORED` off-by-one (dead slot, footprint)

Found reviewing the `debt-dl161` slice above. `ALPHA_LOW_STORED =
ALPHA_SUB_FINE + (ALPHA_MID_SIZE - 1)` = 10, but `AlphaLowSlot` only ever
addresses 9 slots: virtual mid-node indices run
`ALPHA_SUB_FINE+2..ALPHA_LOW_TOTAL-1` (with `ALPHA_SUB_FINE=7`,
`ALPHA_MID_SIZE=4` that is `idx={9,10}`), mapping to slots 7 and 8. Slot
9 is unreachable for every legitimate `idx` — `idx==ALPHA_LOW_TOTAL` (11)
is A1 itself, an EXISTING row of the main table, deliberately "not
re-baked" per this file's own DL-105 derivation, and never passed to
`AlphaLowSlot` at all. So the count baked into `ALPHA_LOW_STORED`
(`ALPHA_MID_SIZE-1` = 3 "mid nodes") was off by one against what the bake
loop actually visits (`ALPHA_MID_SIZE-2` = 2). DL-105 wasted one scalar
row per table (7 isotropic-ish `*_ALPHA_LOW_*` tables); DL-161 inherited
the unchanged constant across its four much larger multi-dimensional
low-alpha table families (`ALPHALOW_X`/`_SUB`, `ALPHALOW_XY`/`_SUB`, and
their `E_avg` twins), so ~24,639 stored zeros (~296 KB of header text,
~12.6% of the DL-161 additions above) were dead weight — baked, stored,
never read by any production code path.

### Fix

`ALPHA_LOW_STORED = ALPHA_SUB_FINE + (ALPHA_MID_SIZE - 2)` = 9, changed
in BOTH `MicrofacetEnergyLUT.h`'s own constant AND
`tools/GenerateMicrofacetEnergyLUT.cpp` (its local copy of the constant,
the `printf` block that emits the constant into the header, and the
`printf`'d doc-comment text describing the stored count as "3 geometric
nodes" — corrected to 2) — the DL-86 lesson repeated verbatim in DL-105's
own section above: a header-only fix the generator does not know about
is silently reverted by the next bake.

Added a compile-time guard so this specific mistake cannot recur
silently: `AlphaLowSlot` is now emitted `inline constexpr` (not just
`inline`) and immediately followed by

```cpp
static_assert( AlphaLowSlot( ALPHA_LOW_TOTAL - 1 ) == ALPHA_LOW_STORED - 1,
    "ALPHA_LOW_STORED must equal the highest slot AlphaLowSlot ever returns, plus one" );
```

Verified this actually fires, not merely compiles: reverted ONLY the
header's `ALPHA_LOW_STORED` formula back to `ALPHA_SUB_FINE +
(ALPHA_MID_SIZE - 1)` (leaving every baked array literal at its correct,
already-regenerated `[9]` size) and rebuilt — every consumer TU
(`CoatedBRDF.cpp`, `CookTorranceBRDF.cpp`, `CookTorranceSPF.cpp`,
`DielectricSPF.cpp`, `GGXBRDF.cpp`, `GGXSPF.cpp`) failed with `static
assertion failed due to requirement 'AlphaLowSlot(ALPHA_LOW_TOTAL - 1) ==
ALPHA_LOW_STORED - 1': ... note: expression evaluates to '8 == 9'`.
Restored the fixed constant and confirmed a clean rebuild.

### Regeneration

Ran the regenerate command from the header's own top comment (`c++ -O2
-Isrc/Library -std=c++11 -o tools/gen_lut
tools/GenerateMicrofacetEnergyLUT.cpp -lm` then `tools/gen_lut >
MicrofacetEnergyLUT.h`) in the foreground: **10m33s** (`627.50s user,
4.14s system, 99% cpu`) — comfortably under this row's own ~30-35 minute
estimate, because the "both axes low" bake loop (the DL-161 `ALPHALOW_XY`
family) now visits 45 `(ixRaw,iyRaw)` pairs (`9x9` upper triangular)
instead of 55 (`10x10`), an 18% reduction in that specific Monte-Carlo
loop's own cost.

Header size: **6,962,043 -> 6,656,394 bytes (-305,649 bytes, -298.5 KiB,
-4.4%)**, 31,333 -> 30,043 lines (-1,290).

### Verifying every reachable value

Wrote a small script (not committed) that parses each of the 13
`*_ALPHA_LOW_*`-family array literals out of both the pre-fix and
post-fix headers (a brace-counting tokenizer, not a line-based diff) and
compares `old[0..8]` (or `old[0..8][0..8]` for the two-axis tables)
against the corresponding post-fix elements:

- **10 of 13 tables are byte-for-byte identical** on every reachable
  slot: `E_ss_ALPHA_LOW_TABLE{,_G2}`, `E_ss_ALPHA_LOW_SUB_TABLE{,_G2}`,
  `E_ss_ALPHA_LOW_LIMIT_TABLE`, `E_avg_ALPHA_LOW_TABLE{,_G2}`,
  `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X{,_SUB}`,
  `E_avg_TABLE_G2_ANISO_ALPHALOW_X`. Their bake loops are keyed directly
  on the virtual node index (`for(idx=ALPHA_SUB_FINE+2; idx<ALPHA_LOW_TOTAL;
  idx++)`), a condition that never referenced `ALPHA_LOW_STORED` at all —
  removing the dead slot changes nothing else these loops do.
- **3 tables shift within Monte-Carlo noise**: `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY`,
  its `_SUB` twin, and `E_avg_TABLE_G2_ANISO_ALPHALOW_XY` — the "both axes
  low" family, whose own bake loop iterates `for(ixRaw=0; ixRaw<ALPHA_LOW_STORED;
  ixRaw++) for(iyRaw=ixRaw; iyRaw<ALPHA_LOW_STORED; iyRaw++)` DIRECTLY on
  the (now-corrected) constant. With the pre-fix `ALPHA_LOW_STORED=10`,
  this loop's LAST iteration at `ixRaw==9` (mapping to the invalid virtual
  `idx=11=ALPHA_LOW_TOTAL`, i.e. A1 itself — a value that should never
  have been re-baked, per the same rule the other 10 tables already
  respect) drew real VNDF Monte-Carlo samples from a SHARED, sequential
  RNG stream (`rand01_anisolow()`) before moving to the next `ixRaw`.
  Removing that extra, invalid iteration means every subsequent
  `(ixRaw,iyRaw)` pair's draws land at an earlier position in the same
  stream than before — a harmless but real shift, since each cell's own
  estimator is still an independently valid Monte-Carlo estimate of the
  same expectation, just fed by different (still uniformly-distributed)
  random numbers. Measured: max absolute diff over the reachable `9x9`
  reduced grid is **0.00238** on `E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY`
  (~0.27% relative at the affected cells — inside this table family's
  own documented `<=0.34%` residual band from the DL-161 section above),
  **0.00233** on its `_SUB` twin, and **4.86e-6** on
  `E_avg_TABLE_G2_ANISO_ALPHALOW_XY`. The DIAGONAL cells of all three
  (`ix==iy`, "iso-seeded" — copied deterministically from the isotropic
  tables with no RNG draw at all, per the DL-161 section's own
  Pass-1/Pass-2 construction) read **EXACTLY 0 diff**, which is the
  clean confirmation that the shift is precisely and only the RNG-stream-
  position effect described above, not a new bug.

### `GGXSpecularBihemisphericalGen.cpp` (DL-160) — checked, left unchanged

`tools/GGXSpecularBihemisphericalGen.cpp` reuses the identical
`ALPHA_SUB_FINE`/`ALPHA_MID_SIZE`/`ALPHA_LOW_TOTAL`/`ALPHA_LOW_STORED`
formulas and its own local `AlphaLowSlot`/`AlphaLowNode` (a standalone
translation unit by design, per its own file comment — it does not
`#include` `MicrofacetEnergyLUT.h`) to bake
`kGGXSpecularQuadWeightAlphaLow` (`GGXBRDF.cpp:782`, hand-pasted from
this generator's stdout). It carries the SAME `ALPHA_MID_SIZE-1`
off-by-one in shape, but its OWN comment already names the consequence
and handles it safely: "last row is an unused zero-padding slot
(MicrofacetEnergyLUT::AlphaLowSlot's own range only ever addresses 9 of
these 10 rows)" — a single explicit, labeled, all-zero row appended after
the real bake loop, not an accidental dead slot. Two things make this a
non-issue rather than a sibling instance of the same bug: (1) its own
footprint is ~8 `Scalar`s, four orders of magnitude smaller than DL-161's
~24,639 dead values: not worth a coordinated fix for its own sake. (2)
More importantly, `GGXBRDF.cpp`'s own consumer
(`LookupGGXSpecularQuadWeight`) indexes this array via
`MicrofacetEnergyLUT::AlphaLowSlot(idx)` — the REAL, now-fixed one — and
`AlphaLowSlot`'s return value has NEVER depended on `ALPHA_LOW_STORED`'s
value (its formula only references `ALPHA_SUB_FINE`), so this row's fix
to the MAIN header's `ALPHA_LOW_STORED` constant does not change what
`AlphaLowSlot` returns for any `idx`, and therefore cannot desynchronize
this table's own (unrelated, hand-maintained) size from what its
consumer actually indexes. Confirmed via `GGXHemisphericalAlbedoTest`
(DL-160's own gate) staying **40/0**, byte-for-byte unaffected. Left
as its own, separately-tracked, harmless design choice — not reopened,
not fixed here.

### Gate

Clean rebuild, zero warnings (full and incremental). `GGXHeightCorrelatedEnergyLUTTest`
278/0, `GGXDiffuseTransmissionTest` 192/0, `GGXSampleEvaluationConsistencyTest`
48/0, `GGXHemisphericalAlbedoTest` 40/0, `CookTorranceMultiscatterTest`
17/0, `LayeredWhiteFurnaceTest` 0/57, `SourceHygieneTest` 167/0,
`CstDeriveGoldenTest` 452 MATCH/0 DRIFT — every gated suite unchanged
from its pre-fix count, exactly as expected for a fix whose entire
purpose is removing dead, unread storage.

### Cost

Per-TU compile time (`GGXBRDF.cpp`, isolated pre-/post-fix header, 5
runs each via a direct `c++` invocation with the project's real
`Config.OSX` flags): pre-fix median **0.90s**, post-fix median **0.94s**.
A ~4.4% smaller header did not translate into a measurable compile-time
win at this file's size — front-end parse/constant-folding cost for a
~300 KB table literal is apparently not the dominant term, and the added
`constexpr`/`static_assert` costs a small, roughly offsetting amount.
Report this honestly as "no measurable win", not as a speedup — the
footprint reduction (disk/repo size, and one fewer dead value for every
future low-alpha table family to inherit) is the real benefit here, not
compile time.

## DL-139: anisotropic bihemispherical single-scatter quadrature

**CLOSED 2026-09-21.** DL-123's single-scatter quadrature collapsed
`alphaX != alphaY` to `sqrt(alphaX*alphaY)`. On current HEAD, an
independent black-box estimator of the real `GGXBRDF::value()` measured
relative errors from **7.84% to 24.01%** over six ordinary off-diagonal
rows, and **21.24%** at the one-axis-low `(0.005, 0.5)` corner. The
historical 11.9%/0.47353 figure was not reused as an expectation: the
current mixture oracle measures `(0.05,0.5)` at 0.46221 and the old
guide reports 0.52971.

The fix bakes the eight Fresnel-kernel moments on the same reachable
24x24 independent-roughness grid and DL-161 low-axis construction used
by the current anisotropic energy LUT. This deliberately supersedes the
row's stale "ratio axis" recipe: DL-77's current source documents why a
ratio/`alphaEff` rectangle contains mostly unreachable high-ratio cells.
The bihemispherical quantity needs no phi axis. It integrates incident
and outgoing azimuth over complete hemispheres, so rotating the material
tangent frame is only a change of integration variables; the result
depends on the unordered pair `(alphaX,alphaY)`. Axis-exchange symmetry
halves the bake work.

The off-diagonal `alphaX -> 0` boundary is not the isotropic perfect
mirror boundary: the orthogonal finite roughness remains in Smith
Lambda and the VNDF. It therefore has no applicable closed-form
isotropic anchor and is baked at the low nodes. The virtual zero node
folds to low node 1, exactly as DL-161 does; production floors roughness
at `1e-4`, between low nodes 1 and 2, so the virtual zero is unreachable.
On the isotropic diagonal the lookup forwards to the original
`LookupGGXSpecularQuadWeight` path before any new arithmetic. The initial
DL-139 representation stored full weights on a rectangular grid and used
bilinear interpolation. That was not a valid diagonal boundary construction:
inside a diagonal cell, bilinear interpolation mixes both off-diagonal
corners into its diagonal trace, which generally differs from the independent
legacy one-dimensional interpolant. The low/low generator also copied two
isotropic midpoint rows by storage index even though the isotropic and
anisotropic grids declare different physical roughness coordinates there.

The corrected representation stores the anisotropic correction

`C(alphaX,alphaY) = W_aniso(alphaX,alphaY)
                     - W_iso(sqrt(alphaX*alphaY))`.

Its mathematical boundary condition is `C(a,a)=0`. Every diagonal anchor is
therefore a literal zero correction at its own declared coordinate. Cells
touching the diagonal are split into triangles along that zero edge; cells
strictly away from it remain bilinear. Runtime adds the complete preserved
`W_iso(sqrt(alphaX*alphaY))` curve after interpolating `C`. Consequently the
unequal-axis limit is the legacy isotropic curve at every coordinate and at
every legacy knot, while off-diagonal bake nodes reconstruct their physical
anisotropic weights. Low/low, low/ordinary, and ordinary/ordinary pieces
share their endpoint values, so knot transitions are continuous. This uses
no epsilon band, near-equal-axis switch, or tolerance-based isotropization.
The albedo's `Eavg`/multiscatter term uses the same constrained construction
locally. The older shared energy-LUT API retains a `1e-9` forwarding band;
calling it directly would only move the public albedo jump to that band's
edge. The local wrapper changes no GGX sampler/evaluator contract outside
`hemisphericalAlbedo{,NM}`.
A slot-by-slot comparison found all **344** pre-existing node/ordinary/
low-axis float literals identical between base HEAD, regenerated output, and
the corrected source.

`TestAnisotropicFallback` now uses its own anisotropic D/Lambda/G1/VNDF
implementation and an independently seeded, randomized low-discrepancy
50/50 VNDF-plus-cosine proposal. The cosine half is required because the
real BRDF numerator includes a broad Kulla-Conty multiscatter term; a
VNDF-only proposal has a heavy tail at the one-axis-smooth boundary.
Each RGB and NM row uses `n=4,800,000` samples and prints mean, sample SD,
and standard error. The initial closure measured **0.0094%–0.0263%** over
eight strongly anisotropic cells; those figures and its **62/16 -> 62/0**
red/green are historical for source `98c024f2`. The correction regression
adds low and ordinary near-diagonal physical cells at Schlick F0=0, exact
one-ULP approaches from both sides, finite two-sided approaches, axis swaps,
RGB/NM, and Schlick/conductor/thin-film public paths. On the corrected
representation the 12 physical cells read **0.0111%–0.0807%** relative error,
inside the unchanged 0.5% accuracy gate, and the complete suite is **186/0**.
The same final regression rebuilt against the committed old relevant source
is **186/84** red. This correction count is distinct from, and does not
relabel, the initial closure's historical 62/16 run.

The anisotropic bake uses its own fixed RNG stream and 100,000 samples
per `(alphaX,alphaY,mu_i)` cell. For the initial full-weight representation
at `98c024f2`, in a granted quiet slot on this checkout,
three foreground bakes took 66.46, 66.23, and 66.03 seconds (mean 66.24 s,
sample SD 0.215 s); all three emitted byte-identical tables. Interleaved
single-file compiles of the old and fixed `GGXBRDF.cpp` measured
0.9567 +/- 0.0115 s and 0.9633 +/- 0.0153 s respectively (n=3, sample
SD), so the observed 0.0067 s difference is below run-to-run noise.
The object grew by 52,832 bytes (1.65%) and the fully linked CLI by
49,744 bytes (0.193%). Those timings and sizes are historical and are not
claimed for the corrected generator/source representation.

**Corrected-source cost refresh (2026-09-21, quiet slot).** The exact
corrected candidate was `3c07250e`; its source blob was `6ebc17ca`. The
committed control was `290d4188`: the complete candidate tree with only
`src/Library/Materials/GGXBRDF.cpp` replaced by the exact `b8be6e7` blob
`bdc03583`. It is therefore a source-only compile/link control, not a
whole-baseline comparison. The library, CLI, and exact
`GGXHemisphericalAlbedoTest` target were rebuilt and relinked for each linked
state outside the compile intervals.

Single-source compiles used the project's actual make target and flags in the
interleaved order old,new/new,old/old,new. The control samples were
`0.948362`, `0.951115`, `0.950048` seconds (mean **0.949842 s**, sample SD
**0.001388 s**); corrected samples were `0.963508`, `0.975672`, `0.989390`
seconds (mean **0.976190 s**, sample SD **0.012949 s**). The observed
`+0.026349 s` / `+2.77%` is a compile-cost measurement only; with three
samples and the corrected side's larger spread it is not a runtime or render
claim.

The corrected `GGXBRDF.cpp` is **178,012 bytes** versus **51,739 bytes** in
the source-only control (`+126,273`). Its object is **3,257,808 bytes** versus
**3,200,736** (`+57,072`, `+1.78%`), and the source-only-linked CLI is
**25,892,840 bytes** versus **25,826,424** (`+66,416`, `+0.257%`). The
generator source is **35,809 bytes** versus **22,430** at base `b8be6e7`
(`+13,379`); its one functional `-O3 -std=c++17` compile took 0.398776 s.
Three untrimmed functional bakes took `66.248197`, `66.081205`, and
`65.851441` seconds (mean **66.060281 s**, sample SD **0.199204 s**). An
initializer parser, rather than a hard-coded success flag, found all **344**
legacy literals and all **6,984** correction literals identical in each bake
and the corrected source; the three complete outputs were byte-identical.
Since `Scalar` is `double`, the correction literals' direct scalar payload is
**55,872 bytes** (58,624 bytes including the 344 preserved legacy literals).
All 20 measured build/compile/link/bake return codes were zero and all 20
warning counts were zero. No test executable or renderer was run; these are
compile, generator, and storage costs only, with no runtime/render claim.
