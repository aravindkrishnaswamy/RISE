# RISE Integrator Refactor Plan

**Status**: PARTIALLY EXECUTED. Phases 0, 1, and 2a have shipped; Phases 2b, 2c, 3, and 4 are deferred. **Read [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) first for what's actually in the tree today** — this document is the original architectural plan, retained as the design record. Treat the body as historical context for the design choices, not as a to-do list to be re-executed wholesale.
**Owner**: Aravind Krishnaswamy
**Scope**: PathTracingIntegrator, BDPTIntegrator, VCMIntegrator, and their six rasterizers.

---

## 1. Executive Summary

**The problem.** Path tracing, BDPT, and VCM each ship with parallel Pel (RGB) and NM (single-wavelength) code paths, and PT also ships a third HWSS variant.  Inside those three integrators, the NM variant is a near-1:1 line-for-line mirror of the Pel variant (~95% parity).  Between them, VCM's four connection-style evaluators are mirror images of BDPT's `ConnectAndEvaluate` strategy cases — the connection math is the same; only the MIS weight differs.  Every new feature costs 6–8× the implementation labor.

**Scope**:

| File | Lines |
|---|---|
| `src/Library/Shaders/PathTracingIntegrator.{h,cpp}` | 4,118 |
| `src/Library/Shaders/BDPTIntegrator.{h,cpp}` | 7,733 |
| `src/Library/Shaders/VCMIntegrator.{h,cpp}` | 2,380 |
| `src/Library/Rendering/{PathTracing,BDPT,VCM}{Pel,Spectral}Rasterizer.{h,cpp}` | ~2,320 |
| `src/Library/Rendering/{BDPT,VCM}RasterizerBase.{h,cpp}` | ~2,083 |
| **Total** | **~18,634** |

Of these, **~6,000 lines are Pel↔NM duplication**, **~1,500 lines are Pel↔HWSS duplication** in PT, and **~2,900 lines are BDPT↔VCM connection-evaluation duplication**.  Combined, ~**10,400 lines of mechanical duplication** — more than half the file count.

**Goal.**
1. Single source of truth per algorithm, per concern.
2. Zero behavior change: bit-identical deterministic scenes, within-noise stochastic ones.
3. Zero performance regression (>5% is a hard fail; >2% triggers investigation).
4. Public `RISE_API.h`, `IMaterial`/`IBSDF`/`ISPF` interface, and all scene-file keywords unchanged.

**Non-goals.**
- No algorithmic changes.  No new features.  No bug fixes riding along.
- No changes to `IMaterial::GetSpecularInfoNM`, `valueNM`, `GetRandomWalkSSSParamsNM`, or any other wavelength-aware virtual (48 occurrences across 20 files — these stay as overloaded virtuals forever per [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md)).  The template layer sits *above* this API.
- No fix to the VCM `RISEPelToNMProxy` luminance approximation for spectral merging.  That's a known v1 correctness limitation; fixing it properly needs per-wavelength photon stores, which is a separate piece of work.
- No templating of the `BDPTIntegrator` / `VCMIntegrator` class itself.  Only the hot inner routines become templates.  The class stays a non-template so factory construction (`Job.cpp`, `RISE_API.h`) is unaffected.

**Expected outcome.** ~6,300 lines removed.  Integrator .cpps shrink by 40–55%.  Every new spectral or HWSS feature after this refactor writes once and runs everywhere.

---

## 2. Current State — Confirmed Findings

Four Explore passes plus direct reading of BDPT `ConnectAndEvaluate`, VCM `EvaluateNEE`, VCM `EvaluateInteriorConnections`, and `VCMSpectralRasterizer.cpp` established these facts.  Every claim below has a file:line anchor.

### 2.1 The Pel↔NM split is mechanical value-type swap

Every Pel/NM pair in the three integrators is algorithmically identical.  Differences reduce to:
- Return type: `RISEPel` (3 channels) vs `Scalar` (single λ).
- Method dispatch: `IBSDF::value(...)` ↔ `IBSDF::valueNM(..., nm)`, `Scatter` ↔ `ScatterNM`, `EvalBSDFAtVertex` ↔ `EvalBSDFAtVertexNM`.
- Throughput field: `BDPTVertex::throughput` ↔ `BDPTVertex::throughputNM` (both fields coexist on every vertex; [BDPTVertex.h:84-85](../src/Library/Shaders/BDPTVertex.h)).

### 2.2 HWSS is a third variant in PT, rasterizer-side in BDPT/VCM

- PT has `IntegrateRayHWSS` / `IntegrateFromHitHWSS` as full integrator-level variants ([PathTracingIntegrator.h:142-189](../src/Library/Shaders/PathTracingIntegrator.h)).
- BDPT and VCM implement HWSS at the **rasterizer** level: sample a hero wavelength, evaluate via the NM integrator API, then re-evaluate companion wavelengths by calling `BDPTIntegrator::RecomputeSubpathThroughputNM` plus the NM evaluators ([BDPTSpectralRasterizer.cpp:359-361](../src/Library/Rendering/BDPTSpectralRasterizer.cpp), [VCMSpectralRasterizer.cpp:420-423](../src/Library/Rendering/VCMSpectralRasterizer.cpp)).
- **VCM piggybacks on BDPT's HWSS infrastructure** — it calls `pIntegrator->RecomputeSubpathThroughputNM` on its owned `BDPTIntegrator` (the subpath generator) rather than re-implementing.

### 2.3 Pel-only divergences (template hazards, manageable)

| Concern | Variant | Anchor |
|---|---|---|
| PixelAOV (denoiser normal/albedo) | Pel only | [PathTracingIntegrator.cpp:1680-1713](../src/Library/Shaders/PathTracingIntegrator.cpp) |
| OpenPGL path guiding — RGB internal | PT + BDPT, Pel-primary, NM trains on luminance projection | both integrators |
| SMS state tracking parameters | Pel has extra `smsPassedThroughSpecular` param | [PathTracingIntegrator.h:110-112](../src/Library/Shaders/PathTracingIntegrator.h) |
| VCM has no path guiding | — | grep: 0 occurrences in VCMIntegrator.cpp |
| HWSS companion termination | HWSS only | [PathTracingIntegrator.cpp:3479-3498](../src/Library/Shaders/PathTracingIntegrator.cpp) |
| HWSS SPF-only/SSS fallback to NM | HWSS only | [PathTracingIntegrator.cpp:3042-3095](../src/Library/Shaders/PathTracingIntegrator.cpp) |
| Wavelength-dependent IOR | NM + HWSS must query material per λ | `IMaterial::GetSpecularInfoNM` etc. |
| VCM spectral LightVertexStore stores RGB only | NM merge uses luminance proxy | [VCMIntegrator.cpp:2055](../src/Library/Shaders/VCMIntegrator.cpp) |

