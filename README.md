# RISE

Contributor map for the Realistic Image Synthesis Engine.

The historical user manual remains in [README.txt](README.txt). This file is the landing page for humans; [AGENTS.md](AGENTS.md) is the shared agent quickstart, [CLAUDE.md](CLAUDE.md) is the Claude-compatible shim, and [docs/README.md](docs/README.md) indexes deeper design notes and roadmaps.

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
3. [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp) loads `.RISEscene` content into the `Job`.
4. The `Job` resolves named assets, configures shaders and rasterizers, and attaches outputs.
5. [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp) drives the render passes.

## Common Commands

```sh
make -C build/make/rise -j8 all
make -C build/make/rise tests
./run_all_tests.sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
./run_scenes.sh
```

If you changed only header files, run a clean rebuild first. The makefile does not track header dependencies reliably.
