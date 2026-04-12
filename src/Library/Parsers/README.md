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

## MIS Weight Parameters (Rasterizer Blocks)

The following parameters control advanced MIS weight computation in the `pixelpel_rasterizer`, `pathtracing_pel_rasterizer`, `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`, and `mlt_rasterizer` blocks. All are disabled by default and fall back to the standard power heuristic.

| Parameter | Rasterizers | Type | Default | Description |
|-----------|-------------|------|---------|-------------|
| `optimal_mis` | PT, BDPT | bool | FALSE | Run training passes to compute variance-minimizing MIS weights for direct illumination (Kondapaneni et al. 2019) |
| `optimal_mis_training_iterations` | PT, BDPT | uint | 4 | Number of 1-SPP training passes |
| `optimal_mis_tile_size` | PT, BDPT | uint | 16 | Spatial tile size for second-moment binning |

## Pure PT Rasterizer Chunks

Two rasterizer chunks bypass the shader-op dispatch chain and call `PathTracingIntegrator` directly:

| Chunk name | Description |
|-----------|------------|
| `pathtracing_pel_rasterizer` | Iterative PT, RGB output, per-sample OIDN AOVs |
| `pathtracing_spectral_rasterizer` | Iterative PT, spectral output, NM or HWSS modes |

These accept all the same parameters as their shader-dispatch counterparts (`pixelpel_rasterizer` and `pixelintegratingspectral_rasterizer`) plus SMS controls (`sms_enabled`, `sms_max_iterations`, `sms_threshold`, `sms_max_chain_depth`, `sms_biased`, `sms_bernoulli_trials`). The pure PT rasterizers do not use or require a `defaultshader` because they bypass the shader pipeline entirely.

When `oidn_denoise TRUE` is set on `pathtracing_pel_rasterizer`, the filtered film resolve is automatically skipped to preserve raw MC noise for OIDN. See `docs/ARCHITECTURE.md` for details.

See `docs/IMPROVEMENTS.md` item 8 for algorithmic details.

## Good Cross-References

- High-level assembly target: [../Job.cpp](../Job.cpp)
- Public API constructors: [../RISE_API.h](../RISE_API.h)
- Scene taxonomy: [../../../scenes/README.md](../../../scenes/README.md)
- Showcase scenes: [../../../scenes/FeatureBased/README.md](../../../scenes/FeatureBased/README.md)
- Regression scenes: [../../../scenes/Tests/README.md](../../../scenes/Tests/README.md)
