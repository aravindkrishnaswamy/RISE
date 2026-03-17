# RISE Agent Guide

Navigation aid for LLM-powered contributors. Use [README.txt](README.txt) for historical user context and [README.md](README.md) for the repo map.

## Start Here

- Public external construction API: [src/Library/RISE_API.h](src/Library/RISE_API.h)
- High-level convenience interface: [src/Library/Interfaces/IJob.h](src/Library/Interfaces/IJob.h)
- Main assembly implementation: [src/Library/Job.cpp](src/Library/Job.cpp)
- Runtime scene object: [src/Library/Scene.h](src/Library/Scene.h)
- Scene syntax and chunk registry: [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp)
- Pixel render loop: [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
- Main CLI entry point: [src/RISE/commandconsole.cpp](src/RISE/commandconsole.cpp)

Active work is mainly in `src/Library`, `src/RISE`, `scenes/FeatureBased`, `tests`, and `build/make/rise`. `src/DRISE`, `src/PRISE`, `src/3DSMax`, and `src/RISE/risempi.cpp` are legacy sidecars.

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

## Reference Counting

- Many core classes derive from `Implementation::Reference` and use explicit reference counting.
- Managers usually `addref()` on insertion and release on shutdown.
- Before changing lifetime behavior, inspect [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h), [src/Library/Interfaces/IManager.h](src/Library/Interfaces/IManager.h), and [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h).

## Change Checklist

- New render feature: implement the class, expose it through `RISE_API` if externally constructible, add a `Job` wrapper if needed, register a scene chunk if user-authored, add a sample scene, add a focused test, and update `build/make/rise/Filelist` for new `.cpp` files.
- Scene language change: inspect the parser registry, decide whether `CURRENT_SCENE_VERSION` should change, update representative scenes, and keep [src/Library/Parsers/README.md](src/Library/Parsers/README.md) aligned.
- Sample coverage change: prefer `scenes/FeatureBased` and keep scenes demonstrative unless they are intentionally stress-oriented.

## Build And Test

- Make build: `make -C build/make/rise all`
- Tests build: `make -C build/make/rise tests`
- Tests run: `./run_all_tests.sh`
- Batch scene render: `./run_scenes.sh`
- Sample scene render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/Geometry/shapes.RISEscene
```

## Noise To Ignore

- `.claude/` is local agent workspace state, not project source.
- `rendered/` is output, not source.
- `.DS_Store` files are incidental.
- Untracked `*.o` files or `* 2.o` files under `src/Library` are local build artifacts and not source-of-truth code.
- `build/make/rise/RISELog.txt`, `build/make/rise/build_log.txt`, and `RISE_Log.txt` are generated logs.
- `/.vscode/`, `Xcuserdata/`, and `Config.specific` are local IDE or platform artifacts.
- `*.mov` files are rendered video output.

## Reading Order

1. [README.md](README.md)
2. [src/Library/README.md](src/Library/README.md)
3. [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
4. [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
5. [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
6. [tests/README.md](tests/README.md)
