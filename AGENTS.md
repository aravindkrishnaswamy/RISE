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
- `Job::SetPrimaryAcceleration()` replaces the object manager. Calling it after adding objects discards them. Default since 2026-05 is **top-level BVH4** (SAH-binned, BVH4 SIMD collapse — same `BVH<>` template as the per-mesh accelerator) with leaf cap 4 and depth cap 32. Pre-2026-05 was no top-level structure (linear loop). Constructor flag `bUseBSPtree` is the historical name; semantically it now means "build a top-level BVH" (the BSPTreeSAH path was removed when ObjectManager moved to `BVH<>`). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) "Top-Level Acceleration (TLAS)" and the BVH retrospective Tier D1 entry.
- Most scene elements are named and stored in managers. Parser chunks usually resolve dependencies by name through those managers.
- `Job::InitializeContainers()` installs `"none"` defaults for a null material and null painter, and also creates several default shader ops.
- The scene parser currently expects the header `RISE ASCII SCENE 6`. v5 scenes (legacy: width/height/pixelAR authored inside camera chunks) must be migrated — see the `Build And Test` → `Migrate a v5 scene to v6` recipe below for the script invocation, properties (idempotent, multi-camera handling, macro/CRLF preservation), and when NOT to use it. The parser emits a clear error pointing at the same recipe if it loads a v5 scene.
- In `.RISEscene` files, chunk braces must appear on their own lines.
- Parser support for macros, math expressions, embedded commands, and `FOR` / `ENDFOR` loops is implemented centrally in `AsciiSceneParser.cpp`.
- Tests are standalone executables, not a framework-based suite.
- Two rendering pipelines exist: **shader-dispatch** (RayCaster → ShaderOp chain) and **pure integrator** (PathTracingIntegrator called directly by `pathtracing_pel_rasterizer` / `pathtracing_spectral_rasterizer`). Changes to path tracing logic should go in `PathTracingIntegrator`, which serves both pipelines.
- **OIDN + FilteredFilm invariant**: When OIDN denoising is enabled, the filtered film resolve is skipped. OIDN needs raw MC noise, not filter-reconstructed images. See `docs/ARCHITECTURE.md` for details.
- **Interactive viewport parity invariant**: macOS (`build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm`), Windows (`build/VS2022/RISE-GUI/ViewportBridge.cpp`), and Android (`android/app/src/main/cpp/RiseBridge.cpp`) must use the same interactive render pipeline. Shared preview shading / AO / rasterizer behavior belongs in `src/Library/Rendering/InteractivePelRasterizer.{h,cpp}` (currently `CreateInteractiveMaterialPreviewPipeline`) rather than platform bridges. Platform bridge code should only adapt frame delivery and UI lifecycle.
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
- **Integrator wavelength variants — check ALL three (Pel / NM / HWSS) on any fix or feature**: integrators (PT, BDPT, VCM, MLT, …) are implemented as parallel RGB-per-lobe (`Pel`, e.g. `IntegrateFromHit`), single-wavelength (`NM`, e.g. `IntegrateFromHitNM`), and hero-wavelength spectral sampling (`HWSS`, e.g. `IntegrateFromHitHWSS`) variants in the same file. They diverge in subtle ways — e.g. NM and HWSS sites must call `RandomlySelect(xi, true)` so lobe selection uses spectral `krayNM` weights matching the spectral selectProb compensation, while Pel uses `RandomlySelect(xi, false)` for RGB max-channel selection — so a correctness bug or new feature almost never applies identically to all three but almost always applies to MORE than one. When touching any single variant, grep for the matching code in the other two (same function-name stem, parallel structure) and explicitly decide for each: does this fix/feature apply here too, does it need a variant-specific form, or is it genuinely inapplicable? Document the answer for the inapplicable cases so the next agent doesn't re-audit. Prior incidents (both fixed in 2026-05): (1) missing `/selectProb` division at multi-lobe scatter sites in PT's iterative path biased RGB renders dim and amplified RR fireflies; (2) NM/HWSS sites called `RandomlySelect(xi, false)` (RGB-domain selection) while compensating with spectral `selectProb` — biased the spectral estimator at every multi-lobe vertex.  Both were caught by adversarial review after path-tree branching was excised.
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

