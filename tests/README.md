# Test Guide

The tests in this repository are standalone executables built from `tests/*.cpp`. They are not managed by a unit test framework.

Scene-based validation taxonomy now lives in [../scenes/README.md](../scenes/README.md) and [../scenes/Tests/README.md](../scenes/Tests/README.md).

## Build And Run

```sh
make -C build/make/rise tests
./run_all_tests.sh
```

Build behavior comes from [../build/make/rise/Makefile](../build/make/rise/Makefile). The makefile glob picks up every `tests/*.cpp` file automatically and links it against the core library.

Built binaries land in `bin/tests/`.

## Current Test Inventory

- Geometry and intersection:
  `BSPMailboxingTest.cpp`, `BSPTreeSAHTest.cpp`, `BezierClippingUnitsTest.cpp`, `CSGObjectIdentityTest.cpp`, `ClippedPlaneGeometryTest.cpp`, `DisplacedGeometryTest.cpp`, `GeometrySurfaceDerivativesTest.cpp`, `RISEMeshLegacyBSPCompatibilityTest.cpp`, `RayBoxIntersectionTest.cpp`, `RayTriangleIntersectionTest.cpp`, `TessellatedShapeDerivativesTest.cpp`
- Math and utility coverage:
  `ColorUtilsTest.cpp`, `GeometricUtilitiesTest.cpp`, `Math3DTest.cpp`, `MortonCodeTest.cpp`, `OpticsTest.cpp`, `PolynomialTest.cpp`, `PrimesTest.cpp`
- Noise and procedural-texture generators:
  `CurlNoise3DTest.cpp`, `DomainWarpNoise3DTest.cpp`, `GaborNoise3DTest.cpp`, `PerlinWorleyNoise3DTest.cpp`, `ReactionDiffusion3DTest.cpp`, `SDFPrimitive3DTest.cpp`, `SimplexNoise3DTest.cpp`, `TurbulenceNoise3DTest.cpp`, `WaveletNoise3DTest.cpp`, `WorleyNoise3DTest.cpp`
- Sampling, shading, and cache behavior:
  `BSSRDFEntryPointTest.cpp`, `BSSRDFSamplingTest.cpp`, `FinalGatherShaderOpTest.cpp`, `GGXWhiteFurnaceTest.cpp`, `IORStackBehaviorTest.cpp`, `IORStackTest.cpp`, `IrradianceCacheTest.cpp`, `LightExitanceTest.cpp`, `RandomWalkSSSTest.cpp`, `SPFBSDFConsistencyTest.cpp`, `SPFPdfConsistencyTest.cpp`
- Light sampling and volumes:
  `LightBVHTest.cpp`, `MajorantGridTest.cpp`, `PainterVolumeAccessorTest.cpp`
- MIS and integrator building blocks:
  `MISWeightsTest.cpp`, `OptimalMISAccumulatorTest.cpp`, `PathValueOpsTest.cpp`
- Spectral support:
  `SampledWavelengthsTest.cpp`, `SpectralValueTraitsTest.cpp`
- SMS (Specular Manifold Sampling):
  `ManifoldSolverTest.cpp` (block-tridiagonal Jacobian, specular-direction math, chain geometry/throughput, TIR, light-to-first-vertex Jacobian determinant), `BDPTSMSSuppressionTest.cpp` (cross-strategy emission suppression for BDPT (s==0) to prevent SMS caustic double-counting)
- VCM (Vertex Connection and Merging):
  `VCMEyePostPassTest.cpp`, `VCMLightPostPassTest.cpp`, `VCMLightVertexStoreTest.cpp`, `VCMRecurrenceTest.cpp`, `VCMSpectralRecurrenceTest.cpp`
- Sampler and dimension budget:
  `MMLTStrategySelectionTest.cpp`, `PSSMLTStreamAliasingTest.cpp`, `SobolDimensionBudgetTest.cpp`, `ZSobolSamplerTest.cpp`
