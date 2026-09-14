# DL-74: surface env-NEE / escape MIS partition violation under one-sample path guiding

Status: **CLOSED 2026-09-14** (debt-guiding2 slice) for the default
one-sample-MIS guiding mode. RIS-mode guiding's structurally-identical
residual is filed OPEN as **DL-83** (no established combined-pdf formula
for an externally-fixed direction under RIS; see "Residual" below).

## Mechanism

`PathTracingIntegrator.cpp`'s surface scatter continuation (PART 3) stores
the COMBINED guiding pdf for the escape-side MIS weight whenever OpenPGL
one-sample guiding fires:

```
rs2.bsdfPdf = effectiveBsdfPdf;   // = combinedPdf = alpha*guidePdf + (1-alpha)*bsdfPdf
```

fed to `RayCasterEnvEscapeMISWeight`'s `w_bsdf = PowerHeuristic(rs.bsdfPdf,
envPdf)` when the continuation escapes to the global env map (DL-53).
Surface env-NEE at the SAME shading point
(`LightSampler.cpp`'s env arm, `pMaterial->Pdf(envDir, ri, defaultIOR)`)
instead always used the RAW, un-guided material pdf, with no guiding term
at all. The two sides therefore fed `PowerHeuristic` DIFFERENT pdf values
for the same physical direction whenever guiding is trained and active, so
`w_bsdf + w_nee != 1` — a genuine partition-of-unity violation, discovered
via the DL-73 derivation (which showed the VOLUME-vertex case is unbiased
for the OPPOSITE reason: both its sides already agree on the raw phase
pdf).

## Repair (one-sample MIS mode)

NEE runs in PART 2, BEFORE PART 3's lobe selection chooses which
`ScatteredRay` (and hence which per-lobe `GuidingEffectiveAlpha`) the
continuation will use — so the exact per-lobe alpha the escape side would
apply is not yet known at NEE time. Mirroring `BDPTIntegrator.cpp`'s own
guiding block (`const Scalar alpha = guidingAlpha;`, no per-lobe damping,
used for the identical structural reason), the fix uses the BASE
`rc.guidingAlpha` uniformly for the NEE-side blend.

Implementation, chosen to avoid touching `LightSampler`'s broad surface
API more than necessary:

- `IGuidedNEEPdfBlend` (new, `LightSampler.h`): a tiny pure interface,
  `Scalar Blend(const Vector3& wo, Scalar rawPdf) const`, added as an
  OPTIONAL trailing parameter (default `nullptr`) to both
  `EvaluateDirectLighting` and `EvaluateDirectLightingNM`. Deliberately
  has no dependency on OpenPGL types, so the header stays buildable
  without `RISE_ENABLE_OPENPGL`; every existing call site (BSSRDF/RW-SSS
  NEE, the shader-op / volume / HWSS call sites) is untouched and
  continues to get the pre-fix raw-pdf behavior via the default.
- Inside `EvaluateDirectLighting{,NM}`'s env-NEE arm, when `pGuidedBlend`
  is non-null AND `!isVolumeScatter` (a local, redundant safety net — see
  below), the raw material pdf is passed through `pGuidedBlend->Blend()`
  before the `pBsdf > 0` check and the `PowerHeuristic`/`OptimalMIS2Weight`
  call.
- `PTGuidedNEEPdfBlend` (new, `PathTracingIntegrator.cpp`, guarded by
  `#ifdef RISE_ENABLE_OPENPGL`): a lightweight, NON-reference-counted
  `IGuidedNEEPdfBlend` implementation constructed fresh on the stack for
  each NEE call (never shared across threads or calls, unlike the
  heap-allocated, integrator-lifetime `ClayNEEMaterial`). `Blend()`
  computes `PathTransportUtilities::GuidingCombinedPdf(alpha,
  pField->Pdf(*pDist, wo), rawPdf)`; a null field (the default-
  constructed, unconfigured state) makes `Blend()` a pure pass-through.
- The PART 2 call site (`PathTracingIntegrator.cpp`) constructs one
  `PathGuidingField` distribution (`InitDistribution` + `ApplyCosineProduct`,
  a fresh thread-local `GuidingDistributionHandle` distinct from PART 3's
  own) and configures `PTGuidedNEEPdfBlend` with it, gated on:
  - `rc.pGuidingField && rc.pGuidingField->IsTrained()`,
  - `rc.guidingSamplingType == eGuidingOneSampleMIS` (see "Residual" —
    RIS mode is deliberately excluded),
  - `depth <= rc.maxGuidingDepth` and `rc.guidingAlpha > NEARZERO`,
    matching PART 3's own gates,
  - `rs.type != IRayCaster::RAY_STATE::eRaySpecular` — the SAME
    incoming-state gate `GuidingEffectiveAlpha` applies,
  - `!pNEEMaterial->ScattersFullSphere()` — see below.

