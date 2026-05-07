# VCM (Vertex Connection and Merging) in RISE

This document describes the VCM integrator, a unified bidirectional light transport method that combines BDPT connection strategies with photon-mapping-style vertex merging under a single MIS umbrella (Georgiev et al., *Light Transport Simulation with Vertex Connection and Merging*, SIGGRAPH Asia 2012).

## Scope

- **VCMIntegrator** with **VCMPelRasterizer** (RGB) and **VCMSpectralRasterizer** (HWSS).
- Surface-only merging.  Medium scatter vertices traverse the recurrence (geometric + phase-function sampling updates using `sigma_t_scalar`) but are not stored or merged.  Connection transmittance through media is Tr=1 in v1.
- SPPM-style progressive radius shrinkage (global per-iteration, clamped by an adaptive density floor), with automatic median-segment-based fallback for the initial radius.  Set `vcm_disable_progressive_radius=true` in global options to revert to fixed-radius SmallVCM.
- Balance-heuristic MIS (matches SmallVCM); RISE BDPT's power-heuristic path is untouched.
- No SMS interop; no OpenPGL guiding.

## Reuse Strategy

BDPT already ships `GenerateLightSubpath` / `GenerateEyeSubpath` that produce `BDPTVertex` arrays with area-measure `pdfFwd` / `pdfRev`, delta flags, material pointers, and full medium / BSSRDF / dispersion handling.  VCM recovers the per-vertex SmallVCM running quantities `(dVCM, dVC, dVM)` in a **single-pass post-walk** over the generated array, so we avoid re-implementing ~2000 lines of generator code.

The one BDPT data-side change is two new fields on [src/Library/Shaders/BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h):

- `emissionPdfW` — solid-angle emission / importance PDF at path endpoints.  Light vertex 0 holds `pdfSelect * pdfPosition * pdfDirection`; camera vertex 0 holds `pdfCamDir`.
- `cosAtGen` — generator-side cosine at the receiving vertex, used by the VCM post-pass to invert `BDPTUtilities::SolidAngleToArea`.

BDPT itself never reads these fields.

### Post-specular vertex handling (Veach transparency)

BDPT follows the Veach convention of storing `pdfFwd = 0` on any vertex reached via a specular scatter — a "delta transparency" marker that tells the BDPT MISWeight walk to pass through the vertex without applying a pdf ratio there.  `ConvertLightSubpath` / `ConvertEyeSubpath` **must not** skip these vertices: they are the diffuse receivers of glass/mirror caustic chains, and skipping them would drop every caustic photon from the light vertex store.