All of these are resolvable with `if constexpr` branches on traits, or with a traits method that's a no-op for the variant that doesn't care.

### 2.4 BDPT and VCM share connection math

BDPT's `ConnectAndEvaluate(s, t)` and VCM's four connection evaluators (`EvaluateS0`, `EvaluateNEE`, `SplatLightSubpathToCamera`, `EvaluateInteriorConnections`) differ only in:
1. Where the light-side endpoint comes from (stored subpath vertex vs. freshly-sampled NEE direction vs. camera).
2. How the MIS weight is computed (Veach balance-heuristic walk vs. SmallVCM closed-form from `VCMMisQuantities`).

Steps 1–5 of each strategy — visibility, BSDF evaluation, geometric term, Le evaluation, connection transmittance — are **literally the same math**.  Confirmed by grep: both integrators route through `PathVertexEval::EvalBSDFAtVertex` and `EvalPdfAtVertex`, and VCM's `VCMIsVisible` ([VCMIntegrator.cpp:51](../src/Library/Shaders/VCMIntegrator.cpp)) is a direct copy of BDPT's `IsVisible`.

`MISWeight` lives outside the shared primitive — BDPT's balance walk (170 lines, [BDPTIntegrator.cpp:4239](../src/Library/Shaders/BDPTIntegrator.cpp)) is its own concern; VCM's SmallVCM formulas use the pre-computed `VCMMisQuantities` from the recurrence.

BDPT also does a pdfRev mutation trick during MIS weight computation (temporarily overwrites vertex pdfRev fields for the reverse-sampling PDF, then restores).  That trick is BDPT-MIS-local and stays inside `MISPolicyBDPT`; the shared primitive does not touch pdfRev.

### 2.5 Inheritance diamond

```
Rasterizer (virtual base)
  └── PixelBasedRasterizerHelper (virtual base)
        ├── PixelBasedPelRasterizer      (RGB pixel loop)
        ├── PixelBasedSpectralIntegratingRasterizer  (wavelength loop, XYZ conversion)
        ├── BDPTRasterizerBase           (SplatFilm, guiding, MIS accumulator)
        └── VCMRasterizerBase            (SplatFilm, LightVertexStore, KDTree)
BDPTPelRasterizer      : BDPTRasterizerBase, PixelBasedPelRasterizer
BDPTSpectralRasterizer : BDPTRasterizerBase, PixelBasedSpectralIntegratingRasterizer
VCMPelRasterizer       : VCMRasterizerBase,  PixelBasedPelRasterizer
VCMSpectralRasterizer  : VCMRasterizerBase,  PixelBasedSpectralIntegratingRasterizer
PathTracingPelRasterizer      : PixelBasedPelRasterizer
PathTracingSpectralRasterizer : PixelBasedSpectralIntegratingRasterizer
```

`IntegratePixel` already returns `RISEColor&` (holds XYZ or RGB uniformly).  `TakeSingleSample` has Pel/Spectral-specific virtuals.  The diamond is workable — we're not restructuring inheritance, only reducing duplication within each leaf.

### 2.6 Existing abstraction layer to build on

Already shared and generally clean:
- `BDPTUtilities.h`: geometric term, PDF measure conversion.  100% Scalar, no Pel dependence.
- `PathTransportUtilities.h`: RR, clamping, bounce limits.  Has dual-signature pairs ready to templatize.
- `MISWeights.h`: balance + optimal MIS.  Scalar-only.
- `PathVertexEval.h`: shared BSDF/PDF eval at surface/medium/BSSRDF-entry.  Already Pel/NM dual overloads.
- `BSSRDFSampling.h`: already carries both `weight`/`weightNM` and `weightSpatial`/`weightSpatialNM`.
- `VCMRecurrence.h`: SmallVCM dVCM/dVC/dVM math.  Wavelength-independent by construction.
- `ManifoldSolver.h`: dual-signature `EvaluateChainThroughput` / `EvaluateChainThroughputNM`.
- `SampledWavelengths.h`: fixed-size N=4 bundle with hero distinction, termination flags.

The existing abstractions are at the right level — they just aren't templated, so callers manually pick the Pel or NM overload.

---

## 3. Refactor Strategy

### 3.1 Design Principle: Traits dispatch, function templates, no class templates

- **Function templating, not class templating.**  Keep `BDPTIntegrator` / `VCMIntegrator` / `PathTracingIntegrator` as non-template classes.  Make hot methods templates on a `ValueT` parameter.  Rationale: class templating would fork the factory wiring, MLT coupling, and `Job.cpp` construction path.  Keeping the class non-template is the low-surface-area choice.
- **Traits struct `SpectralValueTraits<ValueT>`** holds the compile-time-branchable pieces: value type, zero/add/scale/luminance, wavelength context, feature flags (`supports_aov`, `supports_path_guiding`, `is_hwss_bundle`).
- **Policy structs for orthogonal concerns**: `MISPolicyBDPT` vs `MISPolicyVCM` for weight computation.
- **`if constexpr` over `#ifdef`** so the traits decisions are debugged, type-checked, and reviewed by the compiler.
- **Inline templated helpers in `.inl` files**, included only by integrator `.cpp` files.  No template in a public header → no compile-time bloat leaking into consumers, no ABI pollution.

### 3.2 Phase 0 — Foundation

**New files** (all under `src/Library/Utilities/Color/`):

```
SpectralValueTraits.h          // template<class ValueT> struct SpectralValueTraits;
SpectralValueTraits_RISEPel.h  // specialization
SpectralValueTraits_ScalarNM.h // specialization carrying nm
SpectralValueTraits_HWSS.h     // specialization over SampledWavelengths::N
```

And under `src/Library/Utilities/`:

```
PathValueOps.h                 // EvalBSDF<ValueT>, EvalPdf<ValueT> template wrappers
                               // that dispatch to existing Pel / NM overloads.
                               // Zero new logic — just a type-dispatch layer.
```

**Behavior change**: none.  Only new code.  Existing call sites untouched.

**New unit tests** (`tests/`):
- `SpectralValueTraitsTest.cpp` — every traits method on every specialization.
- `PathValueOpsTest.cpp` — each `EvalBSDF<ValueT>` produces identical output to the hand-called overload on representative materials (Lambertian, gold GGX, glass, BSSRDF skin).  This is the critical test that proves the traits layer is a no-op abstraction.

