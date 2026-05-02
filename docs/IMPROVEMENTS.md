# RISE Renderer Improvements

This document catalogs the most impactful rendering improvements for RISE, informed by the major advances in offline physically based rendering from 2010-2025. Each item is scoped for an agent or contributor to pick up independently. Items are ordered by estimated ROI (impact per implementation effort).

This document covers material models, samplers, volume formulations, transport algorithms, and other systems. It supersedes the earlier `PATH_TRANSPORT_ROADMAP.md`.

For the cross-cutting end-to-end pipeline plan that closes the gap between the integrator and a defensibly physically based renderer (HDR output, ray differentials, spectral upsampling, physical camera, layered-material energy audit, glTF importer fidelity), see [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md).  That plan depends on item #11 (Jakob-Hanika spectral uplifting) below.

## Current RISE Baseline

RISE already has significant infrastructure in place:

- Full bidirectional path tracing with explicit light and eye subpaths
- Spectral rendering (sampled spectrum, per-wavelength PT/BDPT)
- SMS / manifold solving (Zeltner et al. 2020)
- OpenPGL path guiding with RIS and variance-aware adaptive alpha (PT and BDPT eye subpaths)
- BSSRDF-aware transport (Donner-Jensen, Burley diffusion profiles) in both PT and BDPT
- Owen-scrambled Sobol samplers
- PSSMLT via BDPT
- Caustic, global, and spectral photon mapping
- Intel OIDN denoising with per-sample albedo + normal AOVs
- Alias table + spatial RIS for many-light sampling
- Pure iterative PT rasterizers (Pel and Spectral) bypassing shader-op dispatch
- Wide-support pixel filter reconstruction via FilteredFilm
- Homogeneous and heterogeneous volume support with phase functions
- Environment map importance sampling

The improvements below target gaps where the field has advanced beyond what RISE currently implements.

---

## Ranked Improvements

| Rank | Improvement | Category | Effort | Depends On |
|------|------------|----------|--------|------------|
| 1 | ~~GGX microfacet + VNDF + Kulla-Conty multiscattering~~ **DONE** | Materials | Medium | None |
| 2 | ~~Light subpath guiding in BDPT~~ **DONE** | Transport | Medium | None (eye guiding complete) |
| 3 | ~~Random-walk subsurface scattering~~ **DONE** | Materials | Medium | None (disk projection complete) |
| 4 | ~~Light BVH for many-light sampling~~ **DONE** | Lights | Medium-Large | Roadmap Rank 1 |
| 5 | ~~Hero wavelength spectral sampling (HWSS)~~ **DONE** | Spectral | Medium-Large | None |
| 6 | ~~Blue-noise screen-space error distribution (ZSobol)~~ **DONE** | Sampling | Small | None |
| 7 | ~~Null-scattering volume framework~~ **DONE** | Volumes | Large | Roadmap Ranks 5-6 |
| 8 | ~~Optimal MIS weights~~ **DONE** | Transport | Medium | None |
| 9 | ~~VCM (Vertex Connection and Merging)~~ **DONE** (v1: VC+VM, surface-only, fixed radius, balance heuristic, Pel + Spectral/HWSS — see [docs/VCM.md](VCM.md)) | Transport | Medium-Large | None |
| 10 | Hair/fiber BSDF (Chiang et al. 2016) | Materials | Medium | ~~1 (GGX foundation)~~ None |
| 11 | Jakob-Hanika sigmoid spectral uplifting | Spectral | Small | 5 (HWSS) |

---

## 1. GGX Microfacet Model With VNDF Sampling And Multiscattering Compensation — DONE

**Implemented April 2026.**

### Summary

Full GGX (Trowbridge-Reitz) conductor material with:
- Anisotropic NDF with independent alpha_x, alpha_y roughness
- Smith height-correlated masking-shadowing G2 = 1/(1 + Lambda(wi) + Lambda(wo))
- VNDF importance sampling via the Dupuy-Benyoub (2023) spherical cap method
- Kulla-Conty multiscattering energy compensation with precomputed E_ss/E_avg LUTs
- Conductor Fresnel evaluated at the microfacet normal (half-vector)
- Three-lobe SPF: diffuse (cosine hemisphere) + specular (VNDF) + multiscatter (cosine hemisphere)
- Roughness clamped to 1e-4 minimum to avoid division-by-zero (no smooth-specular fallback needed; 1e-4 is effectively mirror)
- Reflection-only: BRDF returns zero for sub-surface directions

### Files

New:
- `src/Library/Utilities/MicrofacetUtils.h` — Core math (aniso NDF, Lambda, G2, VNDF sampling/PDF)
- `src/Library/Materials/GGXBRDF.h` / `.cpp` — BRDF (IBSDF interface)
- `src/Library/Materials/GGXSPF.h` / `.cpp` — SPF (ISPF interface, 3-lobe mixture)
- `src/Library/Materials/GGXMaterial.h` — Material wrapper
- `tests/GGXWhiteFurnaceTest.cpp` — 6-test suite: NDF normalization, VNDF PDF integral, energy conservation, isotropic consistency, G2>=G1*G1, material BRDF/SPF pointwise consistency

Modified:
- `src/Library/RISE_API.h` / `.cpp` — Public construction API
- `src/Library/Interfaces/IJob.h`, `src/Library/Job.h` / `.cpp` — Material creation
- `src/Library/Parsers/AsciiSceneParser.cpp` — `ggx_material` parser chunk
- `build/make/rise/Filelist` — Build system
- `tests/SPFBSDFConsistencyTest.cpp`, `tests/SPFPdfConsistencyTest.cpp` — GGX entries added

### Scene syntax

```
ggx_material
{
    name        my_ggx
    rd          diffuse_painter
    rs          specular_painter
    alphax      0.15
    alphay      0.15
    ior         2.45
    extinction  3.45
}
```

### Test scenes

- `scenes/Tests/Materials/ggx_white_furnace.RISEscene` — Luminaire box, 4 roughness levels
- `scenes/Tests/Materials/ggx_roughness_sweep.RISEscene` — 8 gold spheres, alpha 0.01–1.0 (PT)
- `scenes/Tests/Materials/ggx_roughness_sweep_bdpt.RISEscene` — BDPT variant
- `scenes/Tests/Materials/ggx_anisotropy_sweep.RISEscene` — 3x3 aluminum grid, alpha_x vs alpha_y (PT)
- `scenes/Tests/Materials/ggx_anisotropy_sweep_bdpt.RISEscene` — BDPT variant
- `scenes/Tests/Materials/ggx_vs_cooktorrance.RISEscene` — Side-by-side comparison, 4 roughness levels
- `scenes/FeatureBased/Materials/ggx_showcase.RISEscene` — Gold roughness sweep, brushed aluminum, copper/chromium accents

### Validation results

- All unit tests pass (GGXWhiteFurnaceTest, SPFBSDFConsistencyTest, SPFPdfConsistencyTest)
- White furnace: energy conserved across all roughness values
- PT and BDPT converge to the same result (no MIS/PDF bugs)
- GGX correctly brighter than CookTorrance at high roughness (height-correlated G2 less lossy)
- No regressions to CookTorrance or other existing materials

### Original specification

The original specification is preserved below for reference.

---

## 2. Light Subpath Guiding In BDPT — DONE

**Status:** Complete. Implemented April 2026.

### Overview

RISE guides eye subpaths in both PT and BDPT using OpenPGL with RIS-based sampling and variance-aware adaptive alpha (Roadmap Rank 8). Light subpath guiding extends this to the light side: light subpath bounces are guided toward high-contribution regions using a dedicated OpenPGL field, reducing variance for indirect caustics where the eye side alone cannot efficiently discover the transport.

