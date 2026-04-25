# RISE Agent Guide

Navigation aid for LLM-powered contributors. Use [README.txt](README.txt) for historical user context, [README.md](README.md) for the repo map, and [docs/README.md](docs/README.md) for deep dives and roadmap docs.

## Doc Split

- `README.md`: repo map, read order, and canonical command quick reference
- `AGENTS.md`: agent-focused working model, invariants, and change checklist
- `CLAUDE.md`: thin compatibility shim that should point back to the shared docs instead of drifting on its own
- `docs/*.md`: focused design notes and forward-looking plans

## Start Here

- Public external construction API: [src/Library/RISE_API.h](src/Library/RISE_API.h)
- High-level convenience interface: [src/Library/Interfaces/IJob.h](src/Library/Interfaces/IJob.h)
- Main assembly implementation: [src/Library/Job.cpp](src/Library/Job.cpp)
- Runtime scene object: [src/Library/Scene.h](src/Library/Scene.h)
- Scene syntax and chunk registry: [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp)
- Pixel render loop: [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
- Iterative PT integrator: [src/Library/Shaders/PathTracingIntegrator.h](src/Library/Shaders/PathTracingIntegrator.h)
- Main CLI entry point: [src/RISE/commandconsole.cpp](src/RISE/commandconsole.cpp)

Active work is mainly in `src/Library`, `src/RISE`, `scenes/FeatureBased`, `scenes/Tests`, `tests`, and `build/make/rise`. `src/DRISE`, `src/PRISE`, `src/3DSMax`, and `src/RISE/risempi.cpp` are legacy sidecars.

## Core Mental Model

1. `commandconsole.cpp` creates a `Job`.
2. `Job::InitializeContainers()` creates the `Scene` plus all named managers and installs default null assets and default shader ops.
3. A parser loads a `.RISEscene` or script into the `Job`.
4. The `Job` wires cameras, geometry, painters, materials, lights, shaders, photon maps, rasterizer state, and outputs together by name.
5. The rasterizer attaches the `Scene` to a `RayCaster` and renders through a raster sequence.
6. If an irradiance cache exists and is not precomputed, the pixel renderer performs a dedicated irradiance pass before the normal pass.

## Repo-Specific Invariants

- The primary external construction boundary is the C-style API in `RISE_API.h`, even when higher-level wrappers exist.
- `IJob` is the easiest high-level way to understand how external code is expected to build scenes.
- `Job::SetPrimaryAcceleration()` replaces the object manager. Calling it after adding objects discards them.
- Most scene elements are named and stored in managers. Parser chunks usually resolve dependencies by name through those managers.
- `Job::InitializeContainers()` installs `"none"` defaults for a null material and null painter, and also creates several default shader ops.
- The scene parser currently expects the header `RISE ASCII SCENE 5`.
- In `.RISEscene` files, chunk braces must appear on their own lines.
- Parser support for macros, math expressions, embedded commands, and `FOR` / `ENDFOR` loops is implemented centrally in `AsciiSceneParser.cpp`.
- Tests are standalone executables, not a framework-based suite.
- Two rendering pipelines exist: **shader-dispatch** (RayCaster → ShaderOp chain) and **pure integrator** (PathTracingIntegrator called directly by `pathtracing_pel_rasterizer` / `pathtracing_spectral_rasterizer`). Changes to path tracing logic should go in `PathTracingIntegrator`, which serves both pipelines.
- **OIDN + FilteredFilm invariant**: When OIDN denoising is enabled, the filtered film resolve is skipped. OIDN needs raw MC noise, not filter-reconstructed images. See `docs/ARCHITECTURE.md` for details.
- **`branching_threshold` knob** (StabilityConfig, default 0.5): gates subpath splitting at multi-lobe delta vertices. Active in `PathTracingIntegrator::IntegrateFromHit` (RGB specular SPF) and `BDPTIntegrator::Generate{Eye,Light}Subpath{,NM}` (RGB + spectral NM). Live per-branch consumers: `PathTracingPelRasterizer`, `BDPTPelRasterizer`, `BDPTSpectralRasterizer`, `VCMPelRasterizer`, `VCMSpectralRasterizer`, and `VCMRasterizerBase`'s photon-store build (each non-empty branch counts as a subpath; `mVCMNormalization` renormalized with actual `pathsShot` via the 2-arg `ComputeNormalization` overload). `MLTRasterizer` / `MLTSpectralRasterizer` force `threshold=1.0` because the Markov-chain proposal measure requires single-subpath. BDPT consumers apply `eb==0` / `lb==0` dedup gates for strategies that read only shared-prefix endpoints (`t==1` splat, `s==0`/`s==1` emission and NEE); VCM consumers dedup structurally (S0/NEE/Merges outside `lb` loop; Splat outside `eb` loop). The legacy `bBranch` / `branch` flag on `PathTracingShaderOp` / `DistributionTracingShaderOp` is retired — the parser silently ignores it. Photon tracers still keep their own `bBranch` (out of scope). Scene authors who want to disable split on a specific rasterizer set `branching_threshold 1.0`; to force always-split, `branching_threshold 0`.
- **Silent-negative pixel detector**: `FileRasterizerOutput::WriteImageToFile` scans the image at output time and warns if any pixel has a negative channel. Common cause: pixel reconstruction filter with negative side lobes (Mitchell/Lanczos) interacting with bright neighbors. Implementation: `src/Library/Utilities/RasterSanityScan.h`. Unit test: `tests/RasterSanityScanTest.cpp`. If the warning becomes noise on a specific scene, switch that scene's `pixel_filter` to `gaussian` / `triangle` / `box`.
- **Thread priority invariant (LOAD-BEARING)**: `GlobalThreadPool()` is topology-aware — every P-core gets a render worker, every E-core except `render_thread_reserve_count` (default 1) also gets one. Workers use `QOS_CLASS_USER_INITIATED` on macOS and `sched_setaffinity` on Linux/Windows to stay on fast cores. Benchmarks MUST set option `render_thread_reserve_count 0` to use every core — otherwise 1 E-core is reserved for the OS and wall-time looks ~10 % slower. Never put all workers at `QOS_CLASS_UTILITY` / `BELOW_NORMAL` thinking it's "nicer" — the old bug cost 2–4× real throughput on Apple Silicon. Full rationale in [docs/PERFORMANCE.md](docs/PERFORMANCE.md) "Thread priority policy."

## Reference Counting

- Many core classes derive from `Implementation::Reference` and use explicit reference counting.
- Managers usually `addref()` on insertion and release on shutdown.
- Before changing lifetime behavior, inspect [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h), [src/Library/Interfaces/IManager.h](src/Library/Interfaces/IManager.h), and [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h).

## Change Checklist

- New render feature: implement the class, expose it through `RISE_API` if externally constructible, add a `Job` wrapper if needed, register a scene chunk if user-authored, add a sample scene, add a focused test, and update **every** build project for new `.cpp` / `.h` files (see next item).
- **Adding a scene chunk OR a parameter to an existing chunk**: chunk parsers are descriptor-driven (since 2026-04). Each `IAsciiChunkParser` overrides `Describe()` (returns a `ChunkDescriptor` that enumerates every accepted parameter) and `Finalize(const ParseStateBag&, IJob&)` (reads typed values out of the bag and emits the `pJob.AddX` call) — no chunk parser overrides `ParseChunk` directly; the default impl dispatches via the descriptor. The descriptor IS the parser's accepted-parameter set, so drift between "what gets parsed" and "what the syntax highlighter / suggestion engine advertise" is structurally impossible. To add a new chunk: define a new `IAsciiChunkParser` subclass (`Describe` + `Finalize`) and register it in `CreateAllChunkParsers()`; both syntax highlighters and the scene-editor suggestion engine pick it up automatically. To add a parameter: append one entry to the chunk's `Describe()` parameter list and read it via `bag.GetX(...)` in `Finalize`. Full how-to (with skeleton, helper-template catalog, and `ParseStateBag` accessor reference) lives in [src/Library/Parsers/README.md](src/Library/Parsers/README.md). The architecture overview is also documented in the header of [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp).
- **Adding OR removing a source file anywhere under `src/Library/`**: the same five build projects must be updated in lock-step, whether you are adding new files or deleting existing ones. Missing any one leaves at least one platform broken. All five are authoritative — none auto-discovers files:
  - `build/make/rise/Filelist` — SRCLIB sub-list for `.cpp` (the canonical Unix/Linux build).
  - `build/cmake/rise-android/rise_sources.cmake` — `RISE_LIB_SOURCES` list for `.cpp` (Android NDK build, mirrors `Filelist` SRCLIB by hand). The Android Gradle build reads this via `build/cmake/rise-android/CMakeLists.txt`; no other file in `android/` references the library source list.
  - `build/VS2022/Library/Library.vcxproj` — `<ClCompile>` for `.cpp` and `<ClInclude>` for `.h` (Windows build, tracked in git).
  - `build/VS2022/Library/Library.vcxproj.filters` — same entries with `<Filter>` tags mirroring the existing folder taxonomy. Keep `.cpp` in `ClCompile` and `.h` in `ClInclude` so Visual Studio's Solution Explorer tree matches what's actually built.
  - `build/XCode/rise/rise.xcodeproj/project.pbxproj` — `PBXFileReference` + `PBXBuildFile` + group + build-phase entries. When removing a file, every matching ID must be deleted from the `PBXBuildFile` section, the `PBXFileReference` section, the containing `PBXGroup`, and the `Sources` / `Headers` build phases (there is one pair per target — library + GUI — so removals typically touch each ID in two places in each section).
  - Exclusions: `ManagedJob.cpp` (C++/CLI only), Windows-only files (`ThreadsWin32.cpp`, `LoadLibraryWin32.cpp`, `Win32Console.cpp`, `Win32WindowRasterizerOutput.cpp`), and the `Utilities/Communications/*` DRISE socket stack stay out of the Android cmake — everything else should appear in all five projects.
- When a change is wider than just `src/Library/` (new parser chunk, new API, new test, etc.), also update the other surfaces the feature touches: `src/Library/RISE_API.{h,cpp}`, `src/Library/Interfaces/IJob.h`, `src/Library/Job.{h,cpp}`, `src/Library/Parsers/AsciiSceneParser.cpp` chunk registry, `tests/` and its build rules in `build/make/rise/Filelist`, and the relevant scene coverage under `scenes/`.
- **Integrator wavelength variants — check ALL three (Pel / NM / HWSS) on any fix or feature**: integrators (PT, BDPT, VCM, MLT, …) are implemented as parallel RGB-per-lobe (`Pel`, e.g. `IntegrateFromHit`), single-wavelength (`NM`, e.g. `IntegrateFromHitNM`), and hero-wavelength spectral sampling (`HWSS`, e.g. `IntegrateFromHitHWSS`) variants in the same file. They diverge in subtle ways (HWSS opts out of subpath branching; NM always branches at multi-lobe vertices; Pel has a conditional `shouldSplit` gate) so a correctness bug or new feature almost never applies identically to all three but almost always applies to MORE than one. When touching any single variant, grep for the matching code in the other two (same function-name stem, parallel structure) and explicitly decide for each: does this fix/feature apply here too, does it need a variant-specific form, or is it genuinely inapplicable? Document the answer for the inapplicable cases so the next agent doesn't re-audit. Prior incident: a missing `/selectProb` division in PT's iterative random-select branch biased RGB renders dim and amplified RR fireflies; the same hero-driven bug existed in HWSS but had to be checked separately because the NM variant was not affected (NM always branches).
- Scene language change: inspect the parser registry, decide whether `CURRENT_SCENE_VERSION` should change, update representative scenes, and keep [src/Library/Parsers/README.md](src/Library/Parsers/README.md) aligned.
- Sample coverage change: use `scenes/FeatureBased` for curated showcase or torture scenes, and `scenes/Tests` for isolated baselines, feature checks, and regression coverage.

## Build And Test

- Linux/macOS build: `make -C build/make/rise -j8 all`
- Linux/macOS tests build: `make -C build/make/rise tests`
- Linux/macOS tests run: `./run_all_tests.sh`
- Windows tests configure: `cmake -S build/cmake/rise-tests -B build/cmake/rise-tests/_out -A x64`
- Windows tests build: `cmake --build build/cmake/rise-tests/_out --config Release --target rise_all_tests --parallel`
- Windows tests run: `.\run_all_tests.ps1`
- Windows tests run (Debug): `.\run_all_tests.ps1 -Config Debug`
- Batch scene render: `./run_scenes.sh`
- Sample scene render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

If you changed only header files, run `make -C build/make/rise clean` before rebuilding.

## Noise To Ignore

- `.claude/` is local agent workspace state, not project source.
- `rendered/` is output, not source.
- `.DS_Store` files are incidental.
- Untracked `*.o` files or `* 2.o` files under `src/Library` are local build artifacts and not source-of-truth code.
- Ignored `*.o` files or `* 2.o` files under `tests/` are local build artifacts, not source-of-truth tests.
- `build/make/rise/RISELog.txt`, `build/make/rise/build_log.txt`, and `RISE_Log.txt` are generated logs.
- `/.vscode/`, `Xcuserdata/`, and `Config.specific` are local IDE or platform artifacts.
- `*.mov` files are rendered video output.

## Engineering Skills

Process skills distilled from prior RISE sessions live in
[docs/skills/](docs/skills/).  Read the relevant skill BEFORE starting
a task of the matching kind — the lessons are captured there
precisely because ad-hoc judgment reliably misses them.

| Skill | Trigger |
|---|---|
| [adversarial-code-review](docs/skills/adversarial-code-review.md) | Validate a non-trivial change; user asks for multiple / adversarial reviewers. |
| [performance-work-with-baselines](docs/skills/performance-work-with-baselines.md) | Optimize runtime or memory; any change framed as "make X faster." |
| [abi-preserving-api-evolution](docs/skills/abi-preserving-api-evolution.md) | Change a public API — exported function, virtual interface, or abstract base class. |
| [const-correctness-over-escape-hatches](docs/skills/const-correctness-over-escape-hatches.md) | Tempted to add `mutable` / `const_cast` / drop a `const` — apply this decision tree first. |
| [precision-fix-the-formulation](docs/skills/precision-fix-the-formulation.md) | Tempted to widen a `< NEARZERO` / `< EPSILON` check, add an ε fudge, or pick a magic `1e-N` to silence FP-noise speckle / firefly / near-zero misclassification.  Find the cancellation and reformulate. |
| [sms-firefly-diagnosis](docs/skills/sms-firefly-diagnosis.md) | Bright outlier pixels in an SMS render; user reports "fireflies" in an SMS scene. |
| [write-highly-effective-tests](docs/skills/write-highly-effective-tests.md) | Add or strengthen tests; convert smoke tests into strong regression guards; decide whether coverage belongs in `tests/`, `scenes/Tests`, or `tools/`. |

Claude Code auto-discovers these via thin shims under
`.claude/skills/<name>/SKILL.md`.  Other LLM tools should read the
`docs/skills/` directory directly.  See [docs/skills/README.md](docs/skills/README.md)
for the full format and authoring rules.

## Reading Order

1. [README.md](README.md)
2. [docs/README.md](docs/README.md)
3. [src/Library/README.md](src/Library/README.md)
4. [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
5. [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
6. [scenes/README.md](scenes/README.md)
7. [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
8. [scenes/Tests/README.md](scenes/Tests/README.md)
9. [tests/README.md](tests/README.md)
10. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) when touching scene mutability, animation, rasterizers, or thread safety
11. [docs/skills/](docs/skills/) when starting a task whose shape matches one of the skills above