**Adversarial review gate (Phase 0)** — two rounds:

*Round 1, parallel reviewers:*
- **API plumbing reviewer**: does the traits header compile cleanly when included from both RGB-only and spectral contexts?  Are there ODR violations?  Do the specializations all expose the same interface?
- **Template instantiation reviewer**: can you write code that calls `EvalBSDF<RISEPel>(...)` and `EvalBSDF<ScalarNM>(...)` on the same BSDF and get bit-identical output to the direct `value(...)` / `valueNM(...)` calls?  Any overload resolution surprises?

*Round 2, single reviewer after fixes:*
- **ABI preservation reviewer**: re-read the new headers against [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md).  Do any of them leak into a public header (`RISE_API.h`, `Interfaces/*.h`) that an out-of-tree consumer might include?

### 3.3 Phase 1 — Templatize existing dual-signature utility pairs

Mechanical conversion, one utility at a time, each as an atomic commit:

| Utility | File | Change |
|---|---|---|
| `ClampContribution` / `ClampContributionNM` | `PathTransportUtilities.h` | Single function template + overload for Scalar |
| `GuidingRISCandidate` / `GuidingRISCandidateNM` | `PathTransportUtilities.h` | Single struct template |
| `BSSRDFSampling::SampleResult` Pel/NM fields | `BSSRDFSampling.h` | Template on ValueT |
| `ManifoldSolver::EvaluateChainThroughput{,NM}` | `ManifoldSolver.{h,cpp}` | Template method |
| `PathVertexEval::Eval{BSDF,Pdf}AtVertex{,NM}` | `PathVertexEval.h` | Single template, ValueT-dispatched body |
| `BDPTIntegrator::EvalConnectionTransmittance{,NM}` | `BDPTIntegrator.cpp` | Extract to `PathVertexEval.h` as template |

Each commit must be bit-identical at all Gate-B test scenes (see §4) and compile-time-equivalent (same inline asm for each specialization).

**Adversarial review gate (Phase 1)** — per-commit, two rounds:

*Round 1:*
- **Math-correctness reviewer**: does the templated `ClampContribution` (or whichever utility is being migrated) produce identical output to the original overload on (a) typical values, (b) zero throughput, (c) NaN/Inf, (d) grazing angles where applicable?  file:line + concrete failure scenario per finding.
- **Codegen-quality reviewer**: compile both the pre-refactor and post-refactor hot-inner-loop with `-O3 -S` and diff the asm.  Instantiation should produce identical object code.  If not: where did the compiler fail to inline?

*Round 2:*
- **Callsite-scan reviewer**: for each migrated utility, grep the entire codebase for the old symbol name (e.g. `ClampContributionNM`) and verify every caller compiles and produces identical output via the new template.  Any caller missed?

### 3.4 Phase 2a — VCM templatization

VCM goes first because:
1. Smallest (2,063 lines).
2. Its recurrence (`VCMMisQuantities`) is already wavelength-independent.
3. No OpenPGL integration — one fewer axis to parameterize over.
4. It owns a `BDPTIntegrator` but does not call into `BDPT::ConnectAndEvaluate` — only the subpath generators.  So changes here won't accidentally break BDPT users like MLT.

**Changes**:
- Collapse `EvaluateS0` / `EvaluateS0NM` into one template method `EvaluateS0<ValueT>`.
- Same for `EvaluateNEE`, `SplatLightSubpathToCamera`, `EvaluateInteriorConnections`, `EvaluateMerges`.
- Keep public non-template entry points as one-line forwarders:
  ```cpp
  RISEPel EvaluateS0(...) const { return EvaluateS0Templated<RISEPel>(...); }
  Scalar  EvaluateS0NM(..., Scalar nm) const {
      return EvaluateS0Templated<ScalarNM>(..., ScalarNM{nm}).v;
  }
  ```
  External callers (`VCMPelRasterizer`, `VCMSpectralRasterizer`, MLT if any) stay untouched.
- Inline `VCMIsVisible` into a shared helper in `BDPTUtilities.h` (so both BDPT and VCM stop having two copies).

**Expected line-count delta**: VCMIntegrator.cpp 2,063 → ~1,200 (−840).

**Adversarial review gate (Phase 2a)** — three parallel rounds:

*Round 1 (after VCM templatization lands on a branch, before merge):*
- **Math-correctness reviewer**: read the templated `EvaluateInteriorConnections<ValueT>` line-by-line against the original Pel and NM implementations.  For each line in the template, identify which pre-refactor line it corresponds to, note any divergence, and flag with file:line.  Questions: (a) are all visibility checks preserved?  (b) are all continue-conditions identical?  (c) is the MIS weight computed from the same dVCM/dVC values?  (d) is the geometric term formula unchanged?
- **Threading reviewer**: VCM's LightVertexStore is read-only after build, but the template now calls into it from both hero and companion HWSS wavelengths.  Any race on mutable state?  Any atomic counter shared between Pel and NM paths that the template inadvertently double-counts?
- **Numerical-stability reviewer**: grazing angles (cosTerm → 0), delta lights (isDelta=true gate), degenerate PDFs (bsdfDirPdfW = 0).  Does the template's `if(x <= 0) continue;` match exactly the original's early-exit pattern?  file:line + triggering scene configuration per finding.

*Round 2 (after Round 1 fixes):*
- **HWSS integration reviewer**: VCMSpectralRasterizer.cpp drives the templated VCM evaluators through hero-then-companions.  Read the new call sequence — does the HWSS companion loop still hit every strategy (S0, NEE, Interior, Splat, Merge)?  Does the dispersion termination still fire correctly?  file:line + specific wavelength pattern per finding.
- **Regression-suite reviewer**: independently run the Gate B baseline comparison on every `scenes/Tests/VCM/*` scene at fixed SPP and confirm diffs are within noise.  Report per-scene max-pixel RMS, mean luminance delta, and firefly count change.  Do NOT accept the main worker's claim of "passed" — rerun and verify.

*Round 3 (narrow final):*
- **MLT non-regression reviewer**: VCM owns a BDPTIntegrator for subpath generation.  Phase 2a doesn't touch BDPT yet, but `VCMIsVisible` consolidation might.  Verify `scenes/FeatureBased/MLT/*` still renders bit-identical-within-noise.  Report file:line of any MLT call site that now resolves to the consolidated helper.

### 3.5 Phase 2b — PathTracing templatization