### Render test scenes at lower resolution (CLI override)

Production scenes often author 1920×1080 (or higher) in their `film`
chunk or camera-chunk dims; rendering at full res for an iteration cycle
is slow.  Override the resolution at the command line — no scene-file
edit required:

```sh
# Linux/macOS
./bin/rise --width 480 --height 270 scenes/Tests/Geometry/shapes.RISEscene
```

```powershell
# Windows
.\bin\RISE-CLI.exe --width 480 --height 270 scenes\Tests\Geometry\shapes.RISEscene
```

Flags:

- `--width N` — image width in pixels (positive integer)
- `--height N` — image height in pixels (positive integer)
- `--pixel-ar X` — pixel aspect ratio (positive float; 1.0 = square)

Partial overrides preserve the non-overridden axes from whatever the
scene file installed.  Order does not matter — the flag scanner accepts
`RISE-CLI scene.RISEscene --width 480 --height 270` and
`RISE-CLI --width 480 --height 270 scene.RISEscene` identically.

If the scene file omits a `film` chunk and the camera chunk has no
explicit `width`/`height`, the default qHD (960 × 540, square pixels)
applies — the agent doesn't have to author anything for a fast preview.
Background and full design are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) "Camera / Film / Output
Separation".

### Migrate a v5 scene to v6

If the parser fails to load a scene with an error mentioning "Scene is
version 5" or you're handed a `.RISEscene` that still authors
`width` / `height` / `pixelAR` inside a `pinhole_camera` (or any other
camera) chunk, run the migration tool:

```sh
# One file
python tools/migrate_scenes_v5_to_v6.py path/to/scene.RISEscene

# A whole directory (recursive; modifies in place)
python tools/migrate_scenes_v5_to_v6.py path/to/dir/

# Default: migrate every scene under scenes/
python tools/migrate_scenes_v5_to_v6.py
```

What the script does to each v5 file:

1. Bumps the header from `RISE ASCII SCENE 5` → `RISE ASCII SCENE 6`.
2. Lifts every `width` / `height` / `pixelAR` line out of every
   `pinhole_camera`, `onb_pinhole_camera`, `thinlens_camera`,
   `fisheye_camera`, and `orthographic_camera` chunk.
3. Emits a top-level `film { width … height … pixelAR … }` chunk
   immediately before the first camera chunk in the file.
4. **Multi-camera files**: if multiple cameras authored different dims,
   the LAST camera's values win in the emitted `film` chunk (matches the
   v5 parser's last-write-wins semantics, so renders stay
   bit-equivalent to pre-migration).

Properties to rely on:

- **Idempotent.** Running it on a v6 scene (or a v5 scene that already
  has a `film` chunk and no camera-chunk dims) is a no-op.
- **Preserves macros, math expressions (`$(...)`), `@FOO` references,
  and CRLF vs LF line endings.** It edits only the lines it relocates;
  every other byte is left alone.
- **Never touches an existing `film` chunk.** Files that already have
  one get the version bump only.

When NOT to use it:

- The scene already loads cleanly — leave it alone.
- You're authoring a new scene from scratch — write a v6 header and a
  `film` chunk directly; no need to run the migrator.
- The "scene" is actually a glTF / OBJ / 3DS file — the migrator only
  handles `.RISEscene` text.

After running, verify with one short render:

```sh
printf "render\nquit\n" | ./bin/rise --width 320 --height 180 path/to/scene.RISEscene
```

