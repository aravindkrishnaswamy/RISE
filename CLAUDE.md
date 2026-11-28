# RISE Claude Companion

Start with [README.md](README.md) for the repo map, [AGENTS.md](AGENTS.md) for the shared agent guide, [docs/ROADMAP.md](docs/ROADMAP.md) for the current implementation roadmap, and [docs/README.md](docs/README.md) for deep dives. This file pairs quickstart reminders with the current, load-bearing rendering and engineering invariants (historical per-fix logs remain in git history and [docs/DEBT_LEDGER.md](docs/DEBT_LEDGER.md)).

## Quickstart

- Linux/macOS main build: `make -C build/make/rise -j8 all` (`LTO=thin|full|off` on macOS; default `LTO=thin`, `make clean` when switching — see [docs/BUILD_FLAGS.md](docs/BUILD_FLAGS.md))
- Linux/macOS single test: `make -C build/make/rise build-test/<TestName>` (note: `make all` does not relink tests; rebuild the test target after library edits)
- Linux/macOS tests: `make -C build/make/rise -j8 tests` then `./run_all_tests.sh` (`--tier all|fast|extended` [default `all`], `--filter "<glob>"`, `--timeout <sec>`, `--jobs <n>` [default: one test per core; tests marked `exclusive` in `tests/test_schedule.tsv` run alone])
- Stash-based red-proof helper: `tools/red_prove.sh [--args "<test-args>"] <TestName> <marker> <src-file>...`
- Windows tests: `cmake -S build/cmake/rise-tests -B build/cmake/rise-tests/_out -A x64` then `cmake --build build/cmake/rise-tests/_out --config Release --target rise_all_tests --parallel` then `.\run_all_tests.ps1` (`-Tier all|fast|extended`, `-Filter "<glob>"`, `-TimeoutSeconds <sec>`, `-Jobs <n>`, `-Config Debug`)
- Sample render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
# ~1 s: path-traced (direct light only) shapes -> rendered/shapes.png + shapes_denoised.png
# (`mkdir -p rendered` first in a fresh checkout or worktree; the directory is gitignored)
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

(`shapes.RISEscene` renders with `pathtracing_pel_rasterizer`; legacy rasterizer/material tiers are documented in [docs/SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md) §11.4.)

### One-time setup: OIDN with GPU support (extlib submodule)

OIDN is shipped as a git submodule at `extlib/oidn/source` so RISE links against an in-tree install with GPU device backends enabled (Metal on macOS; CUDA / HIP / SYCL toggles on Windows). System OIDN installs from Homebrew, vcpkg, etc. are typically built with CPU device only — RISE's denoise then silently runs on CPU. Full background in [docs/OIDN.md](docs/OIDN.md).

```sh
# macOS / Linux (from source; on macOS requires Xcode MetalToolchain component)
git submodule update --init extlib/oidn/source
extlib/oidn/build.sh

# macOS prebuilt binary release (includes Metal device module without MetalToolchain)
extlib/oidn/fetch_prebuilt.sh
```

```powershell
# Windows (PowerShell, in repo root)
git submodule update --init extlib/oidn/source
pwsh -File extlib\oidn\build.ps1     # add -EnableCuda / -EnableHip / -EnableSycl as needed
```

All three populate `extlib/oidn/install/`, which RISE's Makefile, Xcode project, and VS2022 projects prefer over system OIDN. Skipping this step is fine for development (CPU fallback), but `scripts/create_macos_release.sh` requires a Metal-capable install.

## Source-file add/remove — touch ALL five build projects

When adding or removing any `.cpp` / `.h` under `src/Library/`, update every one of these (mechanically enforced by `tests/SourceHygieneTest.cpp`):