PT is next because it stands alone — no upstream integrator depends on it.

**Changes**:
- Collapse `IntegrateFromHit` / `IntegrateFromHitNM` / `IntegrateFromHitHWSS` into `IntegrateFromHitTemplated<ValueT>`.  HWSS gets a traits specialization where `is_hwss_bundle = true` unlocks the companion-termination and SPF-fallback branches.
- Same for `IntegrateRay` / `IntegrateRayNM` / `IntegrateRayHWSS`.
- OpenPGL training: traits method `record_training_sample(rgb_or_luminance)` — Pel records RGB, NM records the luminance projection it already computes, HWSS no-ops (current behavior).
- PixelAOV population: `if constexpr (traits::supports_aov)` block at first-hit, non-AOV variants compile to nothing.
- SMS state tracking: move the extra Pel-only params onto a struct; HWSS ignores, NM ignores.

**Expected line-count delta**: PathTracingIntegrator.cpp 3,919 → ~2,000 (−1,900), and PathTracingShaderOp drops by ~100 for the redundant `PerformOperation{,NM,HWSS}` shim collapse.

**Adversarial review gate (Phase 2b)** — three rounds:

*Round 1:*
- **Math-correctness reviewer** (PT-focused): read `IntegrateFromHitTemplated<RISEPel>` against the original `IntegrateFromHit`.  Pay particular attention to the specular-branching iterative main loop — this is PT's most subtle piece.  Report file:line for any deviation in (a) bounce counting per ray type, (b) Russian roulette decision, (c) NEE + SMS combined contribution, (d) emission-sampling MIS weight.
- **HWSS reviewer**: read `IntegrateFromHitTemplated<HWSSBundle>` against the original `IntegrateFromHitHWSS`.  Questions: (a) does the SPF-only / SSS fallback delegate correctly back to the `<ScalarNM>` instantiation?  (b) is the dispersion-termination check on companion wavelengths preserved?  (c) does hero-only BSDF sampling still drive all companion wavelengths along the shared path?  (d) is the SampledWavelengths::N loop order preserved?
- **OpenPGL reviewer**: the integrator trains path-guiding fields from path segments.  In the original, Pel records RGB directly; NM records the luminance of the scalar radiance.  In the template, the traits method does the same — verify the recorded samples are identical for each ValueT via a unit test that snapshots `pgl_sample` output on a canned scene.

*Round 2:*
- **ABI reviewer** (re-run of the skill): `PathTracingIntegrator.h` is a public-ish header included by `PathTracingShaderOp.h` and the rasterizers.  Adding template declarations — does any virtual slot shift?  Does any overload name now cause name-hiding in a derived class that had the old signature?  Per [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md).
- **Regression-suite reviewer**: independently run Gate B on every `scenes/Tests/PathTracing/*` and every `scenes/Tests/Spectral/hwss_*pt*` scene.  Report per-scene metrics; rerun any that appear to have drifted.

*Round 3:*
- **BSSRDF reviewer**: PT integrates with BSSRDF entry adapters and random-walk SSS.  The traits layer must route BSSRDF sampling calls correctly for Pel / NM / HWSS.  Read [BSSRDFEntryAdapters.h](../src/Library/Shaders/BSSRDFEntryAdapters.h) against the templated call sites.  Does the Fresnel-fallback path still fire on NM?  Report file:line + a specific material for any divergence.

### 3.6 Phase 2c — BDPT templatization (no connection-primitive extraction yet)

BDPT is the most entangled integrator.  We split it into two phases: **2c templatizes Pel↔NM within each BDPT method** (no algorithmic restructuring); **2d extracts the shared connection primitive**.  Doing 2c first gives us a stable, bit-identical Pel/NM baseline to diff 2d against.

**Changes in 2c**:
- Collapse `GenerateLightSubpath` / `GenerateLightSubpathNM` into `GenerateLightSubpathTemplated<ValueT>`.
- Same for `GenerateEyeSubpath`, `ConnectAndEvaluate`, `EvaluateAllStrategies`, `EvaluateSMSStrategies`, `EvalBSDFAtVertex`, `EvalPdfAtVertex`, `EvalConnectionTransmittance`.
- Keep `MISWeight` non-template — it's PDF-only, already wavelength-agnostic.
- Thin one-line forwarders for all existing public Pel/NM entry points.

**Expected line-count delta**: BDPTIntegrator.cpp 7,266 → ~4,500 (−2,700).  MLT should see zero change because the public entry-point signatures are preserved.

**Adversarial review gate (Phase 2c)** — four rounds.  BDPT is the riskiest change; MLT sits downstream; SMS adds combinatorial complexity:

*Round 1:*
- **Math-correctness reviewer** (BDPT balance heuristic): read the templated `ConnectAndEvaluate` against both original Pel and NM variants.  `MISWeight` is untouched — confirm it's called from both instantiations with identical `(s, t, vertex arrays)` arguments.  The pdfRev mutation trick — is it still done pre-MISWeight and restored post?  Report file:line for any mutation that's now outside the save/restore window.
- **SMS reviewer**: read `EvaluateSMSStrategies` in template form against the original.  ManifoldSolver's `EvaluateChainThroughput{,NM}` was templated in Phase 1 — confirm both SMS paths (Pel, NM) still feed it correctly.  Specifically: (a) is the chain-throughput Pel/NM return type wired up to the correct `ConnectionResult` field?  (b) does the SMS fallback to stochastic BSDF sampling when the solver fails still fire?
- **Medium-transport reviewer**: BDPT has separate code paths for surface-surface, surface-medium, medium-medium connections.  Read the templated versions of `EvalConnectionTransmittance` against the originals.  Volume rendering is acoustically sensitive to transmittance drift — report file:line for any change to the Beer-Lambert accumulation or the null-scattering handling.

*Round 2:*
- **Subpath-generation reviewer**: read `GenerateLightSubpath<ValueT>` against both originals.  Questions: (a) are the throughput-and-isDelta updates identical per-bounce?  (b) is the RR decision driven by the same throughput summary (MaxValue for Pel, fabs for NM)?  (c) is the `emissionPdfW`/`cosAtGen` population on the light endpoint unchanged?
- **Path-guiding reviewer**: BDPT's path guiding is more involved than PT's — it trains both an eye-subpath field and (optionally) a light-subpath field, with strategy-selection candidates for RIS.  Confirm the templated subpath generators still record all the guiding training samples they used to.  file:line + specific training event per finding.
- **HWSS reviewer**: `RecomputeSubpathThroughputNM` and `HasDispersiveDeltaVertex` are the HWSS load-bearers.  Read the templated versions against the originals.  If either is now called through the template layer instead of directly, confirm the asm is identical (no virtualization penalty).

