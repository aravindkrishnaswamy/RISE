# DL-62 / DL-64: GGX glossy-filter roughness and zero-F0 selection weight

Status: **CLOSED 2026-09-13** — source repair `dfdd5ee1`, red proof
`a1db468d`. Independent residual DL-63 (specular-only furnace gain, a
separate G1-vs-G2 masking-model mismatch) is unaffected and stays open.
New debt DL-65 (CookTorrance/Schlick glossy-filter sibling) opened, not
fixed, out of this slice's GGX scope.

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