The implementation uses separate fields (Option B): `BDPTRasterizerBase` creates a second `PathGuidingField` (`pLightGuidingField`) exclusively for light subpaths. Eye training data feeds the eye field; light training data feeds the light field. This avoids conflicting distributions in the same spatial cells — at a given surface position, the eye field learns "where does radiance arrive from" while the light field independently learns "where should light scatter toward." The shared-field approach (Option A) was implemented first but caused destructive interference: floor positions under a glass sphere learned contradictory distributions from eye training ("look up at ceiling light") vs light training ("scatter through glass"), increasing noise 17% when both were enabled simultaneously.

Light subpath training segments are recorded in reversed order so OpenPGL's incident-radiance semantics align with light-forward transport. The terminal segment (vertex 1, closest to the light) carries the emitted radiance `Le` as `directContribution`, recovered from `lightVerts[0].throughput * lightVerts[0].pdfFwd`. Vertices that terminated before scattering (no valid `guidingHasDirectionIn`) are skipped to avoid dangling segments with inconsistent zero `directionOut`.

No changes to `MISWeight()` were required — guided PDFs stored in `BDPTVertex::pdfFwd` auto-propagate through the existing power heuristic ratio chain.

### Scene Syntax

```
bdpt_pel_rasterizer
{
    pathguiding                   true
    pathguiding_max_depth         3       # eye subpath guiding depth (existing)
    pathguiding_light_max_depth   3       # light subpath guiding depth (new, 0 = disabled)
    pathguiding_sampling_type     ris     # ris or onesamplemis (applies to both)
    ...
}
```

Setting `pathguiding_light_max_depth` to `0` (the default) disables light subpath guiding entirely, preserving backward compatibility with all existing scenes.

### Algorithm

At each non-delta surface vertex in `GenerateLightSubpath` (up to `maxLightGuidingDepth` bounces), the guiding field is queried for an incident radiance distribution. Cosine product is applied. Two sampling strategies are available:

**RIS (recommended):** Two candidates (one BSDF, one guide) are drawn and resampled proportional to the target function using `GuidingRISSelectCandidate()` from `PathTransportUtilities.h`. The effective PDF flows into `pdfFwd` for MIS weight computation.

**One-sample MIS:** `pdfCombined = alpha * guidePdf + (1 - alpha) * bsdfPdf` with variance-aware adaptive alpha, identical to the eye subpath scheme.

Light subpath training data is collected via `RecordGuidingTrainingLightPath()`, which iterates light vertices in reverse order (last to first) and swaps direction-in/direction-out so that segments align with OpenPGL's incident-radiance convention. The reversed segments use dedicated reverse PDF and scattering weight fields (`guidingReversePdfDirectionIn`, `guidingReverseScatteringWeight`) computed during path construction, so the auxiliary data matches the reversed `directionIn` rather than the forward scatter direction. The terminal segment (vertex 1, closest to light, recorded last) carries the emitted radiance `Le` as `directContribution` — recovered as `lightVerts[0].throughput * lightVerts[0].pdfFwd` — giving OpenPGL a non-zero radiance source to backpropagate through the chain. Vertices without a valid scatter direction (`guidingHasDirectionIn == false`) are skipped entirely, preventing dangling segments with zero `directionOut` from polluting the field. A separate `thread_local GuidingDistributionHandle` (`lightGuideDist`) avoids cross-contamination with the eye subpath handle. Training and sampling both route through `pLightGuidingField` rather than the eye field.

### Files Modified

| File | Change |
|------|--------|
| `src/Library/Utilities/PathGuidingField.h` | Added `maxLightGuidingDepth` to `PathGuidingConfig` (default 0) |
| `src/Library/Shaders/BDPTIntegrator.h` | Added `pLightGuidingField` and `maxLightGuidingDepth` members, updated `SetGuidingField()` to accept separate light field |
| `src/Library/Shaders/BDPTIntegrator.cpp` | RIS/MIS guiding block in `GenerateLightSubpath` and `GenerateLightSubpathNM` using `pLightGuidingField`; guiding metadata on light vertices including reverse PDF/scattering weight; `RecordGuidingTrainingLightPath()` with reversed segment fields, Le-based emission, and dangling vertex filter; training calls in `EvaluateAllStrategies` and `EvaluateAllStrategiesNM` routed to light field; NM light vertex 0 sets `throughput` from `throughputNM` for Le recovery |
| `src/Library/Shaders/BDPTVertex.h` | Added `guidingReversePdfDirectionIn` and `guidingReverseScatteringWeight` for reversed training segments |
| `src/Library/Rendering/BDPTRasterizerBase.h` | Added `pLightGuidingField` member |
| `src/Library/Rendering/BDPTRasterizerBase.cpp` | Creates separate `pLightGuidingField` when `maxLightGuidingDepth > 0`; trains both fields in parallel; passes both to `SetGuidingField()` at all call sites; releases light field in destructor and cleanup |
| `src/Library/Parsers/AsciiSceneParser.cpp` | Parse `pathguiding_light_max_depth` in both BDPT chunks (Pel and Spectral) |

**Files unchanged (by design):** `PathTransportUtilities.h` (utilities reused as-is), `MISWeight()` (auto-correct via PDF chain), `PathTracingShaderOp.cpp` (untouched — no PT risk), `RuntimeContext.h` (`maxLightGuidingDepth` lives in integrator only).

### Test Scenes

| Scene | Description |
|-------|-------------|
| `scenes/Tests/BDPT/cornellbox_bdpt_light_guiding.RISEscene` | Cornell box + glass sphere, both eye+light guiding, RIS, 64 SPP |
| `scenes/Tests/BDPT/cornellbox_bdpt_caustics_eye_guided.RISEscene` | Eye-only guiding ablation, RIS, 128 SPP |
| `scenes/Tests/BDPT/cornellbox_bdpt_caustics_both_guided.RISEscene` | Both eye+light guiding ablation, RIS, 128 SPP |

### Validation Results

**Performance overhead** (Cornell box + glass sphere, 256x256, 128 SPP):

| Configuration | Wall Clock | Overhead |
|---|---|---|
| No guiding (baseline) | 23.9s | — |
| Eye-only guiding (RIS, depth 3) | 32.3s | +35% |
| Both eye+light guiding (RIS, depth 3) | 36.3s | +52% |

Light guiding adds ~12% on top of eye-only guiding, from the additional field query, RIS resampling, and reverse PDF computation per light vertex.

**Correctness:**
- Ablation renders (no guiding, eye-only, both) show identical overall brightness — no energy gain or loss detected. Caustic position, shape, and intensity are consistent across all three configurations.
- MIS weights verified in [0, 1] for all strategy pairs.
- Both eye+light guiding shows comparable or reduced noise vs. eye-only at equal sample count. The benefit is more pronounced on scenes with complex indirect caustics and deep light paths.

**PT regression:**
- All PT guiding scenes (`pt_indirect_test_guided`, `pt_indirect_test_ris`, `pt_guiding_stress_guided`, `pt_guiding_stress_ris`) produce visually identical output before and after implementation. PT code paths were never modified.

**BDPT regression:**
- BDPT scenes with `pathguiding_light_max_depth 0` (default) produce identical output to pre-implementation baselines, confirming zero behavioral change when the feature is disabled.

### Future Work

If convergence is poor on highly asymmetric (non-reciprocal) scenes, Option B (separate light-specific OpenPGL field) can be added without changing the sampling code — only the field pointer passed to `GenerateLightSubpath` changes.

---

## 3. Random-Walk Subsurface Scattering — DONE

**Implemented April 2026.**

### Summary

Volumetric random-walk SSS (Chiang, Burley et al., SIGGRAPH 2016) as a new material type, parallel to existing disk-projection BSSRDF. At a front-face hit, the algorithm refracts into the surface and traces a random walk using Beer-Lambert free-flight distance sampling and Henyey-Greenstein phase function scattering. When the walk exits the mesh, the exit point becomes the re-emission vertex with cosine-weighted sampling.

