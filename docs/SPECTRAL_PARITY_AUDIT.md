# Spectral-vs-Pel Feature Parity Audit & Remediation Plan

**Date:** 2026-05-07
**Scope:** Every ✗, partial, and "(limited)" cell in the optional-feature support matrix in [docs/RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4, decoded against the parser ([src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) `CreateAllChunkParsers()`) and the rasterizer / integrator source.
**Out of audit scope:** algorithmic changes, anything that requires open research, anything in MLT (deliberately empty per [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md)).

**Status tracker:** as remediation lands, items are flipped from `OPEN` → `DONE` in §6 with a one-line note. The §1 matrix and §2 inventory always reflect post-remediation state.

---

## TL;DR

Of the 13 ✗ / partial / "(limited)" cells in the matrix:

- **3 are quick wins** (1–5 days each): BDPT-spectral path guiding subset, BDPT-spectral adaptive sampling, BDPT/VCM-spectral OIDN albedo proxy.
- **2 are refactor-blocked** (need Phase 2b/2c to land): PT-spectral inline AOV, BDPT-spectral inline AOV.
- **1 is architectural** (2–3 weeks, design + implementation): per-wavelength photon stores for VCM-spectral merging.
- **2 are deliberately out of scope** (MLT × 2): chain-based mutation has no useful interaction with the optional features.
- **2 are matrix errors** (no work — fix the doc): pixelintegratingspectral_rasterizer's ✓ for adaptive sampling and optimal MIS are wrong; BDPT's ✓ for SMS is post-2026-05-excision stale.
- **3 are architecture- or research-blocked** (no path forward without a separate design): VCM-anything path guiding, BDPT/VCM optimal MIS, pixelintegratingspectral path guiding.

The single **correctness** issue (vs feature gap) is **VCM-spectral merging via the `RISEPelToNMProxy` luminance projection** — see §3.

---

## 1. Matrix decoded against source

The [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4 matrix has been corrected against the parser and rasterizer source as part of this audit (Step 1, see §6.1). The current matrix below is bit-for-bit what's now in [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4.

| Chunk | Path guiding | Adaptive sampling | SMS | Optimal MIS | OIDN denoise |
|---|---|---|---|---|---|
| `pixelpel_rasterizer` | ✓ | ✓ | (via shader-op) | ✓ | ✓ |
| `pixelintegratingspectral_rasterizer` ¹ | ✗ | ✗ | (via shader-op) | ✗ | (limited) |
| `pathtracing_pel_rasterizer` | ✓ | ✓ | ✓ | ✓ | ✓ (full filtered-film bypass) |
| `pathtracing_spectral_rasterizer` | ✗ | ✓ | ✓ | ✗ | (limited) |
| `bdpt_pel_rasterizer` | ✓ | ✓ | ✗ | ✗ | ✓ |
| `bdpt_spectral_rasterizer` | ✓ | ✗ | ✗ | ✗ | (limited) |
| `vcm_pel_rasterizer` | ✗ | ✓ | ✗ | ✗ | ✓ |
| `vcm_spectral_rasterizer` ² | ✗ | ✓ | ✗ | ✗ | (limited) |
| `mlt_rasterizer` | ✗ | ✗ | ✗ | ✗ | ✗ (default off) |
| `mlt_spectral_rasterizer` | ✗ | ✗ | ✗ | ✗ | ✗ (default off) |

¹ `pixelintegratingspectral_rasterizer` is soft-deprecated — see §2.1–§2.5.
² VCM-spectral merge correctness gap — see §3.

**Pre-correction errors fixed in Step 1** (kept here for change-log audit trail):
- pixIntSpec adaptive sampling: ✓ → ✗ (parser does not call `AddAdaptiveSamplingParams`).
- pixIntSpec optimal MIS: ✓ → ✗ (parser does not call `AddOptimalMISParams`).
- PT-spectral optimal MIS: ✓ → ✗ (`PixelBasedSpectralIntegratingRasterizer` does not allocate the accumulator; integrator's NM/HWSS paths read `rc.pOptimalMIS` only when a parent has set it, which spectral parents never do).  Step 3 went further and removed `AddOptimalMISParams` from the PT-spectral descriptor entirely; the parser now hard-fails on `optimal_mis*` lines.
- PT-spectral path guiding: ✓ → ✗ (caught in adversarial review 2026-05-07; parser does not call `AddPathGuidingParams`, `IJob::SetPathTracingSpectralRasterizer` takes no `PathGuidingConfig`, and the rasterizer source has no guiding-field plumbing.  The matrix's prior ✓ was a transcription error from the original RENDERING_INTEGRATORS.md that this audit propagated.  PT integrator's NM/HWSS branches reference `rc.pGuidingField`, but the spectral parent never allocates one — same shape as the optimal-MIS gap above.)
- BDPT-pel SMS: ✓ → ✗ (excised 2026-05-07).
- BDPT-spectral SMS: ✓ → ✗ (excised 2026-05-07).
- BDPT-pel optimal MIS: ✓ → ✗ (params parsed into `StabilityConfig` but `BDPTIntegrator.cpp` does not read `rc.pOptimalMIS` anywhere — `grep OptimalMIS src/Library/Shaders/BDPTIntegrator.cpp` returns 0 matches).  Step 3 (audit §6.1 row 2) further removed `AddOptimalMISParams` from BDPT-pel; parser now hard-fails on `optimal_mis*` lines authored against the chunk.
- BDPT-spectral optimal MIS: ✓ → ✗ (same; Step 3 also removed the helper from BDPT-spectral).
- Stale bullet "SMS is wired into PT and MLT" → "SMS is wired into PT only" (MLT has no SMS plumbing at all — `grep SMSConfig src/Library/Rendering/MLT*.cpp` returns 0 matches).

---

## 2. Per-gap inventory

For each ✗ / partial / (limited) cell, this section names the feature, locates the wired Pel side, diagnoses the missing-spectral-side reason, estimates effort, and flags refactor dependencies.

### 2.1 `pixelintegratingspectral_rasterizer` — Path guiding ✗

- **Pel side wired at:** `pixelpel_rasterizer` parser block ([AsciiSceneParser.cpp:6135](../src/Library/Parsers/AsciiSceneParser.cpp#L6135) `AddPathGuidingParams`) plus `PixelBasedPelRasterizer::PreRenderSetup` allocates the OpenPGL field.
- **Spectral side:** parser does not call `AddPathGuidingParams` ([AsciiSceneParser.cpp:6276-6286](../src/Library/Parsers/AsciiSceneParser.cpp#L6276)). `PixelBasedSpectralIntegratingRasterizer` has no `pGuidingField` member.
- **Diagnosis:** **architectural — out of scope.** The legacy spectral rasterizer is shader-dispatch (runs an `IShaderOp` chain at every hit). OpenPGL guiding is per-pixel directional density estimation that requires the integrator to drive per-vertex training samples; the shader-op chain has no such hook. Adding it would mean rewiring the chain dispatch through a new "guiding-aware" path-tracing shader-op variant — a substantial refactor for a legacy code path users are encouraged to migrate off of.
- **Effort:** out of scope. Recommend documenting in the matrix as ✗ with a "shader-dispatch architecture" footnote. Users who need spectral path guiding should use `pathtracing_spectral_rasterizer`.
- **Refactor dependency:** none (we're not building this).

### 2.2 `pixelintegratingspectral_rasterizer` — Adaptive sampling ✗ (matrix corrected)

- **Pel side wired at:** `pixelpel_rasterizer` parser ([AsciiSceneParser.cpp:6136](../src/Library/Parsers/AsciiSceneParser.cpp#L6136) `AddAdaptiveSamplingParams`) + `PixelBasedPelRasterizer::IntegratePixel` Welford loop.
- **Spectral side:** parser does NOT add adaptive params, Finalize() does not parse them. `PixelBasedSpectralIntegratingRasterizer` has no `adaptiveConfig` member.
- **Diagnosis:** legacy chunk is soft-deprecated; not building this feature here. Modern alternative (`pathtracing_spectral_rasterizer`) has full adaptive sampling.
- **Effort:** out of scope (soft deprecation). Implementing the feature = ~2 days if priorities change.
- **Refactor dependency:** none.

### 2.3 `pixelintegratingspectral_rasterizer` — SMS (via shader-op)

- **Diagnosis:** correct as documented. SMS in the shader-dispatch pipeline lives in `SMSShaderOp` and `ManifoldSolverShaderOp`, attached to the `defaultshader` chain. No remediation needed.

### 2.4 `pixelintegratingspectral_rasterizer` — Optimal MIS ✗ (matrix corrected)

- **Pel side wired at:** `pixelpel_rasterizer` parser ([AsciiSceneParser.cpp:6139](../src/Library/Parsers/AsciiSceneParser.cpp#L6139) `AddOptimalMISParams`) + `PixelBasedPelRasterizer::PreRenderSetup` ([PixelBasedPelRasterizer.cpp:396-472](../src/Library/Rendering/PixelBasedPelRasterizer.cpp#L396)) trains and exposes via `rc.pOptimalMIS`.
- **Spectral side:** parser does NOT call `AddOptimalMISParams`. Spectral integrating rasterizer base never sets `rc.pOptimalMIS`.
- **Diagnosis:** the Kondapaneni 2019 optimal-MIS implementation in RISE is RGB-internal; extending it to spectral would require either training one alpha map per wavelength bucket (memory-prohibitive) or projecting spectral contributions to luminance for the training samples (defeats the spectral accuracy that the chunk exists for).
- **Effort:** out of scope (soft deprecation). True remediation = research + ~1 week.
- **Refactor dependency:** none.

### 2.5 `pixelintegratingspectral_rasterizer` — OIDN "(limited)"

- **Pel side:** `pixelpel_rasterizer` inherits the full denoise pipeline through `PixelBasedRasterizerHelper`, including the FilteredFilm bypass on denoise ([PixelBasedRasterizerHelper.cpp:757-816](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp#L757)) and the inline accurate-prefilter AOV path through the PT shader-op.
- **Spectral side:** denoise itself runs (same `PixelBasedRasterizerHelper` machinery), but:
  - **No inline AOV.** Search of [PixelBasedSpectralIntegratingRasterizer.cpp](../src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.cpp) for `aov`/`albedo`/`PixelAOV` returns 0 matches. The rasterizer relies exclusively on the post-render `OIDNDenoiser::CollectFirstHitAOVs` retrace fallback ([PixelBasedRasterizerHelper.cpp:801-807](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp#L801)).
  - **No accurate-prefilter mode.** `aovPrefilterMode == Accurate` recording lives in `PathTracingIntegrator::IntegrateFromHit` ([PathTracingIntegrator.cpp:1842-1854](../src/Library/Shaders/PathTracingIntegrator.cpp#L1842), [:1914-1927](../src/Library/Shaders/PathTracingIntegrator.cpp#L1914)) — never reached when the integrator is a shader-op chain.
  - **FilteredFilm bypass works.** `bDenoisingEnabled` gating in [PixelBasedRasterizerHelper.cpp:759](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp#L759) is shared.
- **Diagnosis:** "limited" = no inline AOV and no accurate-prefilter inline-first-non-delta. The retrace fallback's AOV uses `IBSDF::albedo(rig)` directly which is fine for RGB, but it records at the very first surface hit (including glass front faces and mirrors) — refractive interiors get glass-color AOVs while the beauty pass shows what's behind. This is the canonical "limited" pattern and is the same on every spectral row.
- **Effort:** for the legacy chunk, **out of scope**.
- **Refactor dependency:** would gate on a path-guiding-style "guiding-aware shader-op" rewrite; nothing in motion.

### 2.6 `pathtracing_spectral_rasterizer` — OIDN "(limited)"

- **Pel side wired at:** `PathTracingPelRasterizer::IntegratePixel` passes `&aov` to the integrator ([PathTracingPelRasterizer.cpp:304-310](../src/Library/Rendering/PathTracingPelRasterizer.cpp#L304)). The integrator records inline at first hit (Fast mode) or first non-delta scatter (Accurate mode).
- **Spectral side:** [PathTracingSpectralRasterizer.cpp:151-152](../src/Library/Rendering/PathTracingSpectralRasterizer.cpp#L151) (`IntegrateRayHWSS`) and [:178-179](../src/Library/Rendering/PathTracingSpectralRasterizer.cpp#L178) (`IntegrateRayNM`) take **no `PixelAOV*` argument**. The HWSS / NM integrator entry points listed in [PathTracingIntegrator.h:142-189](../src/Library/Shaders/PathTracingIntegrator.h#L142) lack the parameter. Spectral PT therefore depends entirely on the post-render retrace.
- **Diagnosis:** **deferred plumbing — refactor-blocked.** This is the textbook Phase-2b case from [docs/INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) §3.5: the AOV hook is in `IntegrateFromHit`, which has Pel/NM/HWSS triplicates. Phase 2b collapses them into a `IntegrateFromHitTemplated<ValueT>` with `if constexpr (traits::supports_aov)` gating ([INTEGRATOR_REFACTOR_PLAN.md:233](INTEGRATOR_REFACTOR_PLAN.md) lists this explicitly). Adding AOV plumbing to NM/HWSS without the templatization means duplicating the recording logic in two more 1,200-line methods — exactly the duplication Phase 2b exists to retire.
- **Effort:** if Phase 2b ships → 1 day to flip `supports_aov = true` on `NMTag` / `HWSSTag` and verify. If we skip Phase 2b → 2–3 days of careful copy-paste, but it directly contradicts the refactor plan and inflates the eventual templatization work.
- **Refactor dependency:** **Phase 2b strongly preferred.** Don't add AOV duplication on top of the existing NM/HWSS duplication.

### 2.7 `bdpt_spectral_rasterizer` — Path guiding ✓ (DONE 2026-05-07)

- **Pel side wired at:** `bdpt_pel_rasterizer` parser ([AsciiSceneParser.cpp:6397](../src/Library/Parsers/AsciiSceneParser.cpp#L6397) `AddPathGuidingParams`) supplies the **full** PathGuidingConfig set including `pathguiding_max_light_depth`, `pathguiding_complete_path_strategy_selection`, `pathguiding_complete_path_strategy_samples`.
- **Pre-fix spectral side:** descriptor hand-rolled a subset that omitted `pathguiding_learned_alpha`, `pathguiding_light_max_depth`, `pathguiding_complete_path_strategy_selection`, and `pathguiding_complete_path_strategy_samples` (4 fields).  Finalize() also missed the last three.
- **Fix landed:** descriptor block replaced with `AddPathGuidingParams( P );`; 3 missing `bag.Has` lines added to Finalize.  `BDPTRasterizerBase::PreRenderSetup` honors every field regardless of subclass, so no integrator change was needed.  Smoke-tested with a minimal scene authoring all three previously-dropped params — parser accepts them.  Matrix flipped from "partial" to ✓.
- **HWSS interaction:** light-subpath guiding works as before; training samples on the NM path are luminance-projected (an open future direction for wavelength-dependent guiding, separate from this gap).

### 2.8 `bdpt_spectral_rasterizer` — Adaptive sampling ✗

- **Pel side wired at:** `BDPTPelRasterizer` carries `adaptiveConfig` ([BDPTPelRasterizer.h:83](../src/Library/Rendering/BDPTPelRasterizer.h#L83)), runs the per-pixel Welford luminance gate ([BDPTPelRasterizer.cpp:263-475](../src/Library/Rendering/BDPTPelRasterizer.cpp#L263)). Parser calls `AddAdaptiveSamplingParams` at line 6398.
- **Spectral side:** descriptor at [AsciiSceneParser.cpp:6499-6534](../src/Library/Parsers/AsciiSceneParser.cpp#L6499) **does not** call `AddAdaptiveSamplingParams`. `BDPTSpectralRasterizer` has no `adaptiveConfig` member; per-pixel termination logic absent.
- **Diagnosis:** **deferred plumbing**, but slightly more work than the path-guiding subset because the Pel adaptive loop reads `IRasterImage::GetPEL().GetLuminance()` and the spectral rasterizer accumulates XYZ. The luminance signal is the Y channel of XYZ — directly available — so no algorithmic change is needed.
- **Effort:** **~1 week.** Mirror the Pel adaptive loop in `BDPTSpectralRasterizer::IntegratePixel`, parameterizing on the Y-channel of the XYZ accumulator. Add `AddAdaptiveSamplingParams` to descriptor, parse in Finalize, plumb `adaptiveConfig` through the constructor and `RISE_API_CreateBDPTSpectralRasterizer` factory ([RISE_API.h](../src/Library/RISE_API.h) — see [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) for the ABI-preserving overload pattern).
- **Refactor dependency:** none, but ideally lands after Phase 3 (rasterizer consolidation) which extracts the per-pixel adaptive loop into a shared helper. If you build this before Phase 3, expect to refactor it later.
- **HWSS interaction:** the Welford gate fires once per pixel sample regardless of how many wavelength bundles a sample contains, so adaptive convergence rate per wall-clock-second is independent of `hwss true/false`. No special handling.

### 2.9 `bdpt_{pel,spectral}_rasterizer` — SMS ✗ (matrix corrected; excised 2026-05)

- **Diagnosis:** BDPT-with-SMS was excised on 2026-05-07. `RISE_API_CreateBDPT{Pel,Spectral}Rasterizer` no longer take `sms_*` parameters, and the BDPT chunk parsers hard-fail on any `sms_*` line. Both pel and spectral matrix rows now correctly show ✗. No implementation work.

### 2.10 `bdpt_{pel,spectral}_rasterizer` — Optimal MIS ✗ (matrix corrected + parser hard-rejects 2026-05-07)

- **Pre-fix state:** parser called `AddOptimalMISParams` and Finalize parsed the params into `stabilityConfig.optimalMIS{,TrainingIterations,TileSize}`. Values were stored on the rasterizer but never consumed — `grep OptimalMIS src/Library/Shaders/BDPTIntegrator.cpp` returns zero matches; the BDPT pel rasterizer inherits the `OptimalMISAccumulator` allocator from `PixelBasedPelRasterizer` but `BDPTIntegrator` never reads `rc.pOptimalMIS`.
- **Diagnosis:** Optimal MIS in RISE is a PT-only feature. Extending Kondapaneni 2019 (single-step BSDF-vs-NEE alpha) to BDPT's per-strategy-pair MIS is open research — the principled answer to the BDPT-power vs VCM-balance question per [docs/VCM.md](VCM.md):243.
- **Fix landed (Step 3, 2026-05-07):** `AddOptimalMISParams` removed from BDPT pel and BDPT spectral chunk descriptors and Finalize blocks; `optimal_mis*` lines authored against either chunk now hard-fail at parse time with `ChunkParser:: Failed to parse parameter name 'optimal_mis' (not declared in 'bdpt_pel_rasterizer' descriptor)`.  Verified manually; 0 production scenes affected.  Defaults consistency test migrated to validate hints against `pathtracing_pel_rasterizer` instead.
- **Effort:** real Optimal-MIS-in-BDPT implementation = open research, multi-month.
- **Refactor dependency:** would benefit from Phase 2c if the research direction picks up.

### 2.11 `bdpt_spectral_rasterizer` — OIDN albedo proxy ✓ (DONE 2026-05-07)

- **Pre-fix state:** [BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp) NM and HWSS first-bundle paths recorded `pAOV->albedo = BSDF->value(v.normal, rig) * π` — a Lambertian normal-incidence approximation.  Pel side uses `BSDF->albedo(rig)`.  `IBSDF::albedo` returns RISEPel directly, so spectral has always been able to call it.
- **Fix landed:** both BDPT-spectral AOV sites now call `BSDF->albedo(rig)` against a `RayIntersectionGeometric` whose `ray` field is synthesized from the real `eyeVerts[0]→eyeVerts[iv]` view direction (matching the Pel-side pattern).  Same fix applied to VCM-spectral (§2.17).
- **HWSS interaction:** `pAOV` is recorded once per pixel sample at hero wavelength; companions don't touch it. No HWSS-specific concern.

### 2.12 `bdpt_spectral_rasterizer` — first-hit AOV vs first-non-delta (separate from §2.11)

- **Pel side:** the Accurate-prefilter mode skips first-hit recording in the integrator and records at first non-delta scatter ([PathTracingIntegrator.cpp:1842](../src/Library/Shaders/PathTracingIntegrator.cpp#L1842)). BDPT pel inherits this through `PixelBasedPelRasterizer` for the parts of its loop that go through PT, but BDPT's primary AOV is the eye-subpath walk in [BDPTPelRasterizer.cpp:130-154](../src/Library/Rendering/BDPTPelRasterizer.cpp#L130) which already advances past delta vertices.
- **Spectral side:** the eye-subpath walk in BDPTSpectralRasterizer also advances past deltas, so the first-non-delta logic is preserved. The "limited" caveat at this layer was just the `value() * π` proxy from §2.11, retired 2026-05-07.  No deeper AOV-position issue remains.

### 2.13 `vcm_spectral_rasterizer` — Path guiding ✗

- **Diagnosis:** **architectural — out of scope.** VCM has no path guiding on either pel or spectral side. The merging pass and per-pixel directional density estimation are not cooperative — guiding the eye subpath through OpenPGL would skew the photon-merge pdfs in ways the Georgiev 2012 closed-form recurrence doesn't account for. Hooking guiding into VCM is open research; both Pel and spectral are equivalently blocked.
- **Effort:** out of scope.

### 2.14 `vcm_spectral_rasterizer` — SMS ✗

- **Diagnosis:** intentional. VCM's merging strategy already covers caustics; layering SMS on top adds combinatorial MIS complexity for no measurable variance reduction on caustic-heavy scenes vs VCM alone. Not a gap, not remediated. Same on Pel.

### 2.15 `vcm_spectral_rasterizer` — Optimal MIS ✗

- **Diagnosis:** **architecturally blocked.** VCM uses balance heuristic (β=1) by structural necessity — Georgiev 2012 dVCM/dVC/dVM running quantities only close algebraically at β=1. Switching to power-2 would break the O(1)-per-vertex MIS evaluation, force winner-takes-all between merging and connection at SPPM-shrunk radii, and collapse VCM's architectural advantage. Optimal MIS extension would be open research at the same depth as the BDPT extension. See [docs/MIS_HEURISTICS.md](MIS_HEURISTICS.md) and the CLAUDE.md "MIS heuristic per integrator" entry.
- **Effort:** out of scope.

### 2.16 `vcm_spectral_rasterizer` — Spectral merging via `RISEPelToNMProxy` (CORRECTNESS)

This is the only listed gap that's a **correctness** issue, not a feature gap. It gets its own §3.

### 2.17 `vcm_spectral_rasterizer` — OIDN albedo proxy ✓ (DONE 2026-05-07)

- Identical fix as §2.11.  [VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp) AOV walk now calls `BSDF->albedo(rig)` with a synthesized real-view-direction `RayIntersectionGeometric`.

### 2.18 `mlt_rasterizer` / `mlt_spectral_rasterizer` — All ✗

- **Diagnosis:** intentional and out of scope per [docs/MLT_POSTMORTEM.md](MLT_POSTMORTEM.md). MLT's chain mutation is not per-pixel-local; path guiding, adaptive sampling, optimal MIS, and SMS all assume per-pixel statistical accumulation. OIDN is default off because the entire image lives in MLT's splat film and OIDN smears across splat correlations.
- **Effort:** out of scope. Recommend documenting in [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4 as deliberate, not a gap.

---

## 3. Special section — VCM-spectral merging via `RISEPelToNMProxy`

This is the only gap with **correctness** (not just feature) impact. The proxy is documented in [docs/INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md):35 as a "v1 architectural debt", and in [docs/VCM.md](VCM.md):242 as a known limitation.

### 3.1 What the proxy does

Defined at [VCMIntegrator.cpp:129-132](../src/Library/Shaders/VCMIntegrator.cpp#L129):

```cpp
inline Scalar RISEPelToNMProxy( const RISEPel& p ) {
    return Scalar(0.2126) * p.r + Scalar(0.7152) * p.g + Scalar(0.0722) * p.b;
}
```

Standard Rec. 709 luminance projection. It is invoked at two sites in the spectral merging path:

1. **`EvalLightRadiance<NMTag>`** ([VCMIntegrator.cpp:208-211](../src/Library/Shaders/VCMIntegrator.cpp#L208)) — applied to `ILight::emittedRadiance(dir)` because `ILight` (point / spot / directional) has no NM virtual. Affects scenes with non-mesh delta-position luminaires; mesh emitters use `IEmitter::emittedRadianceNM` which is fully wavelength-aware.
2. **`LightVertexThroughput<NMTag>`** ([VCMIntegrator.cpp:248-251](../src/Library/Shaders/VCMIntegrator.cpp#L248)) — applied to the **stored** photon's accumulated `throughput` field at merge query time, because `LightVertexStore` only stores Pel.

Site (1) is a separate, smaller issue (point-light-through-glass scenes only). Site (2) is the substantive correctness gap.

### 3.2 What it gets wrong

The merge contribution at NM is:

```
contribution_NM = cameraBsdfNM * RISEPelToNMProxy( lv.throughput ) * weight
```

`lv.throughput` is the photon's accumulated alpha from light source through every BSDF lobe along the light subpath, captured at a single hero wavelength during the light pass. The luminance proxy projects this to a scalar **at merge time** rather than carrying the photon's per-wavelength throughput.

**Symptoms on dispersive caustics:**
- A photon that traveled through wavelength-dependent IOR (water, glass, prism) accumulates different throughputs at different wavelengths during the light pass. The store snapshots the hero-wavelength throughput. Companion wavelengths read the SAME stored RGB and project to luminance — losing the dispersion.
- A red-bias photon path (e.g. through a dye glass that absorbs strongly at 450 nm) and a green-bias photon path (absorbs strongly at 600 nm) merge with the same scalar luminance after the projection. Caustics that should show wavelength separation appear neutral.
- Wavelengths inside an absorption peak (e.g. a Beer-Lambert filter) get the projected luminance — overweighting frequencies that the path actually attenuated.

**Symptoms on HWSS dispersive scenes:** identical, but worse — HWSS specifically samples 4 stratified wavelengths per ray and the variance reduction depends on each wavelength carrying its own throughput. The proxy collapses all 4 to the same scalar, so HWSS's variance reduction over per-wavelength NM degrades to zero on merge-dominated regions.

**Symptoms on non-dispersive scenes (uniform IOR, no absorption):** small. The hero-wavelength throughput is wavelength-independent, so the projection is effectively a constant scaling that cancels in the MIS-weighted sum.

VC strategies (s=0, NEE, interior, t=1) are **fully wavelength-accurate** — they re-evaluate the connection BSDF per companion wavelength via `RecomputeSubpathThroughputNM`. Only the merge strategy uses the proxy.

### 3.3 What a per-wavelength photon store would look like

The data structure for the fix already exists. [VCMLightVertex.h:110-120](../src/Library/Shaders/VCMLightVertex.h#L110) declares:

```cpp
struct LightVertexNM : public LightVertex {
    Scalar throughputNM;
    Scalar nm;
    LightVertexNM() : LightVertex(), throughputNM(0), nm(0) {}
};
```

The struct is **declared but unused** — `grep -rn LightVertexNM src/Library/` returns only its declaration. The intended-but-not-built design:

1. **Store layout.** `VCMLightVertexStore` becomes templated on the vertex type (`LightVertex` for Pel, `LightVertexNM` for spectral) — the existing KD-tree balance/query template (`VCMLightVertexKDTree<T>`) already accepts a type parameter. The first two fields (`ptPosition`, `plane`) match for both, so the spatial index code is invariant.
2. **Light pass per-iteration.** The HWSS rasterizer already samples a hero wavelength per pixel. The light pass shoots one photon batch per progressive iteration; in spectral mode, sample a hero wavelength per photon (separate Sobol stream) and store both `throughput` (RGB rendering of the per-wavelength SPF dispatch) **and** `throughputNM` (the scalar NM throughput at the photon's hero) plus `nm`.
3. **Merge query at companion wavelength.** Hero matches the photon's `nm` → use `lv.throughputNM` directly. Companion mismatch → re-evaluate the photon's throughput at the companion wavelength via `BDPTIntegrator::RecomputeSubpathThroughputNM` over the photon's stored vertex chain — but the photon does **not** store its full chain (only the merge endpoint), so this requires either (a) storing the full chain (memory cost grows from ~88 B to ~88 B + 80 B per bounce), or (b) accepting the hero-wavelength throughput for companion merges and treating it as a stratification rather than fully wavelength-accurate.
4. **MIS weight.** `VCMMisQuantities` (dVCM/dVC/dVM) are wavelength-independent by construction ([VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp) asserts this); the same MIS weight applies regardless of which wavelength's throughput we read. Only the throughput accumulator changes.

**Two design choices to flag for cross-cutting questions §6:**

- **Store the full photon chain or accept per-photon-only-hero accuracy?** Full chain is principled but gives a ~10× memory cost on long subpaths. Per-photon-hero matches what HWSS does for connections and is internally consistent.
- **Hero distribution across photons.** If photons are sampled with hero ∈ stratified-equidistant, each merge's companion wavelengths see a photon population with diverse heroes, which gives some natural per-wavelength stratification even without per-companion re-evaluation.

### 3.4 Effort

- **Per-photon hero only:** ~2 weeks. Templatize the store on `LightVertex`/`LightVertexNM`, populate `throughputNM` + `nm` in the spectral light pass, swap the merge read path. No chain storage; companion mismatch falls back to current proxy with a warning. Captures most of the win on hero-matched merges.
- **Full chain storage + per-companion re-evaluation:** ~3–4 weeks plus careful memory measurement. Better correctness on heavy-dispersion scenes; pays a 10× memory tax on long subpaths.

Both are architectural, not refactor-blocked. Phase 2a's VCM templatization established the pattern (`LightVertexThroughput<Tag>` is already the dispatch point); the change is essentially "swap the NM specialization to read `lv.throughputNM` instead of projecting via proxy".

### 3.5 Test coverage

Required regression scenes for the fix:
- [scenes/Tests/VCM/triplecaustic_vcm.RISEscene](../scenes/Tests/VCM/triplecaustic_vcm.RISEscene) — three RGB dielectric caustics, the canonical wavelength-separation test.
- [scenes/Tests/VCM/pool_caustics_vcm.RISEscene](../scenes/Tests/VCM/pool_caustics_vcm.RISEscene) — water dispersion.
- A new `triplecaustic_vcm_spectral.RISEscene` would be ideal (none exists today).
- `cornellbox_vcm_spectral` for non-regression on the easy case.

---

## 4. OIDN "(limited)" decoded across all spectral rows

The single "(limited)" annotation in the matrix means different things on different rows. Post-2026-05-07-fix table:

| Concern | PT pel | PT spec | BDPT pel | BDPT spec | VCM pel | VCM spec | pixIntSpec |
|---|---|---|---|---|---|---|---|
| Inline AOV recording | Yes | **No** | Yes | Yes | Yes | Yes | **No** |
| AOV `albedo()` source | `BSDF->albedo(rig)` | retrace fallback `BSDF->albedo(rig)` | `BSDF->albedo(rig)` | `BSDF->albedo(rig)` ✓ (was `value*π`, fixed §2.11) | `BSDF->albedo(rig)` | `BSDF->albedo(rig)` ✓ (was `value*π`, fixed §2.17) | retrace fallback `BSDF->albedo(rig)` |
| Accurate-prefilter inline first-non-delta | Yes | **No** (entry points lack `pAOV`) | N/A (BDPT walks the eye subpath itself) | N/A (same) | N/A (same) | N/A (same) | **No** |
| FilteredFilm bypass on denoise | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| Pre-splat denoise ordering (BDPT/VCM only) | N/A | N/A | Yes | Yes | Yes | Yes | N/A |

**Reading the table:**

- **PT spectral and pixelintegratingspectral** are the only rasterizers with NO inline AOV. They depend entirely on `OIDNDenoiser::CollectFirstHitAOVs` ([OIDNDenoiser.cpp:687](../src/Library/Rendering/OIDNDenoiser.cpp#L687)), a 4-spp retrace pass that records at the very first surface hit. On scenes where the camera looks through glass or onto a mirror, the retrace AOV captures the glass / mirror — wrong for the beauty's interior signal.
- **BDPT spectral and VCM spectral** now match the Pel side on AOV-albedo source (post-§2.11/§2.17 fix).
- **Accurate-prefilter mode** only fires on `PathTracingIntegrator`-driven rasterizers. BDPT and VCM walk their own eye subpaths to find the first non-delta vertex — equivalent in effect, just a different code path.
- **The FilteredFilm bypass on denoise** (don't run the wide-filter reconstruction before OIDN sees the pixels) works on every rasterizer that goes through `PixelBasedRasterizerHelper`. All listed rasterizers do.

**So the "(limited)" annotation expands to:**

- For **PT spectral**: limited = no inline AOV at all + no accurate-prefilter mode. Closing requires Phase 2b.
- For **BDPT spectral / VCM spectral**: ~~limited = `value() * π` proxy on albedo~~ **CLOSED 2026-05-07** — the AOV-albedo source matches Pel. Remaining "(limited)" residue is just the accurate-prefilter inline-first-non-delta semantics being inherited from the BDPT/VCM eye-subpath walk rather than an explicit `aovPrefilterMode` branch — see §7-Q6 for the design question.  Recommend revising the matrix annotation from "(limited)" to ✓ once §7-Q6 is decided.
- For **pixelintegratingspectral**: limited = no inline AOV at all + no accurate-prefilter mode + shader-dispatch architecture. Out of scope (soft deprecation).

---

## 5. HWSS interaction summary

Per-gap HWSS implications:

| Gap | HWSS-only | HWSS-incompatible | Same as NM |
|---|---|---|---|
| PT-spectral inline AOV (§2.6) | – | – | ✓ — `pAOV` is per-pixel-sample, hero records at first non-delta |
| BDPT-spectral path-guiding subset (§2.7) | – | – | ✓ — config struct is wavelength-independent |
| BDPT-spectral adaptive sampling (§2.8) | – | – | ✓ — Welford reads XYZ.luminance, indifferent to bundle width |
| BDPT-spectral OIDN albedo proxy (§2.11) | – | – | ✓ — AOV is RGB by design |
| VCM-spectral merge proxy (§3) | **WORSE under HWSS** | – | – |
| VCM-spectral OIDN albedo proxy (§2.17) | – | – | ✓ — same as BDPT |
| MLT-* anything | – | most features are MLT-incompatible regardless of color mode | – |

**The HWSS amplification on §3 is worth highlighting:** HWSS's variance reduction on merge-dominated regions depends on each of the 4 sampled wavelengths carrying its own throughput. The luminance proxy collapses all 4 companions to the same scalar, so the variance reduction on merge contributions degrades to ≈0. On non-merge contributions (s=0, NEE, interior, t=1) HWSS works as designed. Net: **HWSS gives a smaller speedup on caustic-heavy spectral scenes specifically because of the proxy.** Per-wavelength photon stores would restore HWSS's full benefit.

---

## 6. Remediation plan (ranked by ROI)

### 6.1 Quick wins — days/weeks, no architectural blocker

| Status | Item | Effort | Files touched | Risk |
|---|---|---|---|---|
| **DONE 2026-05-07** | **Fix the matrix.** Row corrections in [docs/RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4 per §1 of this audit; soft-deprecation footnote added for `pixelintegratingspectral_rasterizer`; cross-cutting bullets rewritten to match source. | 5 min | 1 doc | None |
| **DONE 2026-05-07** | **Reject silently-ignored params (§7-Q4 → option a).** Removed `AddOptimalMISParams` from `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`, and `pathtracing_spectral_rasterizer` descriptors + Finalize.  Migrated `optimal_mis*` hint check in [RasterizerDefaultsConsistencyTest.cpp:305-307](../tests/RasterizerDefaultsConsistencyTest.cpp#L305) to `pathtracing_pel_rasterizer`.  Updated [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) helper-table.  Verified hard-fail with manual repro: `ChunkParser:: Failed to parse parameter name 'optimal_mis' (not declared in 'bdpt_pel_rasterizer' descriptor)`.  No production scenes affected (`grep -r optimal_mis scenes/` returns 0).  All 88 tests pass.  VCM already cleanly rejected path-guiding params at descriptor level; pixIntSpec already rejected adaptive/optimal_mis. | 1 day | [AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp), [Parsers/README.md](../src/Library/Parsers/README.md), [RasterizerDefaultsConsistencyTest.cpp](../tests/RasterizerDefaultsConsistencyTest.cpp) | Low — descriptor-driven parser hard-fails cleanly |
| **DONE 2026-05-07** | **BDPT-spectral path-guiding subset (§2.7).** Replaced hand-rolled descriptor block with `AddPathGuidingParams( P );` (now exposes `pathguiding_learned_alpha`, `pathguiding_light_max_depth`, `pathguiding_complete_path_strategy_selection`, `pathguiding_complete_path_strategy_samples`); added matching `bag.Has` lines for the 3 previously-dropped fields in Finalize.  Smoke-tested: parser accepts the previously-rejected params; build clean; all 88 tests pass. | 1 day | [AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) BDPTSpectralRasterizerAsciiChunkParser | Low — existing scenes that don't author the params unaffected |
| **DONE 2026-05-07** | **BDPT/VCM-spectral OIDN albedo proxy (§2.11, §2.17).** Replaced `BSDF->value(N,rig) * π` Lambertian-normal-incidence proxy with `BSDF->albedo(rig)` at all 3 sites; also synthesized the `RayIntersectionGeometric.ray` from the actual `eyeVerts[0]→eyeVerts[iv]` view direction (matching `BDPTPelRasterizer` pattern) so the BSDF reads a real view direction.  Smoke render of `cornellbox_bdpt_spectral.RISEscene` with OIDN denoise on completed cleanly; all 88 tests pass; build clean. | 1 day (both rasterizers) | [BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp) (NM + HWSS sites), [VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp) | Low — IBSDF::albedo signature unchanged, AOV is RGB |
| **DONE 2026-05-07 (review fix)** | **AOV `RayIntersectionGeometric` reconstruction now uses `PathVertexEval::PopulateRIGFromVertex` (5 sites).** Adversarial review (`audit-by-bug-pattern` skill) flagged that all 5 OIDN-AOV walks (BDPT pel, BDPT spectral × 2, VCM pel, VCM spectral) hand-rolled the rig and only set 3 of the 8 fields the helper populates — `ptCoord` / `ptObjIntersec` / `vColor` / `bHasVertexColor` were silently zero, giving textured surface AOVs the texel at UV (0,0) regardless of where the camera ray landed.  Replaced all 5 sites with the canonical helper (which has an explicit CONTRACT block in [PathVertexEval.h:94-106](../src/Library/Utilities/PathVertexEval.h#L94)).  Also harmonised the `GetBSDF()==nullptr` fallback to `RISEPel(1,1,1)` on BDPT-spectral (was `(0,0,0)` — black albedo would have over-blurred edge information for emitter-only materials).  Build clean, 88 tests pass. | 1 day | [BDPTPelRasterizer.cpp](../src/Library/Rendering/BDPTPelRasterizer.cpp), [BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp), [VCMPelRasterizer.cpp](../src/Library/Rendering/VCMPelRasterizer.cpp), [VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp) | Low — helper signature unchanged, behaviour for non-textured materials identical |
| **OPEN** | **BDPT-spectral adaptive sampling (§2.8).** Mirror the Pel adaptive Welford loop; add parser params; new `RISE_API_CreateBDPTSpectralRasterizer` overload (ABI-preserving per [skill](skills/abi-preserving-api-evolution.md)). | ~1 week | BDPTSpectralRasterizer.{h,cpp}, AsciiSceneParser, RISE_API.h, IJob.h, Job.cpp | Medium — new code path, needs regression scenes; ideally lands after Phase 3 |

### 6.2 Refactor-blocked — wait for Phase 2b/2c

| Item | Effort if Phase 2b lands | Effort without | Recommendation |
|---|---|---|---|
| **PT-spectral inline AOV (§2.6).** Templatized integrator's `IntegrateFromHitTemplated<ValueT>` exposes the AOV hook for NM/HWSS. | 1 day (flip a trait) | 2–3 days of NM/HWSS duplication | **Wait for Phase 2b.** The duplication-then-deduplication round trip is wasteful. Document as known-limited until then. |
| **(future) BDPT/VCM-spectral inline AOV improvements** if Accurate-mode equivalents are wanted. | Folded into Phase 2c. | – | **Not currently a documented gap; skip.** |

### 6.3 Architectural — design + multi-week implementation

| Item | Effort | Risk | Notes |
|---|---|---|---|
| **VCM-spectral per-wavelength photon store (§3).** Templatize `VCMLightVertexStore` on `LightVertex`/`LightVertexNM`, populate `throughputNM` + `nm` in spectral light pass, swap merge read path. | 2 weeks (per-photon-hero only) to 3–4 weeks (full chain storage). | High — touches threading, KD-tree determinism, HWSS bundle interaction. Phase 2a's `LightVertexThroughput<Tag>` is the right plug-in point. | Cross-cutting question for §7. |

### 6.4 Out of scope (matrix correction only, no implementation)

- pixelintegratingspectral path guiding / adaptive sampling / optimal MIS / OIDN-AOV (§2.1, §2.2, §2.4, §2.5) — legacy shader-dispatch chunk; users should migrate to `pathtracing_spectral_rasterizer`.
- BDPT optimal MIS (Pel + spectral) (§2.10) — open research, multi-month.
- VCM path guiding (Pel + spectral) (§2.13) — open research.
- VCM optimal MIS (Pel + spectral) (§2.15) — architecturally blocked by the Georgiev 2012 recurrence.
- MLT all features (§2.18) — deliberately empty.

---

## 7. Cross-cutting questions for design input

These came up during the audit and need a decision before the corresponding remediation can proceed.

1. **VCM per-wavelength photon storage: hero-only or full chain?**
   - Hero-only (~2 weeks, ~88 B/photon): each photon stores `(throughput, throughputNM, nm)`; companion-wavelength merges either fall back to the existing proxy (with a warning) or apply a "stratified hero" approximation that treats the photon population's hero distribution as natural variance reduction.
   - Full chain (~3–4 weeks, ~88 B + 80 B/bounce): each photon retains its bounce list; companion mismatches re-walk via `RecomputeSubpathThroughputNM`. Memory grows up to ~10× on long subpaths.
   - **Recommendation request:** per-photon-hero is the right v2 default for HWSS scenes; full chain is a v3 follow-up if dispersion-heavy regression scenes show residual error.

2. **BDPT-spectral adaptive sampling: per-pixel luminance signal source?**
   - Y-channel of the XYZ accumulator (cheapest, matches what the Pel rasterizer effectively does after RGB→Y).
   - HWSS-aware variance signal (track per-wavelength variance, terminate when all bundles stable).
   - **Recommendation request:** Y-channel is sufficient; HWSS-aware termination is a v2 improvement.

3. **Should the documentation-only matrix corrections (§6.1 row 1) be split out as a standalone doc PR?** **RESOLVED 2026-05-07:** yes — the Step-1 matrix correction is its own coherent change in the unstaged diff, ready to be split into a separate commit at commit time.  Rationale: separates "I don't believe what the doc says" from "I'm shipping new functionality."

4. **BDPT/VCM optimal MIS — accept-with-warning vs hard-reject?** **RESOLVED 2026-05-07 (Step 3):** hard-reject.  The parser no longer calls `AddOptimalMISParams` on BDPT pel, BDPT spectral, or PT spectral; lines authored against those chunks fail at parse time with the descriptor-mismatch error.  Honest and matches the descriptor-driven-parser convention RISE uses for every other never-consumed param.

5. **`pixelintegratingspectral_rasterizer`: deprecate or maintain?** **RESOLVED 2026-05-07 (Step 2): soft deprecation.** No runtime warning, no removal date.  Doc-only annotation in §3 catalogue, §4 matrix footnote, §6 spectral-mode notes, and the rasterizer header comment.  New scenes should use `pathtracing_spectral_rasterizer`; the legacy chunk stays for custom spectral shader-op chains.

6. **Accurate-prefilter mode in BDPT/VCM (Pel and spectral).** Currently the Accurate-mode AOV inline-recording pathway is PT-integrator-only. BDPT/VCM walk their own eye subpath to first non-delta, which gives equivalent semantics — but the parser still accepts `oidn_prefilter accurate` on BDPT/VCM and the `aovPrefilterMode` flag is set to Accurate even though the BDPT/VCM eye-subpath walk doesn't branch on it. Should the eye-subpath walk be made `aovPrefilterMode`-aware (e.g. for the prefilter AOV-buffer denoise step) or should we just document that BDPT/VCM treat fast-vs-accurate as a no-op on the AOV side? Today it's the latter implicitly; the explicit answer is missing from the docs.

---

## 8. References

- [docs/RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4 — the matrix (the source of truth this audit cross-checks).
- [docs/INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) §2-§3 — Pel/NM duplication map; Phase 2b/2c scope.
- [docs/INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) — what's done (Phase 0, 1, 2a) vs deferred.
- [docs/MIS_HEURISTICS.md](MIS_HEURISTICS.md) — power vs balance per integrator; why optimal MIS is currently PT-only.
- [docs/VCM.md](VCM.md) — VCM design, the "spectral merges use luminance proxy" v1 limitation (line 242), the HWSS interaction, scope of optimal-MIS research.
- [docs/OIDN.md](OIDN.md) — OIDN integration, the AOV retrace fallback and inline-AOV (Accurate) modes, the FilteredFilm bypass invariant.
- [docs/MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) — why MLT is deliberately feature-empty.
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) `CreateAllChunkParsers()` — the helper-template wiring this audit cross-references against the matrix.
- [src/Library/Shaders/VCMIntegrator.cpp:129-251](../src/Library/Shaders/VCMIntegrator.cpp#L129) — the `RISEPelToNMProxy` luminance projection and its callers.
- [src/Library/Shaders/VCMLightVertex.h:110-120](../src/Library/Shaders/VCMLightVertex.h#L110) — the unused `LightVertexNM` struct that's the storage destination for the §3 fix.
- [skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) — required reading for any of §6.1's `RISE_API_*` factory changes.
