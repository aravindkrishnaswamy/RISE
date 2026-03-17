# RISE

Modern repo guide for the Realistic Image Synthesis Engine.

The historical user manual remains in [README.txt](README.txt). This file is the current contributor map; [AGENTS.md](AGENTS.md) is the agent-specific companion.

## Read First

- Core architecture: [src/Library/README.md](src/Library/README.md)
- Interface taxonomy: [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
- Scene language: [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
- Sample scenes: [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
- Tests: [tests/README.md](tests/README.md)

## Layout

- `src/Library/`: active core rendering library
- `src/RISE/`: main CLI and conversion tools
- `scenes/FeatureBased/`: canonical feature samples
- `tests/`: standalone executable tests
- `build/make/rise/`: current GNU make build
- `textures/`, `models/`, `colors/`, `volume/`, `lightprobes/`: scene data and media
- `src/DRISE/`, `src/PRISE/`, `src/3DSMax/`: legacy sidecars, preserved but not primary development surfaces

## Main Flow

1. [src/RISE/commandconsole.cpp](src/RISE/commandconsole.cpp) creates a `Job`.
2. [src/Library/Job.cpp](src/Library/Job.cpp) builds the `Scene` and managers.
3. [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp) loads `.RISEscene` content into the `Job`.
4. The `Job` resolves named assets, configures shaders and rasterizers, and attaches outputs.
5. [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp) drives the render passes.

## Common Commands

```sh
make -C build/make/rise all
make -C build/make/rise tests
./run_all_tests.sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/Geometry/shapes.RISEscene
./run_scenes.sh
```