Key features:
- Mixture PDF throughput formulation (avoids infinite variance from per-channel selection)
- Stochastic Fresnel at exit boundary (coin-flip reflect/transmit, weight cancellation)
- TIR handling (reflect back inside and continue walk)
- Coefficient conversion utility (Burley scaling: s = 1.9 - A + 3.5*(A-0.8)^2)
- Full RGB and spectral (NM) support in both PT and BDPT
- Compatible with OpenPGL path guiding (entry vertices set identical guiding metadata)
- Existing disk-projection materials untouched (zero regression risk)

### Files

New:
- `src/Library/Utilities/RandomWalkSSS.h` / `.cpp` — Walk algorithm
- `src/Library/Utilities/SSSCoefficients.h` — Coefficient conversion (header-only)
- `src/Library/Materials/RandomWalkSSSMaterial.h` — Material class (header-only)
- `tests/RandomWalkSSSTest.cpp` — 5-test suite: walk exits sphere, energy conservation, pure absorption, coefficient conversion, TIR handling
- `scenes/Tests/SubsurfaceScattering/rwsss_sphere.RISEscene` — Basic sphere (PT)
- `scenes/Tests/SubsurfaceScattering/rwsss_bdpt.RISEscene` — BDPT convergence test
- `scenes/Tests/SubsurfaceScattering/rwsss_thin_slab.RISEscene` — Thin geometry test
- `scenes/Tests/SubsurfaceScattering/rwsss_colored.RISEscene` — Anisotropic absorption

Modified:
- `src/Library/Interfaces/IMaterial.h` — `RandomWalkSSSParams` struct + `GetRandomWalkSSSParams()`
- `src/Library/Shaders/PathTracingShaderOp.cpp` — RGB + NM random-walk blocks, `RandomWalkEntryBSDF` adapter
- `src/Library/Shaders/BDPTIntegrator.cpp` — 4 random-walk blocks (RGB/NM eye/light subpaths)
- `src/Library/Utilities/PathVertexEval.h` — Sw evaluation for random-walk entry vertices
- `src/Library/RISE_API.h` / `.cpp` — Public construction API
- `src/Library/Interfaces/IJob.h`, `src/Library/Job.h` / `.cpp` — Material creation
- `src/Library/Parsers/AsciiSceneParser.cpp` — `randomwalk_sss_material` parser chunk
- `build/make/rise/Filelist` — Build system

### Scene syntax

```
randomwalk_sss_material
{
    name        my_rwsss
    ior         1.3
    absorption  abs_painter
    scattering  scat_painter
    g           0.0
    roughness   0.05
    max_bounces 64
}
```

---

## 4. Light BVH For Many-Light Sampling

### Why This Is Fourth

RISE's alias table + spatial RIS handles moderate light counts well, but scales poorly to thousands of emitters. A light BVH (as in pbrt-v4) or light tree provides O(log N) importance-weighted selection that accounts for spatial proximity, orientation, and emitter power. This is already identified as Roadmap Stage 1C.

### What To Implement

#### 4A. Light BVH construction

Build a bounding volume hierarchy over all emitters (mesh luminaries and non-mesh lights unified under the shared light abstraction from Roadmap Stage 1A). Each node stores aggregate power, bounding box, and an orientation cone. Construction is a one-time cost at scene build time.

#### 4B. Importance-weighted traversal

At each shading point, traverse the light BVH from the root. At each internal node, compute an importance estimate based on:
- Aggregate power of the subtree.
- Distance from the shading point to the node's bounding box.
- Orientation compatibility (emitter normal cone vs direction to shading point).

Stochastically select left or right child proportional to importance. At a leaf, sample from the emitter. The product of selection probabilities along the traversal path gives the overall PDF.

#### 4C. MIS with BSDF sampling

The light BVH selection PDF replaces the alias-table PDF in the existing MIS framework. Ensure the PDF is evaluable for any emitter (needed for MIS when a BSDF-sampled ray hits an emitter).

### Current RISE Files

- `src/Library/Lights/LightSampler.h` / `.cpp` (current alias table + RIS)
- `src/Library/Managers/LightManager.cpp`
- `src/Library/Rendering/LuminaryManager.cpp`
- `src/Library/Shaders/PathTracingShaderOp.cpp` (NEE consumer)
- `src/Library/Shaders/BDPTIntegrator.cpp` (light subpath emission + connections)

### Deliverables

- Light BVH data structure with aggregate power, bounds, and orientation cones.
- Importance-weighted stochastic traversal with explicit PDF.
- Integration with PT NEE, BDPT light selection, and SMS light selection.
- Many-light test scene (100+ emitters) demonstrating improved convergence vs alias table.

### Acceptance Criteria

- Equal or lower noise on many-light scenes at fixed render time.
- Small light count scenes (< 10 emitters) do not regress.
- PDF is consistent: evaluable for any emitter at any shading point.

### Implementation (April 2026)

**Algorithm**: Conty & Kulla 2018, "Importance Sampling of Many Lights with Adaptive Tree Splitting", as implemented in pbrt-v4. Top-down BVH over emitters with orientation cones; stochastic traversal with xi-rescaling; root-to-leaf PDF evaluation for MIS.

**New files**:

| File | Purpose |
|------|---------|
| `src/Library/Lights/LightBVH.h` | BVH class, OrientationCone, LightBVHNode structs, full algorithmic documentation |
| `src/Library/Lights/LightBVH.cpp` | Build (top-down, power-weighted centroid split), Sample (stochastic traversal), Pdf (root-to-leaf walk) |
| `tests/LightBVHTest.cpp` | 15 unit tests: cone merge (5), construction (3), sampling (3), PDF (4) |
| `scenes/Tests/LightBVH/*.RISEscene` | Many-light test scenes (100-light corridor, 20-light corridor, 12-spotlight stage, BDPT variants) |

**Modified files**:

| File | Change |
|------|--------|
| `src/Library/Interfaces/ILight.h` | `emissionDirection()`, `emissionConeHalfAngle()` with defaults |
| `src/Library/Lights/SpotLight.h` | Override orientation methods for directed emission |
| `src/Library/Lights/LightSampler.h/.cpp` | BVH member, `SetUseLightBVH()`, BVH selection in `EvaluateDirectLighting`, BVH PDF in `PdfSelectLight`/`CachedPdfSelectLuminary` |
| `src/Library/Shaders/PathTracingShaderOp.cpp` | Pass shading point to `CachedPdfSelectLuminary` for BVH PDF |
| `src/Library/Shaders/BDPTIntegrator.cpp` | Pass predecessor vertex to `PdfSelectLuminary` for s=0 strategy |
| `src/Library/Rendering/RayCaster.h/.cpp` | `SetUseLightBVH()` with pending flag (applied before `Prepare()`) |
| `src/Library/Interfaces/IRayCaster.h` | `SetUseLightBVH()` pure virtual |
| `src/Library/Utilities/StabilityConfig.h` | `useLightBVH` field |
| `src/Library/Interfaces/IJob.h` | `StabilityConfig` parameter added to `SetMLTRasterizer` |
| `src/Library/Parsers/AsciiSceneParser.cpp` | `light_bvh` in `pixelpel_rasterizer`, `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`, `mlt_rasterizer` |
| `src/Library/Job.cpp` | Wire `stabilityConfig.useLightBVH` to `pCaster->SetUseLightBVH()` |
| `build/make/rise/Filelist` | Add `LightBVH.cpp` |

**Scene syntax**: Add `light_bvh true` to any `pixelpel_rasterizer`, `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`, or `mlt_rasterizer` chunk.

**Quantitative results** (100-light corridor, 64 SPP, 512x512):