*Round 3:*
- **MLT non-regression reviewer**: run `scenes/FeatureBased/MLT/*` at matched mutation counts.  `MLTRasterizer` consumes `BDPTIntegrator::GenerateEyeSubpath` and `ConnectAndEvaluate` directly.  Bit-identical-within-noise?  Report per-scene metrics; rerun any outlier.
- **API-plumbing reviewer**: scan every call site of every BDPT public method.  Confirm each one still resolves to the correct forwarder.  Note that [BDPTSpectralRasterizer.cpp:359-361](../src/Library/Rendering/BDPTSpectralRasterizer.cpp) calls `RecomputeSubpathThroughputNM` by pointer — does that still work?

*Round 4:*
- **Regression-suite reviewer**: independently run Gate B on all of `scenes/Tests/BDPT/*` plus every `hwss_*bdpt*` scene plus `furnace_sss_*` plus all `scenes/FeatureBased/BDPT/*`.  Report per-scene metrics, highlight any outliers, rerun.

### 3.7 Phase 2d — Extract shared connection primitive (BDPT ↔ VCM)

Now that BDPT and VCM both have clean templated Pel/NM, extract the shared `EvaluatePathConnection<ValueT>` primitive.

**The primitive**:

```cpp
// New header: src/Library/Utilities/PathConnection.h
template<class ValueT>
struct PathConnectionResult {
    // Geometry (populated by EvaluatePathConnection)
    Scalar  dist, distSq;
    Scalar  cosAtLight, cosAtEye;
    Scalar  G;

    // Material evaluations (populated by EvaluatePathConnection)
    typename SpectralValueTraits<ValueT>::value_type  Le;
    typename SpectralValueTraits<ValueT>::value_type  fLight;   // 1 for camera vertex
    typename SpectralValueTraits<ValueT>::value_type  fEye;
    typename SpectralValueTraits<ValueT>::value_type  Tr;       // 1 if not requested

    // Intermediate PDFs (populated if requestPdfs=true; consumed by MIS policies)
    Scalar  bsdfDirPdfW_light, bsdfRevPdfW_light;
    Scalar  bsdfDirPdfW_eye,   bsdfRevPdfW_eye;

    // Status
    bool    visible;
    bool    valid;

    // The *unweighted* contribution, ready for MIS weight scaling
    typename SpectralValueTraits<ValueT>::value_type UnweightedContribution() const;
};

template<class ValueT>
PathConnectionResult<ValueT> EvaluatePathConnection(
    const BDPTVertex&     lightEndpoint,
    const BDPTVertex&     eyeEndpoint,
    const Vector3&        wiAtLight,          // incoming at light endpoint (unused if light-source vertex)
    const Vector3&        woAtEye,            // outgoing at eye endpoint
    const IScene&         scene,
    const IRayCaster&     caster,
    bool                  evalConnectionTransmittance,
    const SpectralValueTraits<ValueT>& ctx );
```

**Non-extraction from VCM**: `EvaluateMerges` stays VCM-specific — it's density estimation, not connection evaluation, truly different primitive.  Subpath-generation recurrence (`VCMMisQuantities`, `ConvertLightSubpath`, `ConvertEyeSubpath`) stays in `VCMRecurrence.{h,cpp}` where it already lives cleanly.

**MIS policies**:

```cpp
// Two policies, each consuming a PathConnectionResult plus its native MIS state.
// BDPT policy performs pdfRev mutation inside its Weight() call.
struct MISPolicyBDPT {
    Scalar Weight( const std::vector<BDPTVertex>& lightVerts,
                   const std::vector<BDPTVertex>& eyeVerts,
                   unsigned int s, unsigned int t,
                   const PathConnectionResult<...>& conn ) const;
};

struct MISPolicyVCM {
    const VCMNormalization*              norm;
    const std::vector<VCMMisQuantities>* lightMis;
    const std::vector<VCMMisQuantities>* eyeMis;

    Scalar WeightS0       ( unsigned int t, const PathConnectionResult<...>& conn ) const;
    Scalar WeightNEE      ( unsigned int t, Scalar lightPickProb, bool isDeltaLight,
                            const PathConnectionResult<...>& conn ) const;
    Scalar WeightT1       ( unsigned int s, const PathConnectionResult<...>& conn ) const;
    Scalar WeightInterior ( unsigned int s, unsigned int t,
                            const PathConnectionResult<...>& conn ) const;
};
```

**Caller reshaping**:
- `BDPT::ConnectAndEvaluate` becomes: build lightEp/eyeEp/directions from `(s, t, lightVerts, eyeVerts)`, call `EvaluatePathConnection<ValueT>`, multiply by `MISPolicyBDPT::Weight` and subpath throughputs, return `ConnectionResult`.
- `VCM::EvaluateS0<ValueT>` becomes: for each eye vertex at t>=2 that hits an emitter, synthesize a light endpoint, call `EvaluatePathConnection<ValueT>` (with `evalConnectionTransmittance=false` per v1), multiply by `MISPolicyVCM::WeightS0`, accumulate.
- Same pattern for `EvaluateNEE`, `SplatLightSubpathToCamera`, `EvaluateInteriorConnections`.

**Expected line-count delta after 2d**:
- BDPTIntegrator.cpp 4,500 (post-2c) → ~3,400 (−1,100).
- VCMIntegrator.cpp 1,200 (post-2a) → ~700 (−500).
- New `PathConnection.{h,inl}` + `MISPolicyBDPT.{h,cpp}` + `MISPolicyVCM.{h,cpp}`: +~900.
- Net across 2d: **−700 lines**.

**Adversarial review gate (Phase 2d)** — four rounds.  This is the algorithmic restructure, the highest-risk phase:

