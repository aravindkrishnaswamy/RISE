# Test Guide

The tests in this repository are standalone executables built from `tests/*.cpp`. They are not managed by a unit test framework.

Scene-based validation taxonomy now lives in [../scenes/README.md](../scenes/README.md) and [../scenes/Tests/README.md](../scenes/Tests/README.md).

## Build And Run

### Linux / macOS

```sh
make -C build/make/rise tests
./run_all_tests.sh
```

Build behavior comes from [../build/make/rise/Makefile](../build/make/rise/Makefile). The makefile glob picks up every `tests/*.cpp` file automatically and links it against the core library.

### Windows

```powershell
# One-time configure:
cmake -S build/cmake/rise-tests -B build/cmake/rise-tests/_out -A x64

# Prereq: build the Library + CLI from build/VS2022/RISE.sln in both Debug
# and Release once so bin/RISE.lib and dbin/RISE.lib exist.
.\run_all_tests.ps1                              # Release
.\run_all_tests.ps1 -Config Debug                # Debug
.\run_all_tests.ps1 -Filter Math3DTest,*Noise3D* # Subset by wildcard
.\run_all_tests.ps1 -TimeoutSeconds 60           # Kill any test exceeding 60s
```

Build behavior comes from [../build/cmake/rise-tests/CMakeLists.txt](../build/cmake/rise-tests/CMakeLists.txt). CMake globs every `tests/*.cpp` into its own per-test executable, links against the existing `RISE.lib` produced by the VS2022 Library project, and stages the OpenEXR + OIDN runtime DLLs alongside each test exe.

Built binaries land in `bin/tests/` (Release) or `dbin/tests/` (Debug).

## Test Map

There are 219 standalone `tests/*.cpp` executables as of 2026-07-24. Do not
maintain a hand-counted filename inventory here: the CST, editor, GUI,
FrameStore, agent, and eval work adds tests often enough that such lists become
wrong within days. The filesystem and build globs are authoritative:

```sh
rg --files tests -g '*Test.cpp' | sort
find tests -maxdepth 1 -type f -name '*.cpp' | wc -l
```

Useful filename families:

- `Agent*Test.cpp`: agent session, chat loop, MCP/stdio/HTTP transports,
  autonomy, proposals, renders, viewport reads, trajectories, and evals
- `Cst*Test.cpp`: lossless parsing, derive contracts, minimal-diff edits,
  identity, reference graphs, scene variants, incremental apply, and cost gates
- `SceneEditor*Test.cpp`, `Viewport*Test.cpp`, `SourceTraceTest.cpp`, and
  `EntityTemplatesTest.cpp`: editing, persistence, source traceability,
  render modes, N-up panes, and entity creation
- `FrameStore*Test.cpp`, `FrameEncoderTest.cpp`, and
  `ViewportFrameStoreTest.cpp`: framebuffer storage, color math, encoders, and
  GUI delivery
- `AutoRasterizerTest.cpp`, `BDPT*Test.cpp`, `VCM*Test.cpp`,
  `ManifoldSolverTest.cpp`, and the `*Spectral*Test.cpp` family: transport,
  MIS, SMS, and spectral behavior
- geometry, materials, painters, samplers, volumes, importers, color, and
  utility code use descriptive subsystem prefixes

CLI diagnostics and data-processing programs that require file arguments live
under `tools/`; they are not assertion-based `run_all_tests` executables.

## Style Of Test Used Here

- Each file is an executable with its own `main`.
- Assertions are usually plain `assert(...)`.
- Helpful progress text is printed with `std::cout`.
- The best targets are deterministic helpers, math utilities, cache logic, and other focused behavior that does not require comparing full rendered images.
- For procedural / noise tests, separate **exact contract checks** from **sampled-difference heuristics**. Put exact identities first in `main()` and label the weaker sampled-difference checks clearly so future readers do not mistake them for strong oracles.
- Prefer exact identities such as periodicity, parameter collapse (`blend=0`, `warpAmplitude=0`, `persistence=0`), symmetry, sign behavior, and simple analytic reference points before adding "different settings produce different outputs" checks.
- Ignored `*.o` files or `* 2.o` files under `tests/` are local build artifacts, not source-of-truth tests.

## Adding A New Test

1. Add a new `tests/<Name>.cpp` file.
2. Include the minimal headers you need from `src/Library`.
3. Keep the test deterministic and fast.
4. Use `assert` for pass/fail checks.
5. Build with `make -C build/make/rise tests` on Linux/macOS, or `cmake --build build/cmake/rise-tests/_out --config Release --target rise_all_tests --parallel` on Windows.
6. Run with `./run_all_tests.sh` on Linux/macOS, or `.\run_all_tests.ps1` on Windows.

No makefile edit is needed for a new `tests/*.cpp` file because the existing wildcard-based rule discovers it automatically. The Windows CMake test project also auto-discovers new `tests/*.cpp` files via `file(GLOB ... CONFIGURE_DEPENDS ...)`.

## Transport Correctness Scenes (Roadmap Step 2)

These scenes validate spectral and SMS correctness. They require visual or
statistical comparison rather than deterministic assertions.

Assume `RISE_MEDIA_PATH` is set to the repo root before running any of the scene-based checks below:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
```

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
```

**Expected**: Dispersive glass caustic with per-wavelength evaluation. The sphere should show a slight chromatic tint from dispersion via the spectral PT + SMS path through `ManifoldSolver::EvaluateAtShadingPointNM` (G(x,v_1) · |det(δv_1/δy)| geometry).

### PT + SMS Caustic Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_vcm.RISEscene             # reference
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_pt_sms_hispp.RISEscene   # unit under test
```

**Expected**: The PT+SMS render's caustic mean luminance should match the VCM reference within ~5%.

## BSSRDF Furnace Tests (Energy Conservation)

These scenes validate that the BSSRDF subsurface scattering implementation conserves energy. They use a large sphere (R=10, ~40x mean free path) in a uniform emissive box so the geometry approaches the flat-slab limit where analytical predictions are available.

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_absorption.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_zero_absorption.RISEscene
```

**Output format**: HDR explicitly converted to `ROMMRGB_Linear`. RISE's working
space is Rec.709 Linear, so this is no longer a verbatim-store path; the ratios
below are intentionally measured after the same output conversion in both
renders.

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