| Method | MSE vs 1024-SPP ref | Relative MSE | Wall time |
|--------|---------------------|--------------|-----------|
| Alias table | 0.0422 | 2.26% | 12.3s |
| **Light BVH** | **0.0088** | **0.47%** | 12.3s |
| **Variance reduction** | **4.78x** | | ~0% overhead |

20-light corridor: ~1.84x local noise reduction. 2-light Cornell box: no regression. BDPT: correct, no energy gain/loss.

**Design notes**:
- BVH is used only for NEE light selection; BDPT emission sampling (`SampleLight`) stays on the alias table (power-proportional, no spatial bias needed).
- When BVH is active, RIS is automatically disabled (BVH supersedes it with tractable PDF for full MIS).
- BVH is disabled by default; existing scenes produce identical output.

---

## 5. Hero Wavelength Spectral Sampling (HWSS) — **DONE** (5A, 5B, 5C, 5D)

### Why This Is Fifth

RISE already does spectral rendering with a sampled-spectrum approach, evaluating all wavelengths per path. HWSS (Wilkie et al., EGSR 2014) reduces this to a single hero wavelength driving all directional decisions, with 3 companions sharing the geometric path but carrying independent spectral throughput. Overhead drops to ~5-15% over RGB. At specular dispersive interfaces, secondary wavelengths are terminated, enabling correct dispersion without tracing separate rays per wavelength.

This is listed as a non-goal in the current roadmap ("deep spectral redesigns such as hero-wavelength sampling"), but the survey of the field strongly suggests it is the right long-term architecture. Both pbrt-v4 and Manuka (Avatar sequels) use HWSS as their spectral foundation. It composes well with null-scattering volumes (spectral tracking) and path guiding.

### What To Implement

#### 5A. SampledWavelengths state

Add a `SampledWavelengths` object carrying 4 wavelengths per path. Sample the hero uniformly from the visible range; place 3 companions at equidistant spectral offsets with wrap-around: lambda_i = lambda_min + mod(lambda_h - lambda_min + i * Delta, lambda_max - lambda_min).

#### 5B. Per-path wavelength propagation

Replace the current approach of evaluating all wavelengths at each vertex with hero-only directional decisions. BSDF sampling, NEE direction selection, and light selection all use the hero wavelength. Companion wavelengths evaluate throughput at the shared geometric direction.

#### 5C. Secondary wavelength termination at specular dispersive interfaces

When a path hits a perfectly specular interface with wavelength-dependent IOR (Cauchy/Sellmeier), terminate the 3 companion wavelengths. This degenerates to single-wavelength transport for the dispersive segment. For rough dispersive BSDFs, MIS over wavelengths remains valid.

#### 5D. XYZ conversion

Weight final `SampledSpectrum` contributions with CIE color matching functions evaluated at the sampled wavelengths, divided by wavelength PDFs.

### Implementation Summary

#### 5A. SampledWavelengths state

Header-only value type in `src/Library/Utilities/Color/SampledWavelengths.h`: 4 equidistant wavelengths with wrap-around, uniform PDF, per-wavelength termination flags. Factory method `SampleEquidistant(u, lambda_min, lambda_max)`.

#### 5B. Per-path wavelength propagation (PT)

`PathTracingShaderOp::PerformOperationHWSS` drives all directional decisions (ScatterNM, NEE) with the hero wavelength. Companion throughputs are evaluated via a two-tier strategy:
1. **`ISPF::EvaluateKrayNM`** — new virtual method on the SPF interface (default returns -1). Overridden by `PolishedSPF` for its dielectric coat and diffuse substrate lobes, which are direction-independent and not represented by the material's BSDF.
2. **`IBSDF::valueNM` fallback** — for all materials with a BSDF (Lambertian, GGX, CookTorrance, etc.), companion throughput is `BSDF::valueNM(wo) * cos(theta) / pdf`. This correctly handles direction-dependent lobes.

SPF-only materials (DielectricSPF, PerfectReflector, PerfectRefractor, BioSpecSkin, GenericHumanTissue) and SSS materials fall back to independent per-wavelength `PerformOperationNM`.

`IRayCaster::CastRayHWSS` / `RayCaster::CastRayHWSS` handle the single intersection + HWSS shader dispatch.

#### 5B (BDPT)

`BDPTSpectralRasterizer` evaluates hero via existing `IntegratePixelNM`, then re-evaluates companion throughputs via `BDPTIntegrator::RecomputeSubpathThroughputNM` at each active companion wavelength.

#### 5B (MLT)

`MLTSpectralRasterizer` wraps the BDPT spectral evaluator with Metropolis acceptance based on hero luminance, re-evaluating companions per accepted mutation.

#### 5C. Dispersive companion termination

`BDPTIntegrator::HasDispersiveDeltaVertex` walks stored BDPT vertices and compares `GetSpecularInfoNM` IOR at hero vs companion wavelengths. When IOR differs at any delta vertex, all companions are terminated via `SampledWavelengths::TerminateSecondary()`. PT handles this via the SPF-only fallback (dielectric materials have no BSDF).

#### 5D. XYZ conversion

Each rasterizer (`PixelBasedSpectralIntegratingRasterizer`, `BDPTSpectralRasterizer`, `MLTSpectralRasterizer`) converts per-wavelength contributions to XYZ via `XYZFromNM(lambda) * c / pdf`, summed over active wavelengths and divided by `numActive`.

### Files

| File | Role |
|------|------|
| `src/Library/Utilities/Color/SampledWavelengths.h` | HWSS wavelength bundle: 4 wavelengths, PDFs, termination |
| `src/Library/Interfaces/ISPF.h` | Added `EvaluateKrayNM` virtual (default -1) |
| `src/Library/Materials/PolishedSPF.h/.cpp` | `EvaluateKrayNM` override for coat and diffuse lobes |
| `src/Library/Interfaces/IShaderOp.h` | Added `PerformOperationHWSS` virtual with default |
| `src/Library/Interfaces/IRayCaster.h` | Added `CastRayHWSS` virtual with default |
| `src/Library/Shaders/PathTracingShaderOp.h/.cpp` | `PerformOperationHWSS` override |
| `src/Library/Rendering/RayCaster.h/.cpp` | `CastRayHWSS` override |
| `src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.h/.cpp` | HWSS mode for PT spectral rasterizer |
| `src/Library/Rendering/BDPTSpectralRasterizer.h/.cpp` | HWSS mode for BDPT spectral rasterizer |
| `src/Library/Shaders/BDPTIntegrator.h/.cpp` | `RecomputeSubpathThroughputNM`, `HasDispersiveDeltaVertex` |
| `src/Library/Rendering/MLTSpectralRasterizer.h/.cpp` | Spectral MLT rasterizer (new) |
| `src/Library/Parsers/AsciiSceneParser.cpp` | `hwss` parameter + `mlt_spectral_rasterizer` chunk |
| `src/Library/RISE_API.h/.cpp` | HWSS params threaded through API |
| `src/Library/Job.h/.cpp`, `src/Library/Interfaces/IJob.h` | HWSS params threaded through job |

### Configuration (Scene File Parameters)

| Parameter | Block | Default | Description |
|-----------|-------|---------|-------------|
| `hwss` | `pixelintegratingspectral_rasterizer` | FALSE | Enable HWSS for PT spectral |
| `hwss` | `bdpt_spectral_rasterizer` | FALSE | Enable HWSS for BDPT spectral |
| `mlt_spectral_rasterizer` | (new block) | — | MLT spectral with built-in HWSS |

### Test Scenes