*Round 1:*
- **Primitive-equivalence reviewer**: read `EvaluatePathConnection<RISEPel>` alongside the original BDPT `ConnectAndEvaluate` bodies for each of its four strategy cases (s=0, s=1, t=1, interior).  Confirm each line of the primitive corresponds to an identical line in the original BDPT code.  file:line + failure mode for every deviation.
- **Primitive-equivalence reviewer** (VCM): read `EvaluatePathConnection<RISEPel>` alongside the original VCM `EvaluateS0`, `EvaluateNEE`, `SplatLightSubpathToCamera`, `EvaluateInteriorConnections` bodies.  Confirm the connection math (steps 1–5) is bit-identical.  Any operation that VCM does differently (e.g., the `pdfLight = lightStart.pdfFwd` in BDPT s=1 vs. `ls.pdfSelect * ls.pdfPosition` in VCM NEE) — is it caller-supplied or primitive-enforced?  file:line.
- **MIS-policy correctness reviewer**: read `MISPolicyBDPT::Weight` and confirm it still mutates pdfRev pre-MISWeight and restores post, exactly as the pre-refactor `ConnectAndEvaluate` did.  Read `MISPolicyVCM::Weight*` and confirm each strategy's SmallVCM formula is unchanged from the pre-refactor VCM evaluator body.

*Round 2:*
- **Cross-strategy equality reviewer**: run the new BDPT and VCM (with VM disabled) on `scenes/Tests/BDPT/cornellbox_bdpt.RISEscene` at high SPP.  VC-only VCM should converge to the same mean luminance as BDPT within 1%.  If not, the shared primitive is producing different intermediate math for one of the two callers.
- **Regression-suite reviewer**: Gate B on every BDPT and VCM scene.  Rerun outliers independently.  Do not accept a blanket "passed" from the main worker.
- **Media/BSSRDF reviewer**: BDPT uses connection transmittance, VCM v1 doesn't.  Confirm the primitive's `evalConnectionTransmittance` flag correctly gates the Beer-Lambert walk.  Run `scenes/FeatureBased/Combined/tidepools.RISEscene` (volume + BSSRDF) and confirm output is within noise of the 2c baseline.

*Round 3:*
- **MLT + SMS non-regression reviewer**: MLT and SMS both call `ConnectAndEvaluate` directly.  Phase 2d reshapes the internal body but keeps the public signature.  Re-verify every SMS scene + every MLT scene.  file:line for any drift.
- **HWSS reviewer**: VCM and BDPT HWSS both hit the primitive.  Confirm companion-wavelength re-evaluation still works through the primitive and that `HasDispersiveDeltaVertex` still gates correctly.

*Round 4:*
- **Performance reviewer**: run Gate D on every benchmark scene.  Compare wall-clock to the pre-2d baseline.  Hard fail >5%.  Investigate >2%.  The template instantiation across two callers must not produce worse inlining.
- **API-plumbing final reviewer**: scan all public BDPT and VCM entry points.  Every pre-refactor caller must compile unchanged.  Confirm MLT, AOV, shader-op paths unchanged.

### 3.8 Phase 3 — Rasterizer consolidation

With the integrators templated, the rasterizers' duplication becomes obvious.  Extract the pixel-loop orchestration plus the HWSS companion-wavelength loop into shared helpers.

**Changes**:
- New `PathTracingRasterizerBase` to match `BDPTRasterizerBase` / `VCMRasterizerBase` (gives PT a place to hang its path-guiding field that currently lives in the pixel-level parent).
- Move the HWSS companion-wavelength loop from `BDPTSpectralRasterizer::IntegratePixelSpectral` and `VCMSpectralRasterizer::IntegratePixelSpectral` into a shared `HWSSCompanionEvaluator` helper class.  Both rasterizers instantiate it with their own integrator reference.
- Move SplatFilm lifecycle and XYZ-conversion boilerplate up into the algorithm-specific bases.
- Use CRTP for the per-sample dispatch so the virtual `IntegratePixel` call disappears.

**Expected line-count delta**: six rasterizer .cpps 2,320 → ~1,200, plus algorithm bases +~400 for the helpers.  Net ~−700.

**Adversarial review gate (Phase 3)** — two rounds:

*Round 1:*
- **Inheritance-diamond reviewer**: after adding `PathTracingRasterizerBase`, the diamond deepens slightly.  Are there ambiguous member resolutions?  Does `PrepareRuntimeContext` still route correctly for each leaf rasterizer?  file:line for any virtual-dispatch ambiguity.
- **HWSS-helper reviewer**: the extracted `HWSSCompanionEvaluator` has to deal with (a) integrator-specific hero evaluation, (b) dispersion check, (c) companion recomputation, (d) per-strategy companion evaluation, (e) splat film writes.  Does the helper signature capture all of this cleanly without leaky abstractions?
- **Regression-suite reviewer**: Gate B on all spectral scenes (PT, BDPT, VCM).  This is the rasterizer phase — a drift here means the HWSS loop lost a wavelength or double-counted a splat.

*Round 2:*
- **Performance reviewer**: CRTP and helper extraction can kill inlining if done wrong.  Compare asm for the hot pixel loop before and after.  Gate D on all benchmark scenes.
- **Threading reviewer**: rasterizer changes can silently break the per-thread scratch pattern.  Verify every `mutable` field's ownership is unchanged and the SplatFilm's thread-local batching works identically.

### 3.9 Phase 4 — Cleanup

- Delete now-unused `FooNM` overloads where the template supersedes them and no external caller remains.  Scan MLT, shader ops, tests before each delete.
- Update comments in `BDPTIntegrator.h`, `VCMIntegrator.h`, `PathTracingIntegrator.h` to reflect the new templated structure.
- Update [docs/ARCHITECTURE.md](ARCHITECTURE.md) section on integrator architecture.
- Update [docs/VCM.md](VCM.md) to reflect the shared connection primitive.

**Adversarial review gate (Phase 4)** — one round:

- **Dead-code reviewer**: grep every symbol slated for deletion.  Confirm zero call sites before removal.  file:line for every remaining reference; delete only after each is accounted for.
- **Documentation reviewer**: do the updated comments and docs correctly describe the new structure?  Will a future maintainer understand the templating?

---

## 4. Regression Strategy (Gates)

Every phase-boundary commit passes all six gates before merge.  Gates fail closed: any gate red blocks the merge, no overrides without a documented exception and user signoff.

### 4.1 Gate A — Existing unit tests

```sh
make -C build/make/rise -j8 tests && ./run_all_tests.sh
```

Particularly load-bearing tests for this refactor:

