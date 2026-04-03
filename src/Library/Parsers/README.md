# Parser Guide

This directory defines the scene, script, command, and options parsers. For most user-visible feature work, `AsciiSceneParser.cpp` is the most important file in the directory.

## Canonical Files

- Scene parser implementation and chunk registry: [AsciiSceneParser.cpp](AsciiSceneParser.cpp)
- Scene parser interface: [../Interfaces/ISceneParser.h](../Interfaces/ISceneParser.h)
- Command parser: `AsciiCommandParser.*`
- Script parser: `AsciiScriptParser.*`
- Math expression evaluator: `MathExpressionEvaluator.*`
- Options parser: [../Options.h](../Options.h) (constructed via `RISE_API_CreateOptionsParser`)

## Scene File Basics

The current parser expects scene files to begin with:

```text
RISE ASCII SCENE 5
```

Chunk syntax rules:

- A chunk is identified by its name on a line by itself.
- The opening `{` must be on its own line.
- The closing `}` must be on its own line.
- Comments are stripped before the chunk-specific parser sees the chunk body.

## Language Features Implemented In The Top-Level Parser

`AsciiSceneParser.cpp` handles more than chunk dispatch:

- Embedded commands with `>`
- Macro definitions with `!`, `define`, or `DEFINE`
- Macro removal with `~`, `undef`, or `UNDEF`
- Arithmetic expressions of the form `$(...)`
- A small set of inline math functions such as `sin`, `cos`, `tan`, `sqrt`, and `hal`
- `FOR` / `ENDFOR` loop expansion
- Block and line comments

If syntax changes in any of those areas, document them here and inspect scene compatibility carefully.

## Registered Chunk Families

The registry in `AsciiSceneParser.cpp` is the authoritative map from scene syntax to implementation. Current families include:

- Painters
- 1D and 2D functions
- Materials
- Cameras
- Geometry
- Modifiers
- Objects
- Shader ops
- Shaders
- Rasterizers
- Rasterizer outputs
- Lights
- Photon map generation and gather settings
- Animation keyframes and timeline options

When in doubt, read the registration block in [AsciiSceneParser.cpp](AsciiSceneParser.cpp).

## Adding A New Scene Chunk

1. Implement the chunk parser class.
2. Register it in the chunk map in `AsciiSceneParser.cpp`.
3. Make sure the feature is constructible through `Job` and or `RISE_API`.
4. Add a representative `.RISEscene` sample.
   Put isolated parser checks in `scenes/Tests`, and reserve `scenes/FeatureBased` for richer showcase scenes.
5. Add a deterministic test if parsing logic or supporting math can be exercised without a full render.
6. Decide whether the scene version marker should change.

## Where Agent Authors Usually Get Tripped Up

- The older comment near the top lists eight primary chunk categories, but the real registry now covers more specialized families. The registration map is the source of truth.
- Parser support is split between top-level syntax handling and per-chunk parsers. A feature may require changes in both places.
- Scene chunk bodies are assembled as strings after token substitution and expression evaluation. If a parameter seems to disappear, inspect macro substitution and token reassembly first.

## Good Cross-References

- High-level assembly target: [../Job.cpp](../Job.cpp)
- Public API constructors: [../RISE_API.h](../RISE_API.h)
- Scene taxonomy: [../../../scenes/README.md](../../../scenes/README.md)
- Showcase scenes: [../../../scenes/FeatureBased/README.md](../../../scenes/FeatureBased/README.md)
- Regression scenes: [../../../scenes/Tests/README.md](../../../scenes/Tests/README.md)
