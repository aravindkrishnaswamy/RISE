# Test Guide

The tests in this repository are standalone executables built from `tests/*.cpp`. They are not managed by a unit test framework.

## Build And Run

```sh
make -C build/make/rise tests
./run_all_tests.sh
```

Build behavior comes from [../build/make/rise/Makefile](../build/make/rise/Makefile). The makefile glob picks up every `tests/*.cpp` file automatically and links it against the core library.

Built binaries land in `bin/tests/`.

## Current Test Inventory

- `ClippedPlaneGeometryTest.cpp`: clipped plane geometry behavior
- `FinalGatherShaderOpTest.cpp`: final gather interpolation helpers and stability logic
- `IrradianceCacheTest.cpp`: irradiance cache behavior
- `OpticsTest.cpp`: optics utilities
- `PrimesTest.cpp`: prime-related utilities

## Style Of Test Used Here

- Each file is an executable with its own `main`.
- Assertions are usually plain `assert(...)`.
- Helpful progress text is printed with `std::cout`.
- The best targets are deterministic helpers, math utilities, cache logic, and other focused behavior that does not require comparing full rendered images.

## Adding A New Test

1. Add a new `tests/<Name>.cpp` file.
2. Include the minimal headers you need from `src/Library`.
3. Keep the test deterministic and fast.
4. Use `assert` for pass/fail checks.
5. Build with `make -C build/make/rise tests`.
6. Run with `./run_all_tests.sh`.

No makefile edit is needed for a new `tests/*.cpp` file because the existing wildcard-based rule discovers it automatically.

## Transport Correctness Scenes (Roadmap Step 2)

These scenes validate the spectral, SMS, and Russian roulette correctness fixes from `docs/PATH_TRANSPORT_ROADMAP.md` Step 2. They require visual or statistical comparison rather than deterministic assertions.

### Russian Roulette (2A)

```sh
echo "render" | bin/rise scenes/Tests/RussianRoulette/cornellbox_highalbedo_pt.RISEscene
echo "render" | bin/rise scenes/Tests/RussianRoulette/cornellbox_highalbedo_bdpt.RISEscene
```

**Expected**: PT and BDPT produce images of comparable brightness. The high-albedo (0.9) walls amplify any bias in path termination. The PT scene uses `min_importance 0.0` to ensure the old biased cutoff does not mask the RR behavior. Compare mean luminance; PT should be within 5% of BDPT.

### Spectral Non-Mesh Lights (2B)

```sh
echo "render" | bin/rise scenes/Tests/SpectralLights/cornellbox_pointlight_spectral.RISEscene
```

**Expected**: The scene is illuminated (not black). Before the fix, spectral rendering with point lights produced a completely black image because `EvaluateDirectLightingNM` skipped non-mesh lights.

### SMS Visibility (2D)

```sh
echo "render" | bin/rise scenes/Tests/SMS/sms_visibility_unoccluded.RISEscene
echo "render" | bin/rise scenes/Tests/SMS/sms_visibility_occluded.RISEscene
```

**Expected**: The unoccluded scene shows a caustic beneath the glass sphere. The occluded scene blocks the caustic with an opaque wall. Note: inter-specular visibility (occluders between glass vertices) is not checked; see `ManifoldSolver::CheckChainVisibility` documentation.

### SMS Spectral Regression

```sh
echo "render" | bin/rise scenes/FeatureBased/SpectralRendering/spectral_dispersive_caustic_pt_sms.RISEscene
```

**Expected**: Dispersive glass caustic with per-wavelength evaluation. The sphere should show a slight chromatic tint from dispersion.

### Output Location

All renders write to `rendered/`. File names match the scene file base name.

## Relationship To Sample Scenes

- Use `tests/` for deterministic logic and small subsystem checks.
- Use `scenes/FeatureBased/` for authored end-to-end coverage of parser and renderer features.
- Use `scenes/Tests/` for transport correctness validation that requires image comparison.
- If a feature is user-visible and deterministically testable, it usually deserves both.
