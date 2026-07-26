# RISE

Contributor map for the Realistic Image Synthesis Engine.

The frozen 2006 user manual remains in [README.txt](README.txt) for historical
context; its version numbers, platform notes, and commands are not current.
This file is the landing page for humans; [AGENTS.md](AGENTS.md) is the shared
agent quickstart, [CLAUDE.md](CLAUDE.md) is the Claude-compatible shim, and
[docs/README.md](docs/README.md) indexes current references, active plans, and
historical design records.

## Start Here

- Repo and workflow map: [README.md](README.md)
- Agent quickstart and invariants: [AGENTS.md](AGENTS.md)
- Docs catalog and deep dives: [docs/README.md](docs/README.md)
- Scene taxonomy: [scenes/README.md](scenes/README.md)
- Core library overview: [src/Library/README.md](src/Library/README.md)
- Interface taxonomy: [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
- Scene language: [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
- Curated showcase scenes: [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
- Regression and validation scenes: [scenes/Tests/README.md](scenes/Tests/README.md)
- Executable tests: [tests/README.md](tests/README.md)

## Doc Roles

- `README.md`: top-level repo map, canonical read order, and common commands
- `AGENTS.md`: concise working model and invariants for LLM contributors
- `CLAUDE.md`: thin tool-specific companion that points back to the shared docs
- `docs/*.md`: focused deep dives and forward-looking plans
- `src/**/README.md`, `scenes/**/README.md`, `tests/README.md`: subsystem-local reference docs

## Layout

- `src/Library/`: active core rendering library
- `src/RISE/`: main CLI and conversion tools
- `scenes/FeatureBased/`: curated showcase and torture scenes
- `scenes/Tests/`: isolated regression, comparison, and validation scenes
- `tests/`: standalone executable tests
- `docs/`: focused design notes and roadmaps
- `build/make/rise/`: current GNU make build
- `textures/`, `models/`, `colors/`, `volume/`, `lightprobes/`, `media/`: scene data and media
- `tools/`: helper utilities and scripts outside the core runtime
- `src/DRISE/`, `src/PRISE/`, `src/3DSMax/`, `src/Blender/`: sidecar integrations and legacy surfaces

## Main Flow

1. [src/RISE/commandconsole.cpp](src/RISE/commandconsole.cpp) creates a `Job`.
2. [src/Library/Job.cpp](src/Library/Job.cpp) initializes the `Scene`, managers, default null assets, and default shader ops.
3. [src/Library/Cst/Cst.cpp](src/Library/Cst/Cst.cpp) parses `.RISEscene`
   content into the canonical CST and derives it into the `Job`; the chunk
   registry lives in
   [src/Library/Parsers/ChunkParserRegistry.cpp](src/Library/Parsers/ChunkParserRegistry.cpp).
4. The `Job` resolves named assets, configures shaders and rasterizers, and attaches outputs.
5. [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp) drives the render passes.

## Common Commands

### Linux / macOS

```sh
make -C build/make/rise -j8 all
make -C build/make/rise tests
./run_all_tests.sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
./run_scenes.sh
```

### macOS release DMG

The release script performs a clean `RISE-GUI-Opto` Xcode build in fresh
DerivedData, bundles and relinks non-system runtime libraries, signs the app,
and creates a drag-to-Applications DMG plus a dSYM archive and SHA-256 file:

```sh
scripts/create_macos_release.sh
```

The default build targets the host architecture and uses an ad-hoc signature.
For public distribution, provide a Developer ID Application identity and a
`notarytool` keychain profile; the script then signs, notarizes, and staples
the DMG:

```sh
RISE_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
RISE_NOTARY_PROFILE=rise-notary \
scripts/create_macos_release.sh --version 1.2.0 --build 42
```

Run `scripts/create_macos_release.sh --help` for architecture, output, and
working-tree options. Artifacts are written to `dist/macos/` by default.

### Windows

```powershell
cmake -S build/cmake/rise-tests -B build/cmake/rise-tests/_out -A x64
cmake --build build/cmake/rise-tests/_out --config Release --target rise_all_tests --parallel
.\run_all_tests.ps1
.\run_all_tests.ps1 -Config Debug
```

The Windows test flow builds every `tests/*.cpp` via
[build/cmake/rise-tests/CMakeLists.txt](build/cmake/rise-tests/CMakeLists.txt)
and runs them through [run_all_tests.ps1](run_all_tests.ps1). The CMake test
project links against the prebuilt `RISE.lib` from the VS2022 Library project,
so build the Windows library/CLI once first.

If you changed only header files, run a clean rebuild first. The makefile does not track header dependencies reliably.