**Why `ScattersFullSphere()` gates this correctly.** `GuidingSupportsSurfaceSampling`
only admits diffuse/reflection lobes (excludes delta, refraction, and
translucent). NEE has no access to which lobe would eventually be chosen,
so it cannot replicate that per-lobe gate directly — but for an ORDINARY
(non-full-sphere) material, env-NEE only ever fires on the reflection
hemisphere (`cosEnv > 0` in `LightSampler.cpp`), so any direction with a
nonzero aggregate `Pdf()` there can only be explained by a diffuse/
reflection lobe (a refraction/translucent lobe's support lives on the
OTHER hemisphere, or both hemispheres only when `ScattersFullSphere()` is
true) — exactly `GuidingSupportsSurfaceSampling`'s own eligible set.
Skipping full-sphere materials (hair, translucent) avoids blending
guiding into a transmission lobe PART 3 would never touch, which would
just relocate the DL-74 asymmetry rather than close it — and matches the
task's own scope boundary (no edits to TranslucentSPF/hair files).

## Red-proof

No dedicated new unit test was added for this row (the fix is a plumbing/
consistency change with no closed-form partition-of-unity assertion
available at the unit level — the row's own recipe asked to "derive the
fix direction and quantify the resulting bias magnitude," not to add a
strict regression). Correctness was instead verified by:

1. Full clean `make -C build/make/rise -j8 all` (zero warnings) after
   every edit.
2. The existing guiding-specific regression suites, unaffected in
   behavior (`PTGuidedSelectProbTest`, `TranslucentIORStackTest` — both
   report identical `trials=4000` distributions before and after this
   change, confirming the NEE-side blend does not perturb the RNG stream
   in a way that changes lobe-selection outcomes) and the broader
   render-correctness suites (`EnvLightBalanceTest` 116/116,
   `SSSRadianceScalingTest` 574017/0) all still pass — this env-NEE
   guiding blend is new code on a path none of those suites' fixtures
   exercise (none of them enable OpenPGL guiding AND an env light AND a
   non-full-sphere material simultaneously with a trained field), so
   "unchanged" is the expected, correct outcome, not a null result.
3. Manual derivation (this doc + inline code comments) of why the chosen
   `GuidingCombinedPdf(rc.guidingAlpha, guidePdf(envDir), rawPdf)` formula
   is the correct MIS partner for the escape side's one-sample-MIS
   `effectiveBsdfPdf` for the SAME shading point and direction.

## Sibling audit