- [VCMRecurrenceTest](../tests/VCMRecurrenceTest.cpp)
- [VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp)
- [VCMEyePostPassTest](../tests/VCMEyePostPassTest.cpp)
- [VCMLightPostPassTest](../tests/VCMLightPostPassTest.cpp)
- [VCMLightVertexStoreTest](../tests/VCMLightVertexStoreTest.cpp)
- [MISWeightsTest](../tests/MISWeightsTest.cpp)
- [OptimalMISAccumulatorTest](../tests/OptimalMISAccumulatorTest.cpp)
- [BSSRDFSamplingTest](../tests/BSSRDFSamplingTest.cpp)
- [BSSRDFEntryPointTest](../tests/BSSRDFEntryPointTest.cpp)
- [ManifoldSolverTest](../tests/ManifoldSolverTest.cpp)
- [SampledWavelengthsTest](../tests/SampledWavelengthsTest.cpp)
- [SPFBSDFConsistencyTest](../tests/SPFBSDFConsistencyTest.cpp)
- [SPFPdfConsistencyTest](../tests/SPFPdfConsistencyTest.cpp)
- [PSSMLTStreamAliasingTest](../tests/PSSMLTStreamAliasingTest.cpp)

**New unit tests added in Phase 0 / 1**:

- `SpectralValueTraitsTest.cpp` — traits correctness.
- `PathValueOpsTest.cpp` — `EvalBSDF<ValueT>` parity with direct overloads.
- `PathConnectionPrimitiveTest.cpp` (Phase 2d) — `EvaluatePathConnection<ValueT>` on synthetic BDPTVertex arrays, comparing against hand-unrolled pre-refactor math.
- `MISPolicyBDPTTest.cpp` + `MISPolicyVCMTest.cpp` (Phase 2d) — each policy weight on synthetic inputs.

### 4.2 Gate B — Bit-identical determinism + within-noise stochastic

Capture baselines once before Phase 2a:

```sh
./scripts/capture_refactor_baselines.sh
```

Renders to linear HDR (.pfm, no tone-map, no dither) under `tests/baselines_refactor/{scene}/baseline.pfm`, at fixed seeds and SPP.

After every refactor commit:

```sh
./scripts/check_refactor_baselines.sh
```

Deterministic scenes — **bit-identical expected** (pixel abs-diff ≤ 1e-6):
- Single-sample pinhole Lambertian cornell box in PT/BDPT/VCM × Pel/Spectral.
- Point-light direct-only scene (NEE-only, no sampling variance).
- MIS-weight-only scenes (shadow tests with no Monte-Carlo integration beyond per-pixel).

Stochastic scenes — **within 2σ of per-pixel sample variance**:
- All of `scenes/Tests/BDPT/`, `scenes/Tests/VCM/`, `scenes/Tests/PathTracing/`, `scenes/Tests/Spectral/`.
- Selected `scenes/FeatureBased/`: `bdpt_cloister`, `bdpt_jewel_vault`, `bdpt_alchemists_sanctum`, `pt_alchemists_sanctum`, `pt_jewel_vault`, `pt_jewel_vault_guided`, `showroom`, `glass_pavilion`, `crystal_lens`, `tidepools` (volume+BSSRDF), every `hwss_*` scene, `bdpt_torus_chain_atrium`.
- BSSRDF furnace: `furnace_sss_absorption`, `furnace_sss_zero_absorption`.

**Thresholds** (extending the pattern from [tests/test_ris_regression.sh](../tests/test_ris_regression.sh)):
- Mean luminance drift per region (full, dark floor, caustic patch) < 0.5%.
- Per-pixel log-luminance RMS < 1σ of per-pixel sample variance.
- Firefly count (pixel > 3σ of neighborhood mean) < 2× baseline.
- Spectral: XYZ tri-stimulus delta per region < 0.5%.

### 4.3 Gate C — Cross-strategy consistency

PT and BDPT should converge on scenes without caustics.  BDPT and VC-only VCM should converge on every scene.  Specifically:
- `scenes/Tests/RussianRoulette/cornellbox_highalbedo_{pt,bdpt}.RISEscene` at high SPP: mean luminance within 1% per channel.
- `scenes/Tests/BDPT/cornellbox_bdpt_correlation_test.RISEscene`: already on the test manifest.
- **New for this refactor**: run VCM with `enableVM=false` (connection-only) on every BDPT test scene and confirm convergence to within 1% of BDPT's mean luminance.  This is the critical gate for Phase 2d — the shared primitive has to produce identical connection math for both callers.
- HWSS vs NM convergence: `hwss_cornellbox_pt_ref.RISEscene` vs `hwss_cornellbox_pt_4samp_ref.RISEscene` — XYZ agreement to 0.5%.

### 4.4 Gate D — Performance non-regression

Pre-refactor baseline capture, once:

```sh
./bench.sh scenes/Tests/BDPT/cornellbox_bdpt.RISEscene 5
./bench.sh scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene 5
./bench.sh scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene 5
./bench.sh scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene 5
./bench.sh scenes/Tests/PathTracing/cornellbox_pathtracer.RISEscene 5
./bench.sh scenes/Tests/Spectral/hwss_cornellbox_pt.RISEscene 5
./bench.sh scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene 5
./bench.sh scenes/FeatureBased/Combined/crystal_lens.RISEscene 3
./bench.sh scenes/FeatureBased/BDPT/bdpt_jewel_vault.RISEscene 3
```

Per [docs/skills/performance-work-with-baselines.md](skills/performance-work-with-baselines.md): 5 runs per measurement, report stddev, single-variable commit-by-commit measurement.

**Thresholds**:
- Hard fail (blocks merge): any scene > 5% slower than baseline.
- Soft fail (investigate before merge): any scene > 2% slower.
- Sanity: HWSS stays > 3× faster than per-wavelength NM on prism dispersion scenes.
- Expected-or-better: template instantiation + inlining should match hand-specialized code within measurement noise (~1%).

### 4.5 Gate E — Visual inspection

Before merging each Phase-2 sub-phase:
- Side-by-side diff in an image viewer on: skin BSSRDF render (HyLIoS test scene), dispersive prism (HWSS-critical), one MLT run.  This catches the occasional "tests pass, image looks wrong" failure mode where a threshold was too loose.

### 4.6 Gate F — MLT non-regression (BDPT phases only)

MLT reuses `BDPTIntegrator`.  Must render bit-identical-within-noise:

```sh
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/MLT/mlt_keyhole.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/MLT/mlt_caustic_chain.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/MLT/mlt_torus_chain_atrium.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/MLT/mlt_reflected_caustic.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/MLT/mlt_chromatic_caustic.RISEscene
```

Compare to baselines at matched mutation counts.

### 4.7 Gate G — Adversarial review

Every phase runs its prescribed rounds of parallel adversarial review per [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md):