`git diff --shortstat` gives you the blast radius.  Full details in
[docs/SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md) §8.5 ("The `film`
chunk").

> **Render renders sequentially, never in parallel.** A single `./bin/rise`
> render is already an embarrassingly-parallel job that takes every CPU core
> (>700 % CPU on a typical workstation). Stacking two renders concurrently
> thrashes the machine to a crawl and makes other work impossible. For A/B
> comparisons or sweeps, chain renders in a single shell sequence
> (`render A; render B`) — never start a second render while a first is still
> running, even if the first is in the background. This applies whether you
> launch with `Bash run_in_background:true`, a separate terminal, or a script
> that invokes `bin/rise` in parallel — always one at a time.

## Compiler Warnings Are Bugs

This codebase treats compiler warnings as bugs.  Do not introduce them
and do not leave them behind when you finish a task.

- **Do not suppress.**  No `#pragma GCC diagnostic ignored`, no
  `#pragma clang diagnostic push`, no `-Wno-foo` added to any build
  file's compile flags, no `(void)unused_var` to silence "unused"
  unless the variable is genuinely intentional dead state (and even
  then, prefer deleting it).  The compiler is right; suppressing it
  is hiding the symptom.
- **Fix the root cause.**  A warning means the compiler found something
  worth your attention.  Examples of the principled fix per warning kind:
  - `-Winconsistent-missing-override` → add `override` to every method
    that overrides a base class virtual.  This is not cosmetic — it
    catches future signature drift between base and derived classes
    at compile time, which is exactly the kind of latent bug RISE has
    been bitten by historically.
  - `-Wunused-function` / `-Wunused-variable` → delete the dead code.
    Git history preserves it if you ever need to resurrect.  Prefer
    deletion over `[[maybe_unused]]`; the latter is for genuinely-
    intentional dead state (e.g., variables kept for debugging).
  - `-Wsign-compare` → fix the type mismatch.  Don't cast.  If a
    container's `size()` is being compared against a signed counter,
    change the counter to `size_t` (or use a range-for).
  - `-Wimplicit-fallthrough` → add `[[fallthrough]];` if intentional,
    `break;` if not.  Don't suppress.
  - `-Wshadow` → rename one of the shadowed variables.  Almost always
    a legit readability concern.
  - `-Wreorder` → fix the member-init order to match declaration order.
  - `-Wdeprecated-declarations` → migrate to the non-deprecated API.
- **Verify before you finish.**  Both the make build and the Xcode build
  must come up warning-free on a clean rebuild:

  ```sh
  # make build
  make -C build/make/rise clean
  make -C build/make/rise -j8 all 2>&1 | grep "warning:"   # expect empty

  # Xcode RISE-GUI build
  cd build/XCode/rise
  xcodebuild -project rise.xcodeproj -scheme RISE-GUI -configuration Deployment clean
  xcodebuild -project rise.xcodeproj -scheme RISE-GUI -configuration Deployment build 2>&1 | grep "warning:" | grep -v appintentsmetadataprocessor   # expect empty
  ```

  Incremental builds will hide warnings on files that didn't recompile.
  When you've finished a task that touched headers or shared sources,
  always do at least one clean rebuild to surface warnings the
  incremental build missed.
- **Third-party headers are out of scope.**  System headers (libc++,
  Foundation, etc.) and bundled dependencies (`opt/homebrew/...`) sometimes
  emit warnings the compiler attributes to RISE source via include chains.
  Filter these out by path; don't try to fix them.

## Noise To Ignore

- `.claude/` is local agent workspace state, not project source.
- `.agents/` is local agent workspace state, not project source.
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
| [effective-rise-scene-authoring](docs/skills/effective-rise-scene-authoring.md) | Author a new `.RISEscene`, port from another tool (glTF / Blender / Unity / Unreal), or diagnose a scene that renders unexpectedly dark / wrong-coloured / oriented backwards.  Walks the [SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md) checklist + Lambertian-control-sphere diagnostic. |

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
