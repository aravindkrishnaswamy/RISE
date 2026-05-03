# RISE Claude Companion

This file intentionally stays thin so it does not drift from the shared docs. Start with [README.md](README.md) for the repo map, [AGENTS.md](AGENTS.md) for the shared agent guide, and [docs/README.md](docs/README.md) for deep dives.

## Quickstart

- Linux/macOS main build: `make -C build/make/rise -j8 all`
- Linux/macOS tests: `make -C build/make/rise tests` then `./run_all_tests.sh`
- Windows tests: `.\run_all_tests.ps1`
- Windows tests (Debug): `.\run_all_tests.ps1 -Config Debug`
- Sample render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

### One-time setup: OIDN with GPU support (extlib submodule)

OIDN is shipped as a git submodule at `extlib/oidn/source` so RISE
links against an in-tree install with GPU device backends enabled
(Metal on macOS; CUDA / HIP / SYCL toggles on Windows). System OIDN
installs from Homebrew, vcpkg, etc., are typically built with CPU
device only — RISE's denoise then silently runs on CPU even on a
Metal-capable Mac. Full background in [docs/OIDN.md](docs/OIDN.md)
(OIDN-P0-3 install gotcha).

Run once after cloning, and again whenever the OIDN submodule
commit is bumped:

```sh
# macOS / Linux
git submodule update --init extlib/oidn/source
extlib/oidn/build.sh
```

```powershell
# Windows (PowerShell, in repo root)
git submodule update --init extlib/oidn/source
pwsh -File extlib\oidn\build.ps1     # add -EnableCuda / -EnableHip / -EnableSycl as needed
```

Both scripts populate `extlib/oidn/install/`; RISE's Makefile and
VS2022 project files prefer that path over any system OIDN. Verify
the selected device on your first render — look for:
`OIDN: creating Metal device (one-time per rasterizer)` in the log.