| Scene | Purpose |
|-------|---------|
| `scenes/Tests/Spectral/hwss_cornellbox_pt.RISEscene` | PT HWSS non-dispersive baseline |
| `scenes/Tests/Spectral/hwss_cornellbox_pt_ref.RISEscene` | PT non-HWSS reference |
| `scenes/Tests/Spectral/hwss_cornellbox_bdpt.RISEscene` | BDPT HWSS non-dispersive baseline |
| `scenes/Tests/Spectral/hwss_cornellbox_bdpt_ref.RISEscene` | BDPT non-HWSS reference |
| `scenes/Tests/Spectral/hwss_prism_dispersion_pt.RISEscene` | PT prism dispersion (companion termination) |
| `scenes/Tests/Spectral/hwss_prism_dispersion_bdpt.RISEscene` | BDPT prism dispersion |
| `scenes/Tests/Spectral/hwss_mlt_spectral_cornellbox.RISEscene` | MLT spectral baseline |

### Tests

- `tests/SampledWavelengthsTest.cpp` — 8 tests covering equidistant spacing, wrap-around, range bounds, PDF values, termination logic, NumActive tracking, edge cases, stratification coverage

### Acceptance Criteria — Met

- ✓ Non-dispersive scenes match current spectral output within noise
- ✓ Dispersive scenes render correctly with companion termination
- ✓ HWSS BDPT ~30% faster than non-HWSS at equal quality (37s vs 54s on Cornell box)
- ✓ All 30 regression tests pass

---

## 6. Blue-Noise Screen-Space Error Distribution (ZSobol) — **DONE** (6A, 6B)

### Implementation Summary

Implemented Morton-indexed Sobol (ZSobol) sampling following Ahmed & Wonka (SIGGRAPH Asia 2020) and pbrt-v4's approach. For pixel (x, y) with per-pixel sample index s, the global Sobol index is computed as `(Morton2D(x,y) << log2(roundUpPow2(SPP))) | s`, ensuring spatially adjacent pixels receive consecutive (0,2)-net sub-sequences. Owen scramble seeds are derived from the Morton index rather than raw coordinates.

#### 6A. Morton-index pixel ordering — DONE

- `src/Library/Utilities/MortonCode.h`: Header-only utility with `Morton2D`, `InverseMorton2D`, `RoundUpPow2`, `Log2Int`
- `tests/MortonCodeTest.cpp`: Round-trip, known-value, Z-order adjacency, and edge-case tests

#### 6B. ZSobol sampler variant — DONE

- `src/Library/Utilities/ZSobolSampler.h`: Inherits `SobolSampler`, remaps sample index via Morton code in constructor. All Get1D/Get2D/StartStream/phase logic inherited unchanged.
- `tests/ZSobolSamplerTest.cpp`: Determinism, Morton reindexing, complementary-sample discrepancy, phase budgeting, overflow guard tests

#### Integration

Modified 14 files to thread `useZSobol` from parser through Job/API to rasterizer constructors:
- Rasterizers: `PixelBasedPelRasterizer`, `BDPTPelRasterizer`, `PixelBasedSpectralIntegratingRasterizer`, `BDPTSpectralRasterizer` (`.h` and `.cpp`)
- `PixelBasedRasterizerHelper.h/.cpp` (added `bool useZSobol` member)
- `RISE_API.h/.cpp`, `IJob.h`, `Job.h/.cpp`, `AsciiSceneParser.cpp`

#### Scene Syntax

Works in all Sobol-based rasterizer blocks: `pixelpel_rasterizer`, `pixelintegratingspectral_rasterizer`, `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`.

```
pixelpel_rasterizer
{
    samples              32
    pixel_sampler        sobol
    blue_noise_sampler   TRUE
}
```

MLT (`mlt_rasterizer`) uses `PSSMLTSampler` and is unaffected. Adaptive and ContrastAA rasterizers do not use `SobolSampler` and are unaffected.

### Validation Results

**Energy conservation**: ZSobol mean pixel values match standard Sobol within 0.03% at all SPP levels. No energy loss.

**Frequency analysis (16 SPP PT, Cornell box)**:
- Low-frequency error power reduced ~5% vs standard Sobol
- High-frequency error power increased ~33% (noise redistributed to less perceptible frequencies)
- Low/High frequency power ratio reduced 28% (9.21 → 6.67)
- At 32 SPP the effect is stronger: L/H ratio reduced 40% (4.72 → 2.82)

**Convergence (512 SPP)**: RMSE between Sobol and ZSobol at 512 SPP is only 0.028 — both converge to the same result.

**Performance overhead**: <2% wall-time increase (7.50s → 7.63s at 16 SPP PT, 1024x1024).

**BDPT**: Works correctly at 16 SPP with no dimension overrun. Metrics match standard Sobol.

**MLT regression**: Renders successfully, completely unaffected (uses `PSSMLTSampler`).

### Test Scenes

`scenes/Tests/Samplers/`: Paired Sobol/ZSobol comparison scenes at 16, 32, 512 SPP (PT), 16 SPP (BDPT), and MLT regression.

---

## 7. Null-Scattering Volume Framework — **DONE** (7A, 7B, 7C, 7E; 7D deferred)

### Why This Is Seventh

RISE has homogeneous and heterogeneous volume support but does not use the null-scattering path integral formulation (Miller, Georgiev, Jarosz, SIGGRAPH 2019). This formulation augments path space with fictitious null-scattering vertices, making the extinction transmittance along each edge trivially evaluable and enabling MIS between different free-flight sampling strategies. It unifies delta tracking, ratio tracking, and spectral tracking under a single framework. This is what pbrt-v4's `VolPathIntegrator` is built on.

This aligns with Roadmap Ranks 5-6 (participating media in PT and BDPT). The recommendation here is to build on the null-scattering formulation from the start rather than implementing ad-hoc tracking methods.

### What To Implement

#### 7A. Majorant grid with DDA traversal

Build a low-resolution grid where each cell stores the maximum density within its region. Decompose rays into segments via 3D-DDA traversal, using only the local majorant for delta/ratio tracking per segment. This dramatically reduces null collisions compared to a global majorant.

#### 7B. Null-scattering path integral

Implement the Miller et al. 2019 formulation: at each sampled point along a ray, make a real-vs-null scattering decision. Null events continue the ray without directional change. The full path PDF is expressible in closed form, enabling MIS between different sampling strategies (e.g., delta tracking vs equiangular sampling).

#### 7C. Ratio tracking for transmittance

Use ratio tracking (Novak et al. 2014) for shadow rays: accumulate multiplicative weights w = prod(1 - mu_t(x_i) / mu_bar) for continuous-valued transmittance estimates instead of delta tracking's binary 0/1. Much lower variance for optically thin media.

#### 7D. Spectral/decomposition tracking

For chromatic volumes with spectral rendering, make per-channel real-vs-null decisions at each interaction point. Combine with HWSS (Improvement 5) by selecting a hero wavelength for distance sampling and applying spectral MIS across the wavelength bundle.

#### 7E. Equiangular sampling for point lights

Implement Kulla and Fajardo (EGSR 2012) equiangular sampling: distribute samples along a ray proportional to the 1/r^2 geometry term toward a point light. Combine with exponential free-flight distance sampling via one-sample MIS.

### Current RISE Files

- `src/Library/Materials/HomogeneousMedium.h` / `.cpp`
- `src/Library/Materials/HeterogeneousMedium.h`
- `src/Library/Utilities/MediumTracking.h`
- `src/Library/Utilities/MediumTransport.h`
- `src/Library/Volume/` (volume data accessors)
- `src/Library/Shaders/PathTracingShaderOp.cpp` (PT medium integration point)

### Deliverables

- Majorant grid data structure with DDA traversal.
- Null-scattering path integral implementation in PT.
- Ratio tracking for shadow rays.
- Equiangular sampling for point lights in media.
- Fog scene and heterogeneous smoke scene.

### Acceptance Criteria