- Image comparison, diagnostics, and instrumentation:
  `RasterSanityScanTest.cpp`, `RenderETAEstimatorTest.cpp`
  (CLI diagnostic utilities like `ImageDiffTest`, `FindFireflyTest`,
  `ExrRegionCompareTest`, and `ExrFireflyInspect` live in `tools/`,
  not here, because they require file arguments and are not
  assertion-based tests.)
- Performance-oriented coverage:
  `MailboxingPerformanceTest.cpp`

## Style Of Test Used Here

- Each file is an executable with its own `main`.
- Assertions are usually plain `assert(...)`.
- Helpful progress text is printed with `std::cout`.
- The best targets are deterministic helpers, math utilities, cache logic, and other focused behavior that does not require comparing full rendered images.
- Ignored `*.o` files or `* 2.o` files under `tests/` are local build artifacts, not source-of-truth tests.

## Adding A New Test

1. Add a new `tests/<Name>.cpp` file.
2. Include the minimal headers you need from `src/Library`.
3. Keep the test deterministic and fast.
4. Use `assert` for pass/fail checks.
5. Build with `make -C build/make/rise tests`.
6. Run with `./run_all_tests.sh`.

No makefile edit is needed for a new `tests/*.cpp` file because the existing wildcard-based rule discovers it automatically.

## Transport Correctness Scenes (Roadmap Step 2)

These scenes validate spectral, SMS, and Russian roulette correctness. They require visual or statistical comparison rather than deterministic assertions.

Assume `RISE_MEDIA_PATH` is set to the repo root before running any of the scene-based checks below:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
```

### Russian Roulette (2A)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/RussianRoulette/cornellbox_highalbedo_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/RussianRoulette/cornellbox_highalbedo_bdpt.RISEscene
```

**Expected**: PT and BDPT produce images of comparable brightness. The high-albedo (0.9) walls amplify any bias in path termination. Russian roulette controls path termination; compare mean luminance — PT should be within 5% of BDPT.

### Spectral Non-Mesh Lights (2B)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/Spectral/cornellbox_pointlight_spectral.RISEscene
```

**Expected**: The scene is illuminated (not black). Before the fix, spectral rendering with point lights produced a completely black image because `EvaluateDirectLightingNM` skipped non-mesh lights.

### SMS Visibility (2D)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_visibility_unoccluded.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_visibility_occluded.RISEscene
```

**Expected**: The unoccluded scene shows a caustic beneath the glass sphere. The occluded scene blocks the caustic with an opaque wall. Note: inter-specular visibility (occluders between glass vertices) is not checked; see `ManifoldSolver::CheckChainVisibility` documentation.

### SMS Spectral Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/Spectral/spectral_dispersive_caustic_bdpt_sms.RISEscene
```

**Expected**: Dispersive glass caustic with per-wavelength evaluation. The sphere should show a slight chromatic tint from dispersion. The BDPT variant validates that `EvaluateSMSStrategiesNM` delegates correctly to `ManifoldSolver::EvaluateAtShadingPointNM` (G(x,v_1) · |det(δv_1/δy)| geometry) — a regression to the obsolete `cosAtLight · chainGeom / jacobianDet` formula would appear as a spectral tint shift on the caustic versus the PT reference.

### BDPT + SMS Double-Counting Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_vcm.RISEscene       # reference
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_bdpt_sms.RISEscene   # unit under test
```

**Expected**: The BDPT+SMS render's caustic mean luminance should match the VCM reference within ~5%. Before the (s==0) suppression fix in `BDPTIntegrator::ConnectAndEvaluate`, BDPT+SMS would render caustics ~1.5–2× brighter than VCM because (s==0) BSDF-sampled emission and the SMS estimator both contributed at full weight for the same path space. The fix uses `BDPTIntegrator::ShouldSuppressSMSOverlap` (covered by `tests/BDPTSMSSuppressionTest.cpp`) to mirror PT's `bPassedThroughSpecular && bHadNonSpecularShading` rule over the eye subpath.

## Production Stability Controls (Roadmap Step 3)

