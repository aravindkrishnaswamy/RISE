# DL-62 / DL-64: GGX glossy-filter roughness and zero-F0 selection weight

Status: **CLOSED 2026-09-13** — source repair `dfdd5ee1`, red proof
`a1db468d`. Independent residual DL-63 (specular-only furnace gain, a
separate G1-vs-G2 masking-model mismatch) is unaffected and stays open.
New debt DL-65 (CookTorrance/Schlick glossy-filter sibling) opened, not
fixed, out of this slice's GGX scope.

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

Fix: derive the selection weight from `GGXInterfaceFresnel::Mean()` /
`MeanNM(nm)` — the lobe's actual hemispherical Fresnel-weighted albedo —
instead of raw F0. This is nonzero even at F0=0 (`SchlickFresnelAvg(0) =
1/21 ≈ 0.048`), so the specular/MS lobes remain reachable; per-direction
Fresnel evaluation for whichever lobe IS sampled (the true throughput)
still reads the exact painter F0/tint independently, so nonzero-F0 and
conductor/thin-film behavior is unchanged. No magic F0 floor was added —
the fix reuses the same closed-form hemispherical average already used
elsewhere in `GGXBRDF`/`GGXSPF` for the multiscatter tail and the DL-37
diffuse-interface model.

`GGXSPF::ScatterNM` reuses the guarded per-wavelength F0 sample for two
purposes pre-fix (the selection weight AND the per-direction Fresnel
input); these had to be split into two variables (`ws` = the new
hemispherical weight, `wsF0` = the original raw F0) so the per-direction
Fresnel math (Schlick, thin-film, conductor branches) stays byte-for-byte
unchanged. `GGXSPF::Scatter` (RGB) and `Pdf`/`PdfNM` had no such reuse and
needed only the weight's source swapped.

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

**RGB/NM twins, other Fresnel modes**: audited within GGX itself —
`eFresnelConductor` and `eFresnelThinFilmConductor` selection weights now
go through the same `Mean()`/`MeanNM()` call as `eFresnelSchlickF0`
(the struct dispatches by `mode` internally), so the fix applies
uniformly across all three GGX Fresnel modes, not only Schlick. Verified
by the conductor/thin-film RGB and NM probes in the red-proof test and by
`ThinFilmBRDFTest`/`ThinFilmFurnaceTest`/`ThinFilmProductionTest`/
`GGXConductorAmbientIORTest` staying green.

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

## Gates

`GGXSampleEvaluationConsistencyTest` (new, 29/0), `GGXWhiteFurnaceTest`,
`GGXFresnelModeTest`, `GGXConductorAmbientIORTest`, `GGXMetalRoughGridTest`,
`GGXFilmTransmissionRangeTest`, `ThinFilmFurnaceTest`,
`ThinFilmProductionTest`, `ThinFilmBRDFTest` (25/0, post Test-B repair),
`CoatedMaterialChunkTest`, `FabricMaterialChunkTest`,
`WeaveMaterialChunkTest`, `FabricRenderTest`, `CookTorranceMultiscatterTest`,
`SPFPdfConsistencyTest`, `SPFBSDFConsistencyTest` — all green.
`LayeredWhiteFurnaceTest`: `0 of 57 configurations failed` (unchanged).
`GGXDiffuseTransmissionTest`: `150 checks, 3 failures`, bit-for-bit
identical to a stashed pre-fix rebuild (the pre-existing DL-63 F0=1
specular-only failures; see DL-63's ledger row for the exact numbers).
Clean `make -C build/make/rise -j8 all`: zero compiler warnings.