- Homogeneous media results match existing implementation within noise.
- Heterogeneous media converge without excessive null collisions.
- Shadow rays in optically thin media show measurably lower variance with ratio tracking.

### Implementation Notes (April 2026)

**Completed sub-items:**
- **7A**: `MajorantGrid` (`src/Library/Utilities/MajorantGrid.h/.cpp`) — low-resolution 3D grid with Amanatides-Woo DDA traversal via templated visitor pattern. Grid resolution: `max(4, ceil(volDim/8))` per axis, capped at 32. `HeterogeneousMedium` builds the grid in its constructor and uses per-cell local majorants for delta tracking. Significant reduction in null collisions for spatially varying volumes.
- **7B**: `NullScatteringTracker` (`src/Library/Utilities/NullScatteringTracker.h`) — evaluates the delta tracking PDF at arbitrary distances via DDA majorant optical depth accumulation. `IMedium::DistanceSample` struct and `SampleDistanceWithPdf`/`EvalDistancePdf` methods added to the interface with default implementations for `HomogeneousMedium`. `HeterogeneousMedium` overrides these using the majorant grid.
- **7C**: `HeterogeneousMedium::EvalTransmittance` replaced deterministic ray march with ratio tracking using thread-local RNG for stochastic sampling. Per-channel multiplicative weights: `w[ch] *= max(0, 1 - sigma_t[ch] / cellMajorant)`.
- **7E**: `EquiangularSampler` (`src/Library/Utilities/EquiangularSampler.h`) — Kulla-Fajardo 2012 closed-form CDF inversion. One-sample MIS (balance heuristic) in `RayCaster::CastRay` (both RGB and spectral paths) between delta tracking and equiangular sampling toward positional lights. `ILight::IsPositionalLight()` added for type identification; `LightSampler` caches positional light list during `Prepare()`.

**Deferred:**
- **7D** (Spectral/decomposition tracking): Requires HWSS (Improvement 5) for hero wavelength selection. Current spectral path uses single-wavelength delta tracking.

**New files:** `MajorantGrid.h/.cpp`, `NullScatteringTracker.h`, `EquiangularSampler.h`
**Modified interfaces:** `IMedium.h` (DistanceSample, SampleDistanceWithPdf, EvalDistancePdf, GetBoundingBox), `ILight.h` (IsPositionalLight)
**Test scenes:** `pt_equiangular_fog`, `pt_equiangular_hetero`, `pt_chromatic_fog`, `pt_thick_fog_corridor`

---

## 8. Optimal MIS Weights — **DONE** (8A complete; 8B and 8C deferred)

### Why This Is Eighth

RISE uses the balance heuristic (and power heuristic) throughout PT and BDPT. Optimal MIS (Kondapaneni et al., SIGGRAPH 2019) derives variance-minimizing weights by solving a linear system of second moments. For the 2-technique PT direct illumination case, the optimal weights reduce to a closed-form alpha-weighted balance heuristic. Correlation-aware MIS (Grittmann et al., EG/CGF 2021) handles correlated samples in BDPT/VCM but requires solving a full linear system — a naive per-strategy discount breaks the partition-of-unity property and produces biased results.

### Implementation Summary

#### 8A. Optimal MIS for PT direct illumination

Implemented via a tiled second-moment accumulator (`OptimalMISAccumulator`) that runs during a training phase before the main render. Each technique estimates its own second moment from its own samples: NEE sites accumulate `(f/p_nee)²` from NEE-sampled directions, and BSDF-hit sites accumulate `(f/p_bsdf)²` from BSDF-sampled directions. The integrand `f` is the full path integrand `Le * BSDF * cos * V * Tr` (not just emitter radiance), computed as an RGB product scalarized via `MaxValue()`. Per-technique sample counts are maintained separately via an `AccumulateCount`/`Accumulate` split. After training, per-tile optimal alpha values are solved:

    alpha = M_nee / (M_nee + M_bsdf)

where `M_i = mean((f/p_i)²)` averaged over all attempts from technique `i`, including zero-valued misses. This is the second moment `E_{x~p_i}[(f/p_i)²] = ∫ f²/p_i dx` of technique i's single-technique estimator. The technique with lower second moment (lower variance) gets more weight. The rendering phase uses `OptimalMIS2Weight(pa, pb, alpha)` — an alpha-weighted balance heuristic — instead of the power heuristic.

Key design decisions:
- **Full integrand training**: the second moment uses `Le * BSDF * cos * V * Tr`, carried as full RGB `bsdfTimesCos` through `RAY_STATE` and combined component-wise with emitter radiance before scalarization, avoiding decorrelation errors from separate channel maxima
- **AccumulateCount/Accumulate split**: every sampling attempt increments the denominator count (via `AccumulateCount`), while only successful hits add moment (via `Accumulate`). This correctly estimates `E[(f/p)²]` including zero-valued draws from geometry rejections, shadow misses, below-hemisphere environment samples, and Russian roulette terminations
- **Zero-moment fallback**: when a technique has enough attempts but zero accumulated moment (all misses), the solver treats it as having no evidence rather than zero variance, falling back to favor the other technique at the clamp bound
- **RR survival compensation**: BSDF-side `bsdfTimesCos` is computed after Russian roulette, using the post-RR throughput scaled by `1/survivalProb`, so the trained moment matches the actual rendered estimator
- **BSSRDF continuation coverage**: subsurface scattering continuation rays participate in training via `AccumulateCount` before RR and `bsdfTimesCos` after RR, preventing alpha skew in subsurface regions
- **Tiled spatial binning** (default 16×16 pixels per tile) provides spatial adaptivity while maintaining adequate per-tile sample counts
- **Thread-safe accumulation** via `std::atomic<double>` with compare-exchange loops
- **Alpha clamping** to [0.05, 0.95] prevents degenerate weights
- **Fallback** to balance heuristic (alpha=0.5) for tiles with insufficient samples
- **Training piggybacks** on the same rendering infrastructure as path guiding (1 SPP × N iterations), adding minimal overhead
- **Symmetric coverage**: both mesh emitter and environment map paths use optimal weights on NEE and BSDF-hit sides

#### 8B. Correlation-aware MIS for BDPT (deferred)

A proper implementation requires solving the full Grittmann et al. 2021 linear system for all N strategies simultaneously, not a per-strategy discount applied only in the denominator. A naive approach of multiplying each competing strategy's contribution by a correlation discount breaks the partition of unity: each strategy's weight is computed with a different normalization constant, so weights no longer sum to 1 across strategies for a fixed path. This produces biased estimates. Deferred until a correct N-technique implementation can be designed.

#### 8C. Efficiency-aware MIS for BDPT (deferred)

Similarly, scaling each strategy's denominator contribution by an absolute cost factor changes the weight to `w_i = p_i² / sum_j(c_j * p_j²)`, which does not form a partition of unity unless all costs are identical. Correct efficiency-aware weighting requires either a common normalization across all strategies or adjusting sampling rates alongside weights. Deferred.

### Configuration (Scene File Parameters)

All controls are in the rasterizer block and disabled by default:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `optimal_mis` | bool | FALSE | Enable optimal MIS weight training for PT |
| `optimal_mis_training_iterations` | uint | 4 | Number of 1-SPP training passes |
| `optimal_mis_tile_size` | uint | 16 | Tile size for spatial binning |

### Files

| File | Role |
|------|------|
| `src/Library/Utilities/MISWeights.h` | Weight functions: BalanceHeuristic, OptimalMIS2Weight |
| `src/Library/Utilities/OptimalMISAccumulator.h/.cpp` | Tiled second-moment accumulator for training |
| `src/Library/Shaders/PathTracingShaderOp.cpp` | Optimal MIS weights at BSDF-hit sites + training accumulation |
| `src/Library/Rendering/RayCaster.cpp` | Optimal MIS weights at environment BSDF-hit sites + training accumulation |
| `src/Library/Lights/LightSampler.cpp/.h` | Optimal MIS weights at NEE sites + training accumulation |
| `src/Library/Rendering/PixelBasedPelRasterizer.cpp/.h` | Training loop orchestration |
| `src/Library/Utilities/RuntimeContext.h` | OptimalMISAccumulator pointer for PT |
| `src/Library/Utilities/StabilityConfig.h` | Configuration fields |