1. `build/make/rise/Filelist` (Unix/Linux, canonical)
2. `build/cmake/rise-android/rise_sources.cmake` (Android NDK)
3. `build/VS2022/Library/Library.vcxproj` (Windows `<ClCompile>` / `<ClInclude>`)
4. `build/VS2022/Library/Library.vcxproj.filters` (Windows Solution Explorer filters)
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` (`PBXBuildFile`, `PBXFileReference`, `PBXGroup`, and `Sources` / `Headers` build phases across library + GUI targets)

Full rules, exclusions, and related surfaces (API, parser chunks, tests, scenes) are in [AGENTS.md](AGENTS.md) → "Change Checklist".

## Compiler warnings are bugs

Never suppress warnings with pragmas or `-Wno-*` flags — fix the root cause. Both `make` and Xcode `RISE-GUI` must come up warning-free on a clean rebuild. See [AGENTS.md](AGENTS.md) → "Compiler Warnings Are Bugs".

## Current Load-Bearing Invariants

- **Floating-point & LTO flags ([docs/BUILD_FLAGS.md](docs/BUILD_FLAGS.md)):**
  - Both `make` (`Config.OSX`) and all Xcode configurations (`Development`, `Deployment`, `Default`, `Opto`) pass `-ffast-math -fno-finite-math-only` globally, with strict-FP32 TUs (`src/Library/Utilities/Osl*.cpp`, simulation/fire/fire-adjacent/HyLIoS/radiometric strict sources; the make and Xcode per-file lists must stay equal) compiled with `-fno-fast-math -ffp-contract=off`.
  - Because `-fno-finite-math-only` is active, `std::isfinite` / `std::isnan` work on every platform; `RISE::IsFiniteDouble` ([src/Library/Utilities/FiniteMath.h](src/Library/Utilities/FiniteMath.h)) is retained for uniformity and `-Ofast` resistance.
  - Keep `SAFE_INV` / `±FLT_MAX` finite sentinels in BVH/ray slab tests (do **not** replace with `infinity()`, which yields `0 * inf = NaN` on axis-aligned slab faces and silently accepts missed slabs).
  - `make` defaults to `LTO=thin` since fire solver version 4 (`LTO=full|off` opt-in; `make clean` when switching, make does not track the mode); simulation and fire bytes are the same in every LTO mode, the one LTO-dependent test being `FireSimulationSolverTest` (pinned for thin; [docs/BUILD_FLAGS.md](docs/BUILD_FLAGS.md) §8). Xcode `Deployment` uses `YES_THIN` while `Opto` (release DMG) uses full `LLVM_LTO = YES`. Verify FP- and LTO-sensitive changes against `Opto`.
  - `SourceHygieneTest` bans NaN/Inf control-flow sentinels in `tests/*.cpp` (opt out only with `// HYGIENE-OK: <reason>`).

- **Color space, spectral uplift, and `IScalarPainter` ([docs/COLOR_SPACE_MIGRATION.md](docs/COLOR_SPACE_MIGRATION.md), [docs/SPECTRAL_ILLUMINANT_CONVENTION.md](docs/SPECTRAL_ILLUMINANT_CONVENTION.md), [docs/ISCALARPAINTER_REFACTOR.md](docs/ISCALARPAINTER_REFACTOR.md)):**
  - `RISEPel = Rec709RGBPel` (Linear Rec.709, D65 working space).
  - Stage C illuminant-aware Jakob–Hanika LUT has 100% convergence: reflectance uses `GetColorNM` (dimensionless), while sources/emitters use `GetRadianceNM` (D65-normalized illuminant).
  - Physical-scalar parameters (IOR, roughness, extinction, absorption, scattering, phase asymmetry) MUST use `IScalarPainter` (never JH-uplifted), whereas color/reflectance/emission slots use `IPainter`. Never add a `GetColorNM` override to `IScalarPainter`.
  - Wavelength-varying `IScalarPainter`s sample RGB at `ScalarPainterRGB::kChannelNM = {611, 549, 465}` nm per channel so dispersive curves render dispersively in RGB as well as spectral rasterizers.

- **Integrators & selection ([docs/RENDERING_INTEGRATORS.md](docs/RENDERING_INTEGRATORS.md), [docs/AUTO_RASTERIZER_DESIGN.md](docs/AUTO_RASTERIZER_DESIGN.md)):**
  - 9 active rasterizers: `pathtracing_{pel,spectral}_rasterizer`, `bdpt_{pel,spectral}_rasterizer`, `vcm_{pel,spectral}_rasterizer`, `auto_{rasterizer,spectral_rasterizer}`, and `hyperspectral_radiometric_rasterizer` (fixed-wavelength NM radiometric PT sweeps for HyLIoS / quantitative radiometry). The 4 shader-dispatch / MLT rasterizers (`pixelpel_rasterizer`, `pixelintegratingspectral_rasterizer`, `mlt_rasterizer`, `mlt_spectral_rasterizer`) are FROZEN legacy ([docs/LEGACY_DEPRECATION_ASSESSMENT.md](docs/LEGACY_DEPRECATION_ASSESSMENT.md)).
  - Default integrator is PT (wins $\sigma^2 \cdot T$ on most scenes); use BDPT for strong-indirect/glossy lighting, VCM for SDS caustics/refractive/volumetric merging, or `auto_*` for two-tier automatic routing.
  - Check all three wavelength variants (**Pel / NM / HWSS**) on any integrator fix or feature: Pel uses `RandomlySelect(xi, false)`, while NM and HWSS must use `RandomlySelect(xi, true)` to match spectral `selectProb` compensation.

- **MIS heuristics & light sampling ([docs/MIS_HEURISTICS.md](docs/MIS_HEURISTICS.md), [docs/LIGHTS.md](docs/LIGHTS.md), [docs/ENVIRONMENT_SAMPLING.md](docs/ENVIRONMENT_SAMPLING.md)):**
  - PT uses power-2 (with optional Kondapaneni optimal-MIS); BDPT uses power-2; VCM uses balance ($\beta = 1$, mandated by the Georgiev $O(1)$ running-quantities recurrence — do not "fix" this intentional asymmetry).
  - An emitter hit that NEE/light sampling could not have generated (`CachedPdfSelectLuminary == 0` for dropped/non-area emitters, or self-hits on a non-convex emitter tracked via `pNEESelfObject`) keeps full weight `1` across PT RGB/NM/HWSS, `EmissionShaderOp` (for marked PT/RayCaster continuation rays), and BDPT (`lightSamplingStrategyAbsent`), and is not suppressed by SMS (DL-633/DL-635).
  - `LightSampler` caches derived from `pEnvSampler` must be recomputed in `SetEnvironmentSampler()` because `RayCaster::AttachScene` calls `Prepare()` before `SetEnvironmentSampler()`.

- **Refractive & subsurface transport ([docs/REFRACTIVE_RADIANCE_SCALING.md](docs/REFRACTIVE_RADIANCE_SCALING.md), [docs/SUBMERGED_CAMERA_IOR_SEEDING.md](docs/SUBMERGED_CAMERA_IOR_SEEDING.md), [docs/MATERIALS.md](docs/MATERIALS.md)):**
  - Flux/importance paths carry the $\eta^2$ basic-radiance factor across refractive boundaries and graded-index media.
  - Submerged cameras, lights, and photons seed their `IORStack` via `IORStackSeeding::SeedFromPoint`.
  - `rough_dielectric_material` provides anisotropic GGX VNDF reflection/refraction with height-correlated Smith $G_2$ and two-sided Kulla–Conty multiscatter compensation ([docs/ROUGH_DIELECTRIC_ENERGY.md](docs/ROUGH_DIELECTRIC_ENERGY.md)); HWSS conservatively terminates companions at rough dielectric interfaces until per-lane medium tracking lands.
  - `hylios_skin_material` provides local (baked asset) and nonlocal (spatial forward-kernel) hyperspectral skin transport, supporting `wavelength_nm` subset loading and spatially varying `boundary_roughness_map` in nonlocal mode ([docs/HYLIOS_WORKPLAN.md](docs/HYLIOS_WORKPLAN.md) §13).

- **SMS ([docs/SMS.md](docs/SMS.md), [docs/SMS_EXTENDED_DESIGN.md](docs/SMS_EXTENDED_DESIGN.md)):**
  - `sms_seeding "snell"` (default) excels on displaced/bumpy refractive casters; `"uniform"` excels on smooth analytic reflection/glint caustics; `sms_target_bounces` pins the target specular chain length $K$.
  - `sms_two_stage` helps smooth primitives + normal maps and regresses on heavily displaced meshes (default OFF; ignored when `sms_extended TRUE`).
  - `sms_extended TRUE` enables multi-bounce R/T solver across RGB/NM/HWSS (per-lane HWSS anchors with dedicated Sobol stream block).
  - Multi-lobe path-tree branching (`branching_threshold`) is excised across all integrators.

- **Path-dependence containment & Blender procedural bridge ([docs/BLENDER_MATERIAL_TRANSLATION.md](docs/BLENDER_MATERIAL_TRANSLATION.md), [docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md)):**
  - Non-physical view/ray shading signals (`osl_layer_weight`, `osl_fresnel`, `osl_is_camera_ray`, `osl_backfacing`) and spatially varying SSS `MixMaterial` weights propagate `RISE::PathDependence` flags (`osl_ao` is `kNone`). Every new composite painter, modifier, or material must forward `GetPathDependence()`.
  - `AutoRasterizer` routes flagged scenes to PT; BDPT, VCM, and MLT refuse flagged scenes; OpenPGL skips flagged hits; SMS excludes flagged casters.
  - Blender add-on bridge is at **C ABI v23** (`src/Blender/native/rise_blender_bridge.h`), compiling 42 Cycles shader node types natively via `expression_painter` / `mix_material` / `ReliefModifier` / `ModifierStack` / `BlendedModifier`.

- **Simulation subsystem ([docs/SIMULATION_SOLVER_CONTRACT.md](docs/SIMULATION_SOLVER_CONTRACT.md), [docs/SIMULATION_GUI_STATUS.md](docs/SIMULATION_GUI_STATUS.md), [docs/FIRE_SMOKE_STATUS.md](docs/FIRE_SMOKE_STATUS.md)):**
  - Four builtin solvers behind the frozen v1 solver contract: `fire.methane` (version 4; Metal + experimental CPU backend), `cloth.shell` (m3a.8), `rigid.bodies` (m6.2), and `liquid.mac` (m7m.1, including two-way cloth/sheet coupling, two-valued sheet-cut level set, and sheet viscosity).
  - Scene chunks (`cloth_simulation`, `rigid_simulation`, `liquid_simulation`, `fire_simulation`, `simulation_group`, `simulation_cache`, `simulated_mesh_geometry`, `simulated_points_geometry`) bake through `rise-sim` into versioned CBOR caches (defaulting to the per-user cache folder) and play back portably across platforms.

- **Thread priority ([docs/PERFORMANCE.md](docs/PERFORMANCE.md)):**
  - `GlobalThreadPool()` assigns workers across P-cores and E-cores (reserving `render_thread_reserve_count`, default 1 E-core, for the OS). Benchmarks must pass `render_thread_reserve_count 0`. Never downgrade all render workers to `QOS_CLASS_UTILITY` / `BELOW_NORMAL`.

- **Noise to ignore:** `.claude/`, `.agents/`, `rendered/`, `.DS_Store`, local `*.o`, generated logs (`RISELog.txt`, `build_log.txt`, `RISE_Log.txt`), `.vscode/`, `xcuserdata/`, `Config.specific`, and `*.mov`.