- Phase 0: 2 rounds, 3 reviewers total.
- Phase 1: per-commit, 2 rounds, 3 reviewers total.
- Phase 2a: 3 rounds, 8 reviewers total.
- Phase 2b: 3 rounds, 8 reviewers total.
- Phase 2c: 4 rounds, 11 reviewers total.
- Phase 2d: 4 rounds, 11 reviewers total.
- Phase 3: 2 rounds, 5 reviewers total.
- Phase 4: 1 round, 2 reviewers total.

Every reviewer prompt: self-contained, explicit file-path list, lettered question list, word cap, file:line-with-failure-mode report format.  Overlapping reviewer axes are forbidden — the skill doc's anti-pattern section is the operating guide.  Findings are verified against the code before acting.

A round that returns no P1 or P2 findings terminates the review for that phase.  A round that finds anything requires a fix + a narrower round 2 confirming resolution.

---

## 5. Phase Ordering and Sequencing

| Phase | Scope | Approx LoC delta | Risk | Rounds of review |
|---|---|---|---|---|
| 0 | Traits + `PathValueOps.h` + unit tests | +500 net | None | 2 |
| 1 | Templatize shared utility pairs | −300 net | Low | 2 per commit |
| 2a | VCM templatization | −840 | Medium | 3 |
| 2b | PathTracing templatization | −1,900 | Medium | 3 |
| 2c | BDPT templatization (no primitive extraction) | −2,700 | High | 4 |
| 2d | Extract `EvaluatePathConnection<ValueT>` + MIS policies | −700 (after counting new files) | High | 4 |
| 3 | Rasterizers + HWSS helper | −700 | Low-medium | 2 |
| 4 | Cleanup + docs | −200 | None | 1 |

**Total expected LoC savings: ~6,300 lines.**

Each phase lands as a single squashed commit against master (or a feature branch that merges whole; no in-flight half-refactored state).  Each commit's message lists:
1. Phase number and scope.
2. All gate results (A/B/C/D/E/F pass, with metrics).
3. Adversarial review round summary (N rounds, M reviewers, P1/P2 findings and how they were resolved).
4. LoC delta and performance delta.

---

## 6. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Templated codegen diverges from hand-specialized, silent perf hit | M | H | Phase 0 asm-diff check; Gate D every phase |
| Header bloat balloons compile times | M | M | All templates in `.inl` files, included only from `.cpp` — never from public headers |
| MLT or shader-op silently breaks | M | H | Gate F every BDPT phase; shader-op scan in Phase 4 cleanup |
| Path guiding silently regresses on spectral | M | M | OpenPGL-dedicated reviewer in every phase that touches guiding; `test_ris_regression.sh` extended to spectral variant |
| HWSS dispersive termination drifts in template | M | H | Phase-2 HWSS reviewer focused specifically on `HasDispersiveDeltaVertex`; hwss prism scene as Gate B fixture |
| VCM NEE vs BDPT s=1 primitive equivalence is wrong | M | H | Phase-2d primitive-equivalence reviewer reads both side-by-side; Gate C cross-convergence check |
| Diamond inheritance becomes unworkable in Phase 3 | L | M | Phase 3 is last — can be dropped if painful without losing the integrator wins |
| Baseline capture is non-deterministic (thread ordering, accumulation order) | M | H | Fix sampler seed per pixel per sample; disable adaptive sampling for baseline runs; verify determinism with 2 back-to-back captures before trusting |

---

## 7. What's Explicitly Out of Scope

- `IMaterial` / `IBSDF` / `ISPF` interface changes.  The `value` / `valueNM` dual-virtual pattern stays.  The template layer sits above it.
- `RISE_API.h` / `Job.cpp` factory wiring.
- Scene-file keywords.  User-facing behavior is identical.
- `RISEPel = ROMMRGBPel` typedef.
- Heavyweight `SpectralPacket` class.  Not used by integrators.
- VCM `LightVertexStore` per-wavelength fidelity.  The `RISEPelToNMProxy` luminance approximation is a v1 architectural debt; fixing it needs a new store layout and is a separate piece of work.
- MLT core algorithm.  Only touched if the templated BDPT public signature shape changes, which it won't.
- `IrradianceCache`, `FinalGatherShaderOp`, `AmbientOcclusionShaderOp`, and other shader-op variants that don't share the Pel/NM duplication pattern.

---

## 8. Open Questions For Review

1. Should `VCMIntegrator::EvaluateNEE` (fresh light sample per eye vertex) continue to sample per-vertex, or should Phase 2d take the opportunity to let it optionally reuse the stored s=1 light endpoint (matching BDPT)?  The current VCM behavior is per the SmallVCM paper.  Changing it would be a behavior change, which the plan forbids — but flagging for future work.

2. Phase 3 rasterizer CRTP: is there appetite to actually use CRTP (touches six class hierarchies), or would you prefer we skip and keep the `virtual IntegratePixel` dispatch?  The perf benefit is small — the pixel loop is L1-bound on ray casts, not dispatch.  Recommended: skip CRTP unless a phase-3 measurement shows the virtual call is non-trivial.

3. Baseline capture scope: 18 scenes listed in §4.2.  Should we extend to all ~60 scenes under `scenes/Tests/`?  Extra ~40 min per baseline refresh, but catches edge cases.  Recommended: yes, opt in.

4. Is Phase 2d's cross-strategy convergence gate (VC-only VCM ≈ BDPT) strict enough?  The 1% threshold matches the existing correlation test.  Could tighten to 0.3% on noise-free scenes (cornell box at 4096 SPP).

5. Should we build a CI job for the gates, or run them manually?  Manual is cheaper to set up but reviewer-dependent.  CI is stricter but needs GitHub Actions + HDR image comparison plumbing.  Recommended: manual for this refactor, CI as a follow-up after it lands.

6. Timeline: with all gates and rounds, Phase 2c and 2d are probably 2–3 days each of focused work.  Other phases 0.5–1 day each.  Total: ~2 weeks end-to-end.  Acceptable?

---

## 9. Go / No-Go Decision Points

The plan has three mandatory checkpoints where I pause and ask for your call:

1. **After Phase 0 lands and passes**: "Traits layer looks good — proceed to Phase 1?"
2. **After Phase 2c lands and passes**: "BDPT templatized and passing all gates.  Proceed to 2d primitive extraction, or stop here?"  (2d is the highest-risk phase; stopping before it still bankst ~5,000 lines of savings.)
3. **After Phase 2d lands**: "Proceed to Phase 3 rasterizer consolidation, or stop here?"  (Phase 3 is opt-in; most of the value is in the integrator phases.)

At each checkpoint, I'll present Gate B+C+D metrics, adversarial review summary, and LoC delta.  You decide whether the next phase runs.