### Test Scenes

| Scene | Purpose |
|-------|---------|
| `scenes/Tests/PathTracing/optimal_mis_test.RISEscene` | PT with optimal MIS enabled |
| `scenes/Tests/PathTracing/optimal_mis_test_baseline.RISEscene` | PT baseline for comparison |
| `scenes/Tests/BDPT/cornellbox_bdpt_correlation_test.RISEscene` | BDPT glass caustics test |

### Tests

- `tests/MISWeightsTest.cpp` — 6 test groups covering BalanceHeuristic, OptimalMIS2Weight, weight sum property, PowerHeuristic, and edge cases
- `tests/OptimalMISAccumulatorTest.cpp` — 11 tests, 27 assertions covering accumulation, solving, clamping, fallback, count/accumulate split, zero-moment fallback, and thread safety

### Acceptance Criteria — Met

- ✓ Equal or lower variance on all test scenes
- ✓ No bias: converged results match power-heuristic reference
- ✓ MLT continues to work correctly
- ✓ Negligible performance overhead (< 5% wall time increase including training)

---

## Cross-Cutting Infrastructure (April 2026)

These items are not ranked improvements but supporting infrastructure that enables and connects the improvements above.

### PathTracingIntegrator — Iterative PT Engine

`PathTracingIntegrator` (`Shaders/PathTracingIntegrator.h/.cpp`) is a standalone iterative unidirectional path tracer modeled on `BDPTIntegrator`. It uses direct intersection (no shader dispatch), inline material evaluation, and an iterative main loop. This replaces the recursive shader-op dispatch chain for scenes that use the pure PT rasterizers.

**Features**: NEE via LightSampler with MIS, BSDF sampling with MIS-weighted emission, BSSRDF disk-projection and random-walk SSS, SMS for caustics, participating media (delta-tracking), per-type bounce limits, Russian roulette, path guiding (OpenPGL), optimal MIS weight accumulation, environment map MIS.

**Key methods**:
- `IntegrateRay()` — Traces a complete path from a camera ray. Optionally populates a `PixelAOV` for the denoiser.
- `IntegrateFromHit()` — Starts from a pre-computed `RayIntersection`. Both `IntegrateRay` and the `PathTracingShaderOp` wrapper delegate to this.
- `IntegrateRayNM()` / `IntegrateRayHWSS()` — Single-wavelength and hero-wavelength spectral variants.
- `IntegrateFromHitNM()` / `IntegrateFromHitHWSS()` — Pre-hit spectral variants for ShaderOp wrapper use.

### Pure PT Rasterizers

Two new rasterizers bypass the shader-op pipeline entirely, calling `PathTracingIntegrator` directly:

| Rasterizer | Base class | Scene chunk |
|-----------|-----------|------------|
| `PathTracingPelRasterizer` | `PixelBasedPelRasterizer` | `pathtracing_pel_rasterizer` |
| `PathTracingSpectralRasterizer` | `PixelBasedSpectralIntegratingRasterizer` | `pathtracing_spectral_rasterizer` |

Both inherit the standard pixel-based sample loop (Sobol/ZSobol, adaptive sampling, path guiding training, optimal MIS training) from their base classes. The Pel variant supports per-sample AOV accumulation for OIDN. The spectral variant supports both per-wavelength (NM) and hero wavelength (HWSS) modes.

**Scene syntax** (same parameters as existing rasterizers plus SMS controls):
```
pathtracing_pel_rasterizer
{
    samples                 64
    oidn_denoise            TRUE
    pathguiding             true
    pathguiding_sampling_type RIS
    sms_enabled             true
    adaptive_max_samples    256
    adaptive_threshold      0.01
    # all stability controls (direct_clamp, indirect_clamp, etc.)
}

pathtracing_spectral_rasterizer
{
    samples                 64
    spectral_samples        16
    use_hwss                TRUE
    sms_enabled             true
    # all stability controls
}
```

### FilteredFilm — Wide-Support Pixel Filter Reconstruction

`FilteredFilm` (`Rendering/FilteredFilm.h/.cpp`) accumulates filter-weighted sample contributions across pixel boundaries. Each sample is splatted to all pixels within the pixel filter's support. After the render pass, `Resolve()` computes the final pixel value as `colorSum/weightSum`.

`PixelBasedRasterizerHelper::UseFilteredFilm()` returns true when the pixel filter's support exceeds half a pixel. Since the default pixel filter is Mitchell-Netravali (support 2.0), the film is active by default for all multi-sample pixel-based rasterizers.

**Important**: The filtered film resolve is skipped when OIDN denoising is enabled. See `docs/ARCHITECTURE.md` for details on why.

### Per-Sample AOV Accumulation For OIDN

`AOVBuffers` (`Rendering/AOVBuffers.h/.cpp`) stores first-hit albedo and normal data for the OIDN denoiser. Previously only BDPT populated AOVs per-sample during rendering. Now:

- `PixelAOV` struct (defined in `AOVBuffers.h`) is shared between PT and BDPT
- `PathTracingIntegrator::IntegrateRay()` accepts an optional `PixelAOV*` parameter and populates first-hit albedo (via `BSDF::value() * PI`) and normal after the camera ray intersection
- `PathTracingPelRasterizer::IntegratePixel()` accumulates per-sample AOVs into `pAOVBuffers` with proper weight normalization
- `pAOVBuffers` member is owned by `PixelBasedRasterizerHelper` (was previously only in BDPT base)
- `AOVBuffers::HasData()` tracks whether any per-sample data was accumulated; if false after the render pass, `OIDNDenoiser::CollectFirstHitAOVs()` fires a separate retrace pass as fallback

### Files

| File | Role |
|------|------|
| `src/Library/Shaders/PathTracingIntegrator.h/.cpp` | Iterative PT integrator (new) |
| `src/Library/Rendering/PathTracingPelRasterizer.h/.cpp` | Pure PT Pel rasterizer (new) |
| `src/Library/Rendering/PathTracingSpectralRasterizer.h/.cpp` | Pure PT Spectral rasterizer (new) |
| `src/Library/Rendering/FilteredFilm.h/.cpp` | Wide-support pixel filter film buffer (new) |
| `src/Library/Rendering/AOVBuffers.h/.cpp` | Shared AOV types and buffers (modified) |
| `src/Library/Rendering/PixelBasedRasterizerHelper.h/.cpp` | Film allocation, OIDN bypass, AOV ownership (modified) |
| `src/Library/Rendering/PixelBasedPelRasterizer.cpp` | Film splatting in sample loop (modified) |
| `src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.cpp` | Film splatting in spectral loop (modified) |
| `src/Library/Rendering/BDPTRasterizerBase.h/.cpp` | AOV ownership moved to base (modified) |
| `src/Library/Shaders/PathTracingShaderOp.h/.cpp` | Now delegates to PathTracingIntegrator via IntegrateFromHit (modified) |
| `src/Library/Shaders/BSSRDFEntryAdapters.h` | Shared BSSRDF entry-point adapters (new) |
| `src/Library/Job.cpp` | Rasterizer construction (modified) |
| `src/Library/Parsers/AsciiSceneParser.cpp` | New chunk parsers (modified) |
| `src/Library/RISE_API.h/.cpp` | Public construction API (modified) |
| `build/make/rise/Filelist` | Build file list (modified) |

---

## 9. VCM (Vertex Connection And Merging) — DONE