Skipping this step is fine — RISE falls back to the system OIDN
(typically Homebrew's CPU-only build) and denoise still works,
just without GPU acceleration.

## Source-file add/remove — touch ALL five build projects

When you add **or remove** any `.cpp` / `.h` under `src/Library/`, update every one of these; none auto-discovers files:

1. `build/make/rise/Filelist` (Unix/Linux, canonical)
2. `build/cmake/rise-android/rise_sources.cmake` (Android NDK, consumed by `build/cmake/rise-android/CMakeLists.txt` which the Gradle build invokes)
3. `build/VS2022/Library/Library.vcxproj` (Windows, tracked in git) — `<ClCompile>` for `.cpp`, `<ClInclude>` for `.h`.
4. `build/VS2022/Library/Library.vcxproj.filters` — same entries with `<Filter>` tags so the Solution Explorer tree matches the build.
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` — Xcode references appear in four sections (`PBXBuildFile`, `PBXFileReference`, the containing `PBXGroup`, and per-target `Sources` / `Headers` build phases) and usually in two targets (library + GUI); delete every match when removing a file.

Full rules, exclusions, and related update surfaces (API, parser chunks, tests, scenes) are in [AGENTS.md](AGENTS.md) → "Change Checklist".

## Compiler warnings are bugs

Don't introduce warnings.  Don't leave them.  Don't suppress them with
pragmas or `-Wno-*` flags — fix the root cause.  Both the `make` build
and the Xcode `RISE-GUI` build must come up warning-free on a *clean*
rebuild before you call a task done — incremental builds hide warnings
on files that didn't recompile.  Full rationale, the principled fix per
warning kind, and the verification commands are in [AGENTS.md](AGENTS.md) →
"Compiler Warnings Are Bugs".

## High-Value Facts

- **SMS two-stage solver scope** (`sms_two_stage` parameter, [docs/SMS_TWO_STAGE_SOLVER.md](docs/SMS_TWO_STAGE_SOLVER.md)): the Zeltner 2020 §5 two-stage solver helps on **smooth analytic primitives with normal-perturbing maps** (verified: bumpmap on `ellipsoid_geometry` reaches ΣL_sms/ΣL_supp = 1.15 at gentle bumps).  It **regresses** on **heavily-displaced meshes** (`displaced_geometry` with `disp_scale` > ~10 % of curvature radius): Stage 1 converges to the smooth-surface caustic, whose corresponding mesh position is up to `disp_scale` units away from the bumpy caustic — Stage 2's seed is *worse* than the original Snell-traced on-mesh seed.  This matches Mitsuba's reference scope: their [`Figure_9_Twostage`](https://github.com/tizian/specular-manifold-sampling/tree/master/results/Figure_9_Twostage) scenes use only smooth primitives + `normalmap` BSDFs; their displaced-mesh comparison ([`Figure_16`](https://github.com/tizian/specular-manifold-sampling/tree/master/results/Figure_16_Displacement)) never engages two-stage.  Mitsuba's smoothing is plumbed via `bsdf->lean()` (LEAN moments, Olano-Baker 2010) — a no-op for non-normal-mapped BSDFs.  **Default is OFF; turn ON only for the smooth-primitive + normal-map regime.**  For heavily displaced caustics, prefer VCM.


- `branching_threshold` (StabilityConfig, default 0.5): normalized throughput gate for subpath splitting at multi-lobe delta vertices (Fresnel dielectric etc.). Parameter on all rasterizer chunks (PT/BDPT/VCM pel & spectral, MLT). `0` = always branch at first encounter, `1` = never branch. Live on RGB + spectral NM for both eye and light subpaths across PT/BDPT/VCM — the photon-store build also branches and renormalizes `mLightSubPathCount` with actual `pathsShot`. MLT forces `1.0` (Markov-chain proposal assumes single-subpath). The retired `branch` param on PathTracing/DistributionTracing shader-ops is silently ignored by the parser; photon tracers keep their own `bBranch` (out of scope).
- **Chunk parsers are descriptor-driven** (since 2026-04). Every `IAsciiChunkParser` overrides only `Describe()` and `Finalize(const ParseStateBag&, IJob&)`; the default `ParseChunk` impl validates input lines against `Describe().parameters` and dispatches to `Finalize`. The descriptor IS the accepted-parameter set, so syntax-highlighter / suggestion-engine drift is structurally impossible. Adding a new chunk = new `IAsciiChunkParser` subclass + register in `CreateAllChunkParsers()`. Adding a parameter = one entry in `Describe()` + one `bag.GetX(...)` call in `Finalize`. Full how-to in [src/Library/Parsers/README.md](src/Library/Parsers/README.md); architecture overview in the header of [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp).
- Public construction API: [src/Library/RISE_API.h](src/Library/RISE_API.h)
- High-level construction interface: [src/Library/Interfaces/IJob.h](src/Library/Interfaces/IJob.h)
- Main assembly implementation: [src/Library/Job.cpp](src/Library/Job.cpp)
- Parser source of truth: [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp)
- Render-pass entry point: [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
- Scene files expect `RISE ASCII SCENE 5`, and chunk braces must be on their own lines
- `Job::InitializeContainers()` creates the `Scene`, named managers, `"none"` defaults, and default shader ops
- `Job::SetPrimaryAcceleration()` replaces the object manager and discards previously added objects
- Named managers plus explicit reference counting are foundational; inspect [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h) and [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h) before changing lifetimes
- Tests are standalone executables, not a unit-test framework suite
- The logger defaults to `RISELog.txt`, but the main CLI overrides it to `RISE_Log.txt` in the working directory
- **Scene-authoring conventions** ([docs/SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md)): `directional_light.direction` is the vector FROM the surface TO the light source (a surface is lit when `N · direction > 0`), NOT the shine direction.  Cameras at `+Z` looking at origin need lights with **positive Z** in `direction`.  Importers from foreign formats (glTF, Unity, Unreal) that use the shine-direction convention MUST negate before passing through.  Other recurring traps captured in the same doc: `colorspace` selection, `power` semantics, `standard_object` transform precedence (`matrix > quaternion > orientation`), V-axis flip on glTF textures, alpha-mask only honored under `pixelpel_rasterizer`

## Thread priority — read before touching anything threading-related

- **Production default (topology-aware):** every P-core gets a render worker, every E-core except **one** also gets a worker. The reserved E-core keeps UI / daemons responsive. macOS workers use `QOS_CLASS_USER_INITIATED`; Linux/Windows use CPU-affinity pinning.
- **Benchmarks:** set `render_thread_reserve_count 0` to give every core a worker. `./bench.sh` at repo root does this automatically. Without the override, Apple Silicon's E-core reservation makes the machine appear ~10 % slower than it really is.
- **Do NOT** put all threads at `QOS_CLASS_UTILITY` / `BELOW_NORMAL` / `nice(10)`. That's the old bug — cost 2–4× real throughput on macOS. A legacy `force_all_threads_low_priority` option still exists for users who explicitly want the background-render mode; it's opt-in only.
- Controls live in `src/Library/Utilities/CPUTopology.{h,cpp}`, `src/Library/Utilities/ThreadPool.cpp`, `src/Library/Utilities/Threads/ThreadsPTHREADs.cpp`, `src/Library/Utilities/Threads/ThreadsWin32.cpp`.
- See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) "Thread priority policy" for the full rationale, per-platform APIs, and measurement recipe.

## Engineering Skills — read the matching skill before starting

Process skills distilled from prior RISE sessions.  Auto-invoked via
`.claude/skills/<name>/SKILL.md` shims; full content lives in
[docs/skills/](docs/skills/).

- [adversarial-code-review](docs/skills/adversarial-code-review.md) — validate a non-trivial change with 2–3 orthogonal reviewers in parallel.  Fire when the user asks for adversarial review / multiple reviewers, or after landing any correctness-sensitive change.
- [performance-work-with-baselines](docs/skills/performance-work-with-baselines.md) — structured quantitative perf work: baseline before code, measure with stddev, profile first, one variable at a time, correctness invariant, numeric reporting.
- [abi-preserving-api-evolution](docs/skills/abi-preserving-api-evolution.md) — when touching `RISE_API.h`, any `src/Library/Interfaces/` header, or a derived class that already has an overloaded method name.  Three layers (exported signatures, vtable slots, name hiding) each with their own failure mode.
- [const-correctness-over-escape-hatches](docs/skills/const-correctness-over-escape-hatches.md) — decision tree to run BEFORE reaching for `mutable` / `const_cast` / dropping a `const`.
- [precision-fix-the-formulation](docs/skills/precision-fix-the-formulation.md) — when you're tempted to widen a `< NEARZERO` check, add a fudge factor, or pick a magic ε to silence speckles / fireflies / FP-noise spurious hits.  Find the cancellation; deflate the polynomial / transform all corners / evaluate both algorithm branches instead of papering over with thresholds.
- [sms-firefly-diagnosis](docs/skills/sms-firefly-diagnosis.md) — when you see bright outlier pixels in an SMS render, or the user reports "fireflies" in an SMS scene.  Walks the firefly-triggered trace, decodes `attenChain` fingerprints into specific bugs, and lists audit sites (`ManifoldSolver`, `SMSPhotonMap`, PT emission-suppression state) that historically produce spurious contributions.
- [bdpt-vcm-mis-balance](docs/skills/bdpt-vcm-mis-balance.md) — when a BDPT or VCM render disagrees with PT (mean off, max blown out, splotches that don't fade with sample count), user reports "fireflies even at high spp", or you've just changed MIS code (`MISWeight`, `EvaluateNEE`, `EvaluateMerges`, `Convert*Subpath`, the auto-radius pre-pass).  Covers PT-vs-X test pattern, per-strategy instrumentation, the recurring delta-position-light vs delta-surface-scatter trap, and applying the fix at the right layer (skip rule vs running quantity vs auto-radius gate).
- [variance-measurement](docs/skills/variance-measurement.md) — when you need to PROVE a rendering change reduces variance / RMSE rather than just looks different.  K-trial EXR protocol with `bin/tools/HDRVarianceTest.exe`, master-vs-fix stash-rebuild-pop sequencing, both pure variance and RMSE-vs-reference metrics.  Knows the pitfalls (PNG quantization, HDRReader RLE bug, `LNK1104` SolutionDir trap, K too small).
- [effective-rise-scene-authoring](docs/skills/effective-rise-scene-authoring.md) — when authoring a new `.RISEscene` from scratch, porting from another tool (glTF / Blender / Unity / Unreal lighting setups), or diagnosing a scene that renders unexpectedly dark / wrong-coloured / oriented backwards.  Walks the [SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md) checklist (directional `direction` is FROM-surface-TO-light, `power` semantics, colour-space defaults, transform precedence), then the Lambertian-control-sphere diagnostic before you touch material code.

## Read Next

- Core library map: [src/Library/README.md](src/Library/README.md)
- Interface contracts: [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
- Scene language and parser behavior: [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
- Scene taxonomy: [scenes/README.md](scenes/README.md)
- Curated showcase scenes: [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
- Regression scenes: [scenes/Tests/README.md](scenes/Tests/README.md)
- Executable tests: [tests/README.md](tests/README.md)
- Thread-safety and scene immutability: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Planned improvements: [docs/IMPROVEMENTS.md](docs/IMPROVEMENTS.md)