The converter detects `isDelta` on the vertex itself and applies the **specular branch** of `ApplyBsdfSamplingUpdate` (which only needs `cosThetaOut` and explicitly zeros `dVCM` per Veach's convention) instead of the non-specular branch that would require `next.pdfFwd > 0`.  This preserves the SmallVCM recurrence across arbitrary specular chains and is essential for VCM to catch S-D-S-E caustics on dielectric / mirror scenes like triplecaustic.

### BSSRDF entry vertices

BSSRDF entry vertices are not ordinary edge-sampled BSDF vertices: their `pdfFwd` is already a spatial area-density for the sampled entry point, not a solid-angle PDF that should be converted through `dist^2 / cos`.  `ConvertLightSubpath` / `ConvertEyeSubpath` therefore skip the edge-Jacobian recurrence at `isBSSRDFEntry` and write `dVCM = 1 / pdfFwd` into the parallel MIS array.  They are still kept out of the merge store.  This lets VC strategies through SSS entry points compete with the sampled BSSRDF transport instead of inheriting the previous delta-marked boundary state and over-weighting `eye -> SSS -> light` paths.

### Endpoint eligibility: `isConnectible` vs `isDelta`

BDPT records two distinct flags per vertex: `isDelta` (which lobe was *sampled* to continue the subpath) and `isConnectible` (whether the material has any non-delta BxDF component).  A mixed material that sampled its specular lobe is `isDelta=true, isConnectible=true`.  BDPT explicitly clears endpoint `isDelta` for connectible vertices before MIS weight computation (BDPTIntegrator.cpp:4283) because connection evaluation uses the full non-delta BSDF, independent of the previously sampled lobe.

VCM follows this convention: all endpoint gates (store append, NEE, splats, interior connections, merges) check `isConnectible` only.  `isDelta` is used solely in the recurrence (specular branch of `ApplyBsdfSamplingUpdate`) and does not exclude vertices from VC/VM strategies.

### `vc_enabled` / `vm_enabled` ablation

Both flags are plumbed from the parser through `ComputeNormalization` into `VCMNormalization::mEnableVC` / `mEnableVM`.  All VC strategies (t=1 splat, s=0, NEE, interior connections) are gated by `mEnableVC` in both rasterizers.  VM is gated by `mEnableVM`.  When VM is disabled, `PreRenderSetup` skips the entire light vertex store build and KD-tree construction.

## Critical Files

| File | Role |
|---|---|
| [src/Library/Shaders/VCMRecurrence.h](../src/Library/Shaders/VCMRecurrence.h) / `.cpp` | Pure-math SmallVCM running quantities (`InitLight`, `InitCamera`, `ApplyGeometricUpdate`, `ApplyBsdfSamplingUpdate`, `ComputeNormalization`) |
| [src/Library/Shaders/VCMLightVertex.h](../src/Library/Shaders/VCMLightVertex.h) | Compact (~88 B) `LightVertex` record stored in the merging KD-tree |
| [src/Library/Shaders/VCMLightVertexStore.h](../src/Library/Shaders/VCMLightVertexStore.h) / `.cpp` | Append-then-query store with a left-balanced KD-tree cloned from `PhotonMapCore<T>` |
| [src/Library/Shaders/VCMIntegrator.h](../src/Library/Shaders/VCMIntegrator.h) / `.cpp` | Strategy implementations: `ConvertLightSubpath`, `ConvertEyeSubpath`, `EvaluateS0`, `EvaluateNEE`, `EvaluateInteriorConnections`, `SplatLightSubpathToCamera`, `EvaluateMerges` |
| [src/Library/Rendering/VCMRasterizerBase.h](../src/Library/Rendering/VCMRasterizerBase.h) / `.cpp` | Lifecycle + VCM light pass in `PreRenderSetup`, splat-film resolve in the flush overrides |
| [src/Library/Rendering/VCMPelRasterizer.h](../src/Library/Rendering/VCMPelRasterizer.h) / `.cpp` | Pel (RGB) subclass; `IntegratePixel` runs the per-sample eye pass |

## Pipeline

`VCMRasterizerBase::PreRenderSetup` runs **once per render** (before the block dispatch).  It:

1. Computes `mVCMNormalization` from `(W, H, mergeRadius, enableVC, enableVM)`.
2. Generates `W * H` light subpaths (single-threaded in v1).
3. For each subpath: `ConvertLightSubpath` produces the parallel `VCMMisQuantities` array and appends connectible surface vertices to the `LightVertexStore`.  (t=1 splats are handled per-sample in `IntegratePixel`, not here.)
4. `LightVertexStore::BuildKDTree()` balances the tree.

`VCMPelRasterizer::IntegratePixel` runs **per pixel per eye sample**.  It:

1. Generates the eye subpath via `BDPTIntegrator::GenerateEyeSubpath`.
2. Walks `ConvertEyeSubpath` to produce the eye-side `VCMMisQuantities` array.
3. Generates an **independent** per-pixel light subpath (used by interior connections for MIS state on both sides).  The store built in `PreRenderSetup` is used for merging only.
4. Accumulates the strategies: `EvaluateS0` + `EvaluateNEE` + `EvaluateInteriorConnections` + `EvaluateMerges`.

The final flush resolves the splat film on top of a scratch image before forwarding to the rasterizer outputs, matching BDPT's convention.

## SmallVCM Formulas (balance heuristic)

```
mLightSubPathCount  = W * H
etaVCM              = PI * radius^2 * mLightSubPathCount
mMisVmWeightFactor  = VM ? etaVCM       : 0
mMisVcWeightFactor  = VC ? 1 / etaVCM   : 0
mVmNormalization    = 1 / etaVCM
```

**InitLight** (at the emitter):
```
dVCM = directPdfA / emissionPdfW
dVC  = cosLight / emissionPdfW    (delta: 0)
dVM  = dVC * mMisVcWeightFactor   (note: VC, not VM)
```

**InitCamera** (at the pixel):
```
dVCM = mLightSubPathCount / cameraPdfW
dVC  = 0
dVM  = 0
```

**ApplyGeometricUpdate** (at each bounce, before BSDF sampling):
```
dVCM *= distSq   (if not the first bounce off an infinite light)
dVCM /= |cosThetaFix|
dVC  /= |cosThetaFix|
dVM  /= |cosThetaFix|
```

**ApplyBsdfSamplingUpdate** (non-specular branch, after BSDF sampling):
```
dVC  = (cosThetaOut / bsdfDirPdfW) * (dVC  * bsdfRevPdfW + dVCM + mMisVmWeightFactor)
dVM  = (cosThetaOut / bsdfDirPdfW) * (dVM  * bsdfRevPdfW + dVCM * mMisVcWeightFactor + 1)
dVCM = 1 / bsdfDirPdfW
```
(Specular branch: `dVCM = 0`, `dVC *= cosThetaOut`, `dVM *= cosThetaOut`.)

See [src/Library/Shaders/VCMRecurrence.cpp](../src/Library/Shaders/VCMRecurrence.cpp) for the authoritative implementation.

## Strategy Weights

Per-vertex MIS weights (balance heuristic), evaluated inside each `Evaluate*` function:

- **s=0** (eye hits emitter):
  `wCamera = directPdfA * dVCM + emissionPdfW * dVC`; `w = 1 / (1 + wCamera)`.

- **s=1** (NEE):
  `wLight = bsdfDirPdfW / (lightPickProb * directPdfW)`;
  `wCamera = (emissionPdfW * cosToLight / (directPdfW * cosAtLight)) * (mMisVmWeightFactor + dVCM + dVC * bsdfRevPdfW)`;
  `w = 1 / (wLight + 1 + wCamera)`.

- **t=1** (splat):
  `wLight = (cameraPdfA / mLightSubPathCount) * (mMisVmWeightFactor + dVCM + dVC * bsdfRevPdfW)`;
  `w = 1 / (wLight + 1)`.

- **interior** (s≥2, t≥2):
  `wLight = cameraBsdfDirPdfA * (mMisVmWeightFactor + L.dVCM + L.dVC * lightBsdfRevPdfW)`;
  `wCamera = lightBsdfDirPdfA * (mMisVmWeightFactor + C.dVCM + C.dVC * cameraBsdfRevPdfW)`;
  `w = 1 / (wLight + 1 + wCamera)`.

- **merge** (per candidate in query radius):
  `wLight = L.dVCM * mMisVcWeightFactor + L.dVM * cameraBsdfDirPdfW`;
  `wCamera = C.dVCM * mMisVcWeightFactor + C.dVM * cameraBsdfRevPdfW`;
  `w = 1 / (wLight + 1 + wCamera)`.
  The summed contributions are scaled by `mVmNormalization` and the eye vertex's throughput.

## Regression Scenes

| Scene | Purpose |
|---|---|
| [`cornellbox_vcm_simple`](../scenes/Tests/VCM/cornellbox_vcm_simple.RISEscene) | Minimal smoke test |
| [`cornellbox_vcm_caustics`](../scenes/Tests/VCM/cornellbox_vcm_caustics.RISEscene) | Cornell + glass sphere — VC+VM on LSDE caustics |
| [`cornellbox_vcm_spectral`](../scenes/Tests/VCM/cornellbox_vcm_spectral.RISEscene) | HWSS spectral variant |
| [`diacaustic_vcm`](../scenes/Tests/VCM/diacaustic_vcm.RISEscene) | Reflective-tube S-D-S-E caustic |
| [`triplecaustic_vcm`](../scenes/Tests/VCM/triplecaustic_vcm.RISEscene) | Three-sphere RGB dielectric caustics |
| [`pool_caustics_vcm`](../scenes/Tests/VCM/pool_caustics_vcm.RISEscene) | Water refraction caustics |

## Feature Parity with BDPT

`VCMPelRasterizer::IntegratePixel` mirrors `BDPTPelRasterizer::IntegratePixel` line-for-line on the structural side so that progressive rendering, per-sample light subpath splats, ZSobol blue-noise pixel distribution, adaptive sampling, and OIDN denoising all behave identically:

- **Progressive film state persistence** — each pass resumes from `(colorSum, weightSum, sampleIndex, wMean, wM2, wN)` instead of starting fresh, so `GetEffectiveSplatSPP` sees the cumulative sample count.
- **Per-sample t=1 splat** — `SplatLightSubpathToCamera` runs inside the sample loop so the splat film's variance reduces across progressive passes.  Splats land on the image-plane pixel determined by `BDPTCameraUtilities::Rasterize`, not the pixel being rendered.
- **`mTotalAdaptiveSamples` counter** + **`GetEffectiveSplatSPP`** — the splat film normalization uses the actual running total of rendered camera samples.  Adaptive early-termination increments the counter by the delta rather than a fixed batch size.
- **ZSobol blue-noise sampler** — Morton-code seeding, `effectiveIndex = (mortonIndex << log2SPP) | globalSampleIndex`, gated by `blue_noise_sampler true`.
- **Adaptive sampling** — Welford luminance variance check with `adaptive_max_samples` / `adaptive_threshold` / `show_adaptive_map`.  Converged pixels return their stored value and skip resampling.
- **Light subpath first, sharing the sampler** — mirrors `BDPTPelRasterizer::IntegratePixelRGB` so eye and light subpaths are jointly stratified across the Sobol dimensions.
- **OIDN denoising pipeline** — first-hit albedo / normal are extracted from `eyeVerts[1]` and accumulated into `pAOVBuffers`; the pre-denoised output is the splat-composited image, denoise runs on the non-splat pImage, then `FlushDenoisedToOutputs` composites splats back onto the denoised result.
- **Scene parser parity** — the `vcm_pel_rasterizer` chunk accepts the same knobs as `bdpt_pel_rasterizer` (minus SMS / path guiding / optimal MIS, which are out of scope for VCM v1): `oidn_denoise`, `adaptive_max_samples`, `adaptive_threshold`, `show_adaptive_map`, `blue_noise_sampler`, the full `StabilityConfig`, `progressive_rendering`, `progressive_samples_per_pass`.
- **Path-tree branching at multi-lobe delta vertices** — removed in 2026-05.  PT, BDPT, VCM, MLT all use stochastic single-lobe selection at Fresnel splits with proper `kray / selectProb` correction (matches PBRT/Mitsuba/Arnold/Cycles X convention).  See [CLAUDE.md](../CLAUDE.md) High-Value Facts entry on the BDPT MIS-vs-tree mismatch.  Scenes that previously set `branching_threshold` will now fail to parse — the parameter has been removed.

At 256 spp the diffuse Cornell box matches BDPT within 1% on both the pre-denoised and denoised outputs.

## Merge Radius

The `merge_radius` parameter controls VM merging:
- **`merge_radius > 0`** — explicit radius in world units, used as the initial `r_0`.
- **`merge_radius 0` + `vm_enabled true`** — automatic radius.  `VCMRasterizerBase::PreRenderSetup` runs a pre-pass over the generated light subpaths, collects the length of every segment where **at least one endpoint is a storeable vertex** (`isConnectible` surface), takes the **median** and multiplies by `0.01` to derive the initial merge radius `r_0`.  Filtering to storeable segments avoids skewing the median with long specular chains through glass.  The median is robust against outliers from infinite-plane hits at shallow angles.  The chosen value is logged as `auto-radius segments=... median_segment=... effective_radius=...`.
- **`vm_enabled false`** — VM is disabled regardless of radius; VCM degenerates to VC-only (matching BDPT).

### Progressive radius shrinkage (SPPM-style)

By default (`mProgressiveRadiusEnabled = true`), `VCMRasterizerBase` applies the Hachisuka-Ogaki-Jensen shrinkage formula once per progressive pass:

```
r_{n+1} = r_n * sqrt( (n + alpha) / (n + 1) )
```

with `alpha = 2/3` (asymptotically optimal rate) and `n = mMergeRadiusPassCount`.  The shrunk radius is clamped below by `mMergeRadiusFloor`, an adaptive lower bound derived from `mTargetPhotonsPerQuery` (default 20) and the current photon density so that Poisson noise on photon count does not drown out bias reduction.  A scene-derived safety floor (`0.001 * medianSegment`) prevents the radius from collapsing below a geometric floor on pathologically sparse scenes.

The photon-store build sets `mLightSubPathCount` to `pathsShot` each pass — equal to W×H since path-tree branching was removed in 2026-05; the renormalization is now a no-op but harmless.

To revert to the legacy fixed-radius SmallVCM behavior for benchmarking or regression comparison, set `vcm_disable_progressive_radius=true` in `global.options` (or via `RISE_OPTIONS_FILE`).

Defaults live in `VCMRasterizerBase::VCMRasterizerBase` at [src/Library/Rendering/VCMRasterizerBase.cpp](../src/Library/Rendering/VCMRasterizerBase.cpp).  The shrinkage call site is `OnProgressivePassBegin`.

## Spectral / HWSS Variant

`VCMSpectralRasterizer` mirrors `BDPTSpectralRasterizer` for spectral rendering.  Key HWSS property the integrator relies on: `VCMMisQuantities` (dVCM / dVC / dVM) are **wavelength-independent**, so `ConvertLightSubpath` / `ConvertEyeSubpath` from the Pel side are reused verbatim.  Only the strategy evaluators (`EvaluateS0NM`, `EvaluateNEENM`, `SplatLightSubpathToCameraNM`, `EvaluateInteriorConnectionsNM`, `EvaluateMergesNM`) have NM variants that return a Scalar radiance estimate; the caller converts to XYZ via `ColorUtils::XYZFromNM`.

HWSS flow per pixel sample:
1. Select a hero wavelength via `SampledWavelengths::SampleEquidistant`.
2. Generate light and eye subpaths at the hero wavelength (`GenerateLightSubpathNM` / `GenerateEyeSubpathNM`).
3. Run `ConvertLightSubpath` / `ConvertEyeSubpath` once — the result is shared across wavelengths.
4. Evaluate all VCM-NM strategies at the hero wavelength; accumulate `XYZFromNM(heroNM) × heroValue`.
5. For each companion wavelength: check `BDPTIntegrator::HasDispersiveDeltaVertex`.  If any vertex is dispersive, terminate the companion (its geometry doesn't survive wavelength change).  Otherwise copy the vertex arrays, call `BDPTIntegrator::RecomputeSubpathThroughputNM` to patch `throughputNM`, then run the NM evaluators again.

[tests/VCMSpectralRecurrenceTest.cpp](../tests/VCMSpectralRecurrenceTest.cpp) asserts the wavelength-invariance of `VCMMisQuantities` — ConvertLightSubpath / ConvertEyeSubpath produce bit-identical MIS arrays regardless of the wavelength-dependent `throughputNM` field.

## v2 Architecture: Per-Iteration Store + Parallel Light Pass

Each progressive pass is one VCM iteration (`progressiveConfig.samplesPerPass = 1` forced when VM is enabled), matching the paper's iteration model:

1. `PreRenderSetup` — auto-radius pre-pass, initial light-vertex-store build (for iteration 0).
2. `OnProgressivePassBegin(passIdx)` — rebuilds the store with fresh photon positions for iteration `passIdx` (using `sampleIndex = passIdx` in the Sobol sampler).
3. `IntegratePixel` — exactly 1 eye sample per pixel, querying the current iteration's store for merges.

Total SPP = number of iterations.  Each iteration gets a fresh photon store, so density-noise variance averages as 1/√N_iterations.

### Parallel light pass (`LightPassDispatcher`)

Light-subpath generation is parallelized via a 32×32 tile-based dispatcher in an anonymous namespace within `VCMRasterizerBase.cpp`:

- Tiles pulled by atomic counter (`nextTile.fetch_add`)
- Each worker has its own `RandomNumberGenerator` + `RuntimeContext` + scratch buffers + local `LightVertex` accumulator
- After all workers join, per-thread buffers are concat'd into the shared store in deterministic worker-index order (preserves KD-tree determinism)
- Sobol sampler indexing: `SobolSampler(sampleIndex=passIdx, pixelSeed=y*W+x)` — identical to the eye-pass sampler for the same (pass, pixel) pair, but draws from non-overlapping dimension streams via `StartStream()`

Worker count comes from `HowManyThreadsToSpawn()` (respects `maximum_thread_count` / `force_number_of_threads` options).

### Super-iteration batching — why not

The literature (SmallVCM, Mitsuba 3, PBRT-v4 SPPM) suggests batching K classical iterations into one super-iteration: `K × W × H` light subpaths in the store, K eye samples per pixel per super-iteration.  Georgiev eq. (20) confirms ηVCM = (nVM/nVC) × πr² is invariant under uniform K-scaling.

**Measured on RISE:** K=32 on diacaustic runs 5.8× slower per eye sample than K=1.  The issue is that per-iteration photon density scales K× and merge candidate counts scale K× too — merge evaluation becomes K× more expensive per sample.  SmallVCM/Mitsuba counter this with SPPM-style progressive radius reduction (r scales as 1/√K), which keeps candidate count constant.  RISE now applies SPPM-style shrinkage **across progressive passes (each K=1)** — see "Progressive radius shrinkage" above.  True super-iteration batching (K>1 with intra-batch per-pixel radius tracking) remains out of scope.

For v2, K=1 matches the paper and minimizes per-sample merge cost.  Parallel efficiency comes from within-iteration parallelism (tile-based dispatch) rather than cross-iteration batching.

### Block-level progressive output

VCM overrides `SkipPerBlockIntermediateOutput()` to return `true`.  With 1 spp per pass, per-block intermediate flushes (every 32×32 tile) would trigger thousands of output updates per render — wasted I/O.  The end-of-pass flush still runs once per iteration, giving the user progressive preview at the natural VCM iteration boundary.

## Known Limitations (v1 → v2)

- **Store lives for one iteration.**  Kept for v2: each iteration has a fresh store.  Density-noise variance averages across iterations as 1/√N.
- **Surface-only merging.**  Medium scatter vertices propagate the geometric update (using `sigma_t_scalar` in place of `|cos|`) so dVCM/dVC/dVM are approximately correct at post-media surface vertices, but medium vertices themselves are not stored.  Connections skip MEDIUM-type endpoints.  Connection transmittance through participating media is not evaluated in v1 (Tr=1); VCM connections in scenes with global or per-object media may be slightly overbright.  BDPT's full boundary-walking `EvalConnectionTransmittance` is a follow-up.
- **Spectral merges use hero throughput luminance for companion wavelengths.**  `EvaluateMergesNM` uses the stored Pel throughput's luminance as a companion-wavelength proxy for the light vertex store's accumulated throughput — the store is populated at a single wavelength during PreRenderSetup and cannot be re-walked per companion.  Scenes with strong wavelength-dependent emission near dielectric caustics will see reduced HWSS accuracy on the merge strategy only; VC strategies (s=0, NEE, interior, t=1) are fully wavelength-accurate.
- **No SMS, path guiding, or optimal MIS.**  User-confirmed out of scope for v1.
- **BDPT vs VCM heuristic mismatch.**  RISE BDPT uses power heuristic; VCM uses balance.  On diffuse scenes the two agree to within ~1% at 256 spp.

## Unit Tests

- [tests/VCMRecurrenceTest.cpp](../tests/VCMRecurrenceTest.cpp) — 75 assertions against SmallVCM formulas
- [tests/VCMLightVertexStoreTest.cpp](../tests/VCMLightVertexStoreTest.cpp) — 31 assertions incl. brute-force KD-tree comparison
- [tests/VCMLightPostPassTest.cpp](../tests/VCMLightPostPassTest.cpp) — 28 assertions on synthetic light subpaths
- [tests/VCMEyePostPassTest.cpp](../tests/VCMEyePostPassTest.cpp) — 26 assertions on synthetic eye subpaths
- [tests/VCMSpectralRecurrenceTest.cpp](../tests/VCMSpectralRecurrenceTest.cpp) — 27 assertions on HWSS wavelength-invariance

Total: 187/187 passing.