Landed as `VCMIntegrator` + `VCMPelRasterizer` (RGB) + `VCMSpectralRasterizer` (HWSS), reusing BDPT's subpath generators via a single-pass post-walk that produces the SmallVCM `(dVCM, dVC, dVM)` quantities.  Balance-heuristic MIS; SPPM-style progressive radius shrinkage with adaptive density floor; parallel light pass via tile-based dispatcher.  Full implementation notes, MIS formulas, regression scenes, and known limitations live in [docs/VCM.md](VCM.md).

### Original specification

The original specification is preserved below for reference.

---

### Why This Was Ninth

RISE already has both BDPT and photon mapping. VCM (Georgiev et al., SIGGRAPH Asia 2012) unifies them under MIS, subsuming PT, LT, BDPT, and photon mapping as special cases. The marginal implementation effort is lower than for most renderers since both halves already exist. VCM provides robust caustic rendering without relying solely on SMS.

### What To Implement

#### 9A. Area-measure photon density estimation PDF

Reformulate photon density estimation as a bidirectional sampling technique with an explicit PDF in area-product measure. This is the key insight from VCM: photon merging becomes just another technique in the MIS framework.

#### 9B. Recursive MIS weight computation

Track three running values per vertex (dVCM, dVC, dVM) as described in Georgiev's SmallVCM. These accumulate the information needed to compute MIS weights for all connection and merging strategies.

#### 9C. Integration with existing BDPT

Extend the existing `MISWeight()` to include the vertex merging PDF terms alongside the current vertex connection terms.

### Reference Implementation

SmallVCM (smallvcm.com) is a ~2000-line C++ educational implementation covering PT, LT, PPM, BPM, BDPT, and VCM. Use as the primary reference.

### Current RISE Files

- `src/Library/Shaders/BDPTIntegrator.h` / `.cpp` (BDPT core)
- `src/Library/PhotonMapping/` (photon map infrastructure)

### Deliverables

- VCM integrator extending BDPT with vertex merging.
- Caustic scene comparison: VCM vs BDPT vs photon mapping vs SMS.

### Acceptance Criteria

- Caustic scenes converge faster than BDPT alone.
- Non-caustic scenes perform comparably to BDPT (merging adds overhead but MIS should prevent regression).

---

## 10. Hair/Fiber BSDF (Chiang Et Al. 2016)

### Why This Is Tenth

RISE has no dedicated hair or fiber BSDF. The Chiang et al. model (Disney/Hyperion, also used in pbrt-v4 and Cycles) is the production standard. It builds on Marschner et al. (2003) with near-field formulation, a single residual lobe for all higher-order internal reflections, and logistic distributions for azimuthal roughness with closed-form CDF. This is a self-contained material addition.

### What To Implement

#### 10A. Longitudinal scattering (M lobes)

Implement the R, TT, TRT, and residual longitudinal scattering functions using shifted Gaussian distributions parameterized by roughness.

#### 10B. Azimuthal scattering (N lobes)

Implement azimuthal scattering using logistic distributions with closed-form CDF for importance sampling.

#### 10C. Near-field formulation

Use the true fiber offset (h parameter) rather than width-averaged far-field approximation. This matters for close-up rendering.

#### 10D. Importance sampling

Sample the combined longitudinal x azimuthal distribution. The logistic CDF enables exact inversion for the azimuthal component.

### Current RISE Files

- `src/Library/Interfaces/ISPF.h` (interface to implement)
- `src/Library/Interfaces/IBRDF.h`
- `src/Library/Materials/` (add new files here)

### Deliverables

- Hair BSDF with R, TT, TRT, and residual lobes.
- Importance sampling with logistic azimuthal inversion.
- Hair rendering test scene (straight and curved fibers under directional light).
- Parser support for hair material parameters.

### Acceptance Criteria

- White furnace test passes for the hair BSDF.
- Importance sampling PDF matches BSDF evaluation (chi-squared test).
- Visual comparison with pbrt-v4 hair reference images.

---

## 11. Jakob-Hanika Sigmoid Spectral Uplifting

### Why This Is Eleventh

The mapping from RGB to spectral reflectance is underdetermined. The Jakob and Hanika (CGF/Eurographics 2019) sigmoid method parameterizes spectra as S(lambda) = sigmoid(c0 * lambda^2 + c1 * lambda + c2), intrinsically bounded in [0,1] and smooth. A precomputed 3D lookup table maps RGB to three coefficients. Evaluation costs ~6 FLOPs per wavelength. Zero round-trip error on the full sRGB gamut. This is the standard used by both pbrt-v4 and Mitsuba 3.

This is most valuable in combination with HWSS (Improvement 5) since the sigmoid coefficients replace RGB texels directly (three floats for three floats).

### What To Implement

#### 11A. Precomputed coefficient table

Generate or import the 3D LUT mapping sRGB to sigmoid coefficients. The tables are publicly available from Mitsuba 3 and pbrt-v4.

#### 11B. Texture load-time conversion

At texture load time, convert RGB texels to sigmoid coefficients. Store as three floats per texel (same memory footprint as RGB).

#### 11C. Runtime evaluation

At shading time, evaluate S(lambda) = sigmoid(c0 * lambda^2 + c1 * lambda + c2) at the sampled wavelengths. ~6 FLOPs per wavelength per texel lookup.

### Current RISE Files

- `src/Library/Utilities/Color/SpectralPacket.h` (spectral evaluation infrastructure)
- `src/Library/Texturing/` (texture loading and evaluation)

### Deliverables

- Sigmoid coefficient LUT (imported or generated).
- Texture conversion pipeline.
- Runtime spectral evaluation from sigmoid coefficients.

### Acceptance Criteria

- Round-trip error: RGB -> sigmoid -> spectrum -> RGB produces the original RGB within floating-point tolerance.
- No visible artifacts under non-D65 illumination.
- Overhead vs direct RGB evaluation is negligible.

---

## Explicit Non-Goals

These are interesting but should not displace the ranked items above:

- **Neural importance sampling:** OpenPGL path guiding with RIS is already the practical production answer. NIS requires dedicated GPU inference and offers worse performance-to-overhead ratios.
- **NeRF / 3D Gaussian Splatting:** Scene acquisition tools, not rendering improvements.
- **Polarization:** 1.5-2x overhead, invisible to humans in standard rendering. Only if a specific scientific visualization need arises.
- **Full MLT overhaul (RJMCMC, delayed rejection):** PSSMLT is already implemented. Marginal gains do not justify the complexity for most scenes.
- **GPU / wavefront execution:** Feature gaps come first, not execution-model gaps.
- **Neural BRDFs / neural radiance caching:** Niche applications (measured material compression, interactive preview). Not a priority for reference-quality offline rendering.

---

## Context On Prior Work

Several items here build on transport work already completed or planned before this document was written:

| Item | Prior status |
|------|-------------|
| Light BVH (Rank 4) | **Done** (April 2026) — 4A, 4B, 4C implemented; 4.78x variance reduction on 100-light corridor |
| Null-scattering volumes (Rank 7) | **Done** (April 2026) — 7A, 7B, 7C, 7E implemented; 7D deferred pending HWSS |
| Light subpath guiding (Rank 2) | New scope |
| Random-walk SSS (Rank 3) | **Done** (April 2026) |

The following validation and correctness work from the prior roadmap remains relevant before starting items that depend on light sampling or spectral correctness:
- Validation harness: focused scenes for many-light, caustic, BSSRDF, and fog transport.
- Light sampling unification: shared sampled-light abstraction across PT, BDPT, and SMS.
- Spectral/SMS correctness fixes: non-mesh light spectral path, SMS visibility, unbiased RR in PT.
- Production stability controls: per-type bounce limits, direct/indirect clamps, glossy filtering.