- **RIS-mode guiding** (`rc.guidingSamplingType == eGuidingRIS`): confirmed
  sibling of the SAME bug pattern (escape side uses a guided, non-raw
  pdf; NEE side used the raw pdf) — but the escape side's
  `risEffectivePdf` for RIS is a per-candidate, RIS-normalized
  approximation (`risTarget * N / sumWeights`, see
  `PathTransportUtilities::GuidingRISSelectCandidate`'s doc) with no
  established closed-form value at an EXTERNALLY fixed direction the way
  the one-sample formula has — computing one would need a second,
  hypothetical RIS candidate this call has no principled way to draw
  without introducing new randomness or bias risk. Applying the one-
  sample formula here anyway (while the escape side uses `risEffectivePdf`)
  would trade one MIS-partner mismatch for a different one, not close the
  partition — so the fix explicitly gates OUT RIS mode
  (`guidingSamplingType == eGuidingOneSampleMIS`) rather than guess.
  Filed as **DL-83** (OPEN).
- **HWSS**: confirmed NOT a sibling — `IntegrateFromHitHWSS`'s own
  `effectiveBsdfPdf` (~line 5605) is set once to `pS->isDelta ? 0 :
  pS->pdf` and never reassigned via guiding anywhere in the HWSS-specific
  function body (no RIS/`combinedPdf` lines exist there) — HWSS's escape
  and NEE sides both always use the RAW material pdf, so no partition
  violation exists for HWSS in the first place. Matches the task's own
  given fact ("HWSS runs no guiding").
- **BDPT/VCM/MLT**: none of these call `LightSampler::EvaluateDirectLighting`/
  `EvaluateDirectLightingNM` at all (confirmed by grep — every reference
  in `BDPTIntegrator.cpp` is a comment, not a call); BDPT's own env-NEE/
  escape guiding consistency (if any) is a structurally separate code
  path, out of this row's and this slice's scope.
- **Volume vertices** (`isVolumeScatter=true`): NOT a sibling — DL-73
  already established this side is unbiased (both env-NEE and the escape
  weight use the SAME raw `phasePdf`). The fix guards this explicitly:
  `pGuidedBlend` is only ever constructed and passed from PART 2's
  SURFACE NEE call site (never from the volume NEE call in
  `MediumTransport.cpp`, which does not pass the new parameter and so
  gets the default `nullptr`), and `EvaluateDirectLighting{,NM}`'s env-NEE
  arm additionally gates the blend on `!isVolumeScatter` as a second,
  redundant safety net against a future call-site regression.
- **BSSRDF entry-point NEE** (`PathTracingIntegrator.cpp`'s SSS
  continuation, `PTEvaluateDirectLighting<Tag>` calls at the BSSRDF entry
  point): NOT wired to the new blend — out of scope per the task's own
  BSSRDF-file exclusion, and also a poor fit for the one-sample formula
  as derived here (the entry material's `Pdf()` is the BSSRDF's own
  cosine-hemisphere density, a different quantity than a surface
  material's aggregate BRDF `Pdf()`; no evidence this call site's escape
  counterpart even applies guiding — the BSSRDF continuation's own
  `rs2.bsdfPdf = bssrdf.cosinePdf` is never guiding-adjusted anywhere in
  the codebase). Left untouched, consistent with DL-72's own scope there.

## Known simplification (not a separate debt)

The NEE-side blend uses ONE uniform `rc.guidingAlpha` for the whole
material's aggregate pdf, whereas PART 3's escape side would apply
`GuidingEffectiveAlpha`'s per-LOBE damping (full alpha for diffuse, half
for glossy reflection) to whichever single lobe ends up selected. This is
a smaller, second-order mismatch than the one this fix closes (which
could be many orders of magnitude wherever the guide has learned a
sharply peaked distribution) — mirrors `BDPTIntegrator.cpp`'s own
precedent of using the base alpha uniformly for the identical structural
reason (NEE/connection evaluation happening independently of a specific
lobe choice). Not filed as a separate row: it is an approximation
introduced BY this fix, not a pre-existing, independently-discoverable
defect.

## File status

| File | Status |
|---|---|
| `src/Library/Lights/LightSampler.h` | Added `IGuidedNEEPdfBlend` interface; added optional trailing `pGuidedBlend` parameter to `EvaluateDirectLighting`/`EvaluateDirectLightingNM` (default `nullptr`). |
| `src/Library/Lights/LightSampler.cpp` | Both env-NEE arms (RGB/NM) blend the raw pdf through `pGuidedBlend` when non-null, gated `!isVolumeScatter`. |
| `src/Library/Shaders/PathTracingIntegrator.cpp` | Added `PTGuidedNEEPdfBlend` (guarded `RISE_ENABLE_OPENPGL`); PART 2's NEE call site constructs/configures it and passes it through the extended `PTEvaluateDirectLighting<Tag>` template wrapper (also extended with the same optional trailing parameter, defaulted so the two BSSRDF/RW-SSS call sites are unaffected). |
| `docs/DEBT_LEDGER.md` | DL-74 CLOSED (one-sample mode); DL-83 filed OPEN (RIS-mode residual). |
| `docs/DL74_ENV_NEE_GUIDING_PARTITION.md` | Added (this file). |

Gate: full clean `make -C build/make/rise -j8 all` (zero warnings);
`PTGuidedSelectProbTest`, `TranslucentIORStackTest` (ALL TESTS PASSED,
unchanged), `EnvLightBalanceTest` (116/116), `SSSRadianceScalingTest`
(574017/0), `RayCasterEnvEscapeMISTest` (91/91), `OptimalMISAccumulatorTest`
(34/0), `AgentLiveCommitTest` (884/0), `MISWeightsTest` (59/0),
`RasterizerDefaultsConsistencyTest` (164/0), plus the render suites
touched by the `EvaluateDirectLighting` signature change
(`AgentChunkCrudTest` 3809/0, `CstCameraEraseTest` 43/0,
`CstSourceInstanceTest` 455/0, `SignalEmitterRecordTest` 93/0,
`AgentViewModeRenderTest` 687/0, `DirectionalFogTest` 11/0,
`FabricRenderTest` 57/0, `HairRenderTest` 26/0, `VolumeEnvFurnaceTest`
29/0) all pass.