These scenes validate the production stability controls. Each control is disabled by default (zero or UINT_MAX) so existing scenes are unaffected.

### Sample Clamping (3A)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_baseline_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_active_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_active_bdpt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_active_spectral_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_active_spectral_bdpt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/clamp_indirect_pt.RISEscene
```

**Expected**: The baseline (no clamps) shows firefly noise from the small bright light (scale 800). The clamped variants (`direct_clamp 10`, `indirect_clamp 5`) suppress fireflies with only mild darkening. The PT clamped scene also outputs HDR for quantitative comparison. The spectral PT and spectral BDPT variants validate that stability controls propagate through the `pixelintegratingspectral_rasterizer` and `bdpt_spectral_rasterizer` pipelines respectively. Spectral scenes use the reference Cornell box spectral painters and standard light (scale 10) with higher clamp values (`direct_clamp 500`, `indirect_clamp 250`) because single-wavelength scalar radiance is larger than per-channel RGB — CIE conversion and sample averaging happen after clamping. The `clamp_indirect_pt` scene uses a glass sphere and gold sphere to drive multi-bounce indirect paths that exercise `indirect_clamp` more heavily than the basic Cornell box.

### Russian Roulette Tuning (3C)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/rr_baseline_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/rr_tuning_pt.RISEscene
```

**Expected**: The baseline uses default RR settings (rr_min_depth 3, rr_threshold 0.05). The tuned variant uses `rr_min_depth 1` and `rr_threshold 0.5` for aggressive path termination. The tuned image should be noticeably noisier than the baseline but correctly illuminated. High-albedo walls (0.9) amplify any RR bias.

### Per-Type Bounce Limits (3D)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/bounce_limits_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/bounce_limits_bdpt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/bounce_limits_glossy_translucent_pt.RISEscene
```

**Expected**: Glass sphere renders with full transmission (`max_transmission_bounce 50`) while diffuse inter-reflection is limited (`max_diffuse_bounce 2`). The ceiling should appear slightly darker than unlimited bounces. The sphere should remain clear and refractive. PT and BDPT variants should produce comparable results. The `bounce_limits_glossy_translucent_pt` scene adds a glossy gold sphere (`max_glossy_bounce 2`) and two nested translucent spheres (`max_translucent_bounce 2`, `scattering 0.9`) to exercise the glossy and translucent bounce counters. The high scattering coefficient ensures back-face translucent events fire, and the nested geometry forces 4+ translucent bounces per through-path so the limit of 2 actively engages.

### Glossy Filtering (3B)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/glossy_filter_baseline_pt.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/StabilityControls/glossy_filter_pt.RISEscene
```

**Expected**: Both scenes feature a gold metallic sphere (Cook-Torrance, facets 0.15). The baseline has no glossy filtering. The filtered variant (`filter_glossy 0.5`) should show slightly softened secondary reflections from the sphere onto walls. The direct specular highlight on the sphere itself should be unaffected (filtering only applies after the first glossy bounce).

### Scene File Keywords

All keywords below are valid in `pixelpel_rasterizer`, `pixelintegratingspectral_rasterizer`, `bdpt_pel_rasterizer`, and `bdpt_spectral_rasterizer` blocks (except `filter_glossy` which is PT-only):

```
direct_clamp        10.0    # 0 = disabled (default)
indirect_clamp      5.0     # 0 = disabled (default)
filter_glossy       0.5     # 0 = disabled (default); PT blocks only
rr_min_depth        3       # default: 3
rr_threshold        0.05    # default: 0.05
max_diffuse_bounce       4       # default: unlimited
max_glossy_bounce        8       # default: unlimited
max_transmission_bounce  32      # default: unlimited
max_translucent_bounce   8       # default: unlimited
```

### Known Limitations

- **Glossy filtering + BDPT**: The `filter_glossy` control only affects the PT shader path. BDPT connection evaluations construct synthetic intersection records and do not carry the accumulated filter width, so glossy filtering has no effect on BDPT-evaluated strategies.
- **Spectral clamp scaling**: Spectral rasterizers clamp single-wavelength scalar radiance values before CIE color matching function multiplication and sample averaging. This means the same clamp threshold produces a much stronger effect in spectral mode than in RGB mode. Spectral scenes should use clamp values roughly 10–20× higher than equivalent RGB scenes.

