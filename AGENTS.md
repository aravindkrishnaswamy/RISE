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
- **Thread priority invariant (LOAD-BEARING)**: `GlobalThreadPool()` is topology-aware — every P-core gets a render worker, every E-core except `render_thread_reserve_count` (default 1) also gets one. Workers use `QOS_CLASS_USER_INITIATED` on macOS and `sched_setaffinity` on Linux/Windows to stay on fast cores. Benchmarks MUST set option `render_thread_reserve_count 0` to use every core — otherwise 1 E-core is reserved for the OS and wall-time looks ~10 % slower. Never put all workers at `QOS_CLASS_UTILITY` / `BELOW_NORMAL` thinking it's "nicer" — the old bug cost 2–4× real throughput on Apple Silicon. Full rationale in [docs/PERFORMANCE.md](docs/PERFORMANCE.md) "Thread priority policy."

## Reference Counting

- Many core classes derive from `Implementation::Reference` and use explicit reference counting.
- Managers usually `addref()` on insertion and release on shutdown.
- Before changing lifetime behavior, inspect [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h), [src/Library/Interfaces/IManager.h](src/Library/Interfaces/IManager.h), and [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h).

## Change Checklist

- New render feature: implement the class, expose it through `RISE_API` if externally constructible, add a `Job` wrapper if needed, register a scene chunk if user-authored, add a sample scene, add a focused test, and update `build/make/rise/Filelist` for new `.cpp` files.
- **New source file anywhere under `src/Library/`**: register it in every IDE/build project so the tree stays consistent across platforms. The Makefile `Filelist` is only one of five places to touch:
  - `build/make/rise/Filelist` — SRCLIB sub-list for `.cpp` (the canonical Unix/Linux build).
  - `build/cmake/rise-android/rise_sources.cmake` — `RISE_LIB_SOURCES` list for `.cpp` (Android NDK build, mirrors `Filelist` SRCLIB by hand).
  - `build/VS2022/Library/Library.vcxproj` — `<ClCompile>` for `.cpp` and `<ClInclude>` for `.h`.
  - `build/VS2022/Library/Library.vcxproj.filters` — same entries with `<Filter>` tags mirroring the existing folder taxonomy.
  - `build/XCode/rise/rise.xcodeproj/project.pbxproj` — `PBXFileReference` + `PBXBuildFile` + group + build-phase entries (Xcode does not auto-discover).
  - Exclusions: `ManagedJob.cpp` (C++/CLI only), Windows-only files (`ThreadsWin32.cpp`, `LoadLibraryWin32.cpp`, `Win32Console.cpp`, `Win32WindowRasterizerOutput.cpp`), and the `Utilities/Communications/*` DRISE socket stack stay out of the Android cmake — everything else should appear in all five projects.
- Scene language change: inspect the parser registry, decide whether `CURRENT_SCENE_VERSION` should change, update representative scenes, and keep [src/Library/Parsers/README.md](src/Library/Parsers/README.md) aligned.
- Sample coverage change: use `scenes/FeatureBased` for curated showcase or torture scenes, and `scenes/Tests` for isolated baselines, feature checks, and regression coverage.

## Build And Test

- Make build: `make -C build/make/rise -j8 all`
- Tests build: `make -C build/make/rise tests`
- Tests run: `./run_all_tests.sh`
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