### Output Location

All renders write to `rendered/`. File names match the scene file base name.

## BSSRDF Furnace Tests (Energy Conservation)

These scenes validate that the BSSRDF subsurface scattering implementation conserves energy. They use a large sphere (R=10, ~40x mean free path) in a uniform emissive box so the geometry approaches the flat-slab limit where analytical predictions are available.

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_absorption.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_zero_absorption.RISEscene
```

**Output format**: HDR in ROMMRGB_Linear (no color space conversion, enabling accurate per-channel ratio analysis).

**Verification procedure**:
1. Render both scenes
2. Measure sphere center vs background corner pixel values in each HDR image
3. Compute `ratio_abs = sphere/bg` for the absorption scene and `ratio_zero = sphere/bg` for the zero-absorption scene
4. The corrected ratio `ratio_abs / ratio_zero` should match the flat-slab prediction within 1%:
   - Red: 0.995, Green: 0.872, Blue: 0.672
5. The zero-absorption scene should have equal ratios across all channels (~0.96, deficit from probe failures/recursion limits)

**What this catches**: Any regression in BSSRDF weight computation, Fresnel handling, profile evaluation, or importance sampling PDF that would break energy conservation.

**Companion unit test**: `tests/BSSRDFSamplingTest.cpp` tests the same properties deterministically without rendering (profile normalization, sampling consistency, Fresnel conservation, Sw normalization, weight formula correctness, flat-slab energy balance).

## Path Guiding RIS Regression (Roadmap Stage 8)

The script `tests/test_ris_regression.sh` is an automated regression test for the RIS path guiding implementation.  It renders a Cornell box at 128×128 / 64 SPP with both RIS and one-sample MIS guiding, then compares mean luminance, firefly counts, and floor variance.

```sh
bash tests/test_ris_regression.sh
```

**Thresholds:**
- Luminance difference < 5% (energy conservation).
- RIS fireflies < 3× MIS fireflies + 10 (no firefly regression).
- RIS floor variance ratio < 2.0 (no variance explosion).

**Exit code:** 0 on pass, 1 on failure.

**Requirements:** RISE binary (`bin/rise`), Python 3 with Pillow and numpy.  Optional: scipy for neighbor-aware firefly detection.

The script generates a minimal Cornell box scene on the fly, so it does not depend on any checked-in scene files.  Temporary files are cleaned up on exit.

### Related Scenes

- `scenes/Tests/PathTracing/pt_guiding_stress_ris.RISEscene` — small-opening stress test with RIS guiding.
- `scenes/Tests/PathTracing/pt_indirect_test_ris.RISEscene` — indirect-only Cornell box with RIS guiding.
- `scenes/FeatureBased/BDPT/bdpt_jewel_vault_ris.RISEscene` — BDPT jewel vault with RIS guiding.
- `scenes/FeatureBased/PathTracing/pt_jewel_vault_guided.RISEscene` — PT jewel vault with RIS guiding.

### Adaptive Alpha

The guiding alpha is adaptively scaled using a variance-aware approach inspired by Rath et al. 2020.  The coefficient of variation (CoV) of indirect sample energy determines how much the guiding distribution helps.  An alternative Cycles-style approach (using `sqrt(indirectFraction)`) was also tested — see the inline comments in `src/Library/Rendering/PixelBasedPelRasterizer.cpp` and `BDPTRasterizerBase.cpp` for how to switch between approaches.

## Relationship To Sample Scenes

- Use `tests/` for deterministic logic and small subsystem checks.
- Use `scenes/FeatureBased/` for curated showcase and torture scenes.
- Use `scenes/Tests/` for isolated regression, comparison, and image-validation scenes.
- If a feature is user-visible and deterministically testable, it usually deserves both.
