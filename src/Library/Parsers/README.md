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

For semantic conventions (light direction sign, colour space defaults,
transform precedence on `standard_object`, V-axis flip, alpha-mask
integrator caveats, …), see
[docs/SCENE_CONVENTIONS.md](../../../docs/SCENE_CONVENTIONS.md).
That document is the canonical reference for "why does my scene
render unexpectedly?" — the chunk parsers themselves only enforce
syntactic correctness.

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

## Chunk-Parser Architecture (descriptor-driven)

Every chunk parser derives from `IAsciiChunkParser` ([IAsciiChunkParser.h](IAsciiChunkParser.h)) and overrides exactly two virtual methods:

| Method | Purpose |
|--------|---------|
| `Describe()` | Returns a `ChunkDescriptor` enumerating every parameter the chunk accepts (name, kind, enum values, reference categories, defaults, descriptions). The descriptor IS the parser's accepted-parameter set. |
| `Finalize(const ParseStateBag&, IJob&)` | Reads typed values out of the bag and emits the corresponding `pJob.AddX(...)` / `pJob.SetX(...)` call. |

No chunk parser overrides `ParseChunk` directly. The default `ParseChunk` impl (in [AsciiSceneParser.cpp](AsciiSceneParser.cpp), just after `CreateAllChunkParsers`) walks the input lines, validates each name against `Describe().parameters`, stores matched values in a `ParseStateBag`, then invokes `Finalize` to emit the AddX call. An input parameter whose name is not in the descriptor fails the parse.

**The invariant:** drift between "what the parser parses" and "what the descriptor advertises" is structurally impossible. Both are read from the same `ChunkDescriptor`. The same descriptor feeds:

- the parser (via `DispatchChunkParameters` in [AsciiSceneParser.cpp](AsciiSceneParser.cpp))
- the syntax highlighters (Qt + AppKit, via `SceneGrammar` in [SceneEditorSuggestions/](../SceneEditorSuggestions/))
- the scene-editor suggestion engine (right-click context menu and inline autocomplete in both GUI apps)
- any future grammar consumer (linters, doc generators, …)

`Describe()` and `Finalize()` are both `[[nodiscard]] = 0` pure virtual on `IAsciiChunkParser`, so a chunk parser cannot ship without both — the build fails until they're implemented.

### `ParseStateBag` accessor reference

Defined in [ChunkDescriptor.h](ChunkDescriptor.h). Read parameter values inside `Finalize` via:

| Method | Returns |
|--------|---------|
| `GetString(key, def)` | `std::string` |
| `GetDouble(key, def)` | `double` |
| `GetUInt(key, def)` | `unsigned int` |
| `GetInt(key, def)` | `int` |
| `GetBool(key, def)` | `bool` |
| `GetVec3(key, double[3])` | `bool` (true if present, fills the array) |
| `Has(key)` | `bool` (was the param explicitly set?) |
| `GetRepeatable(key)` | `const std::vector<std::string>&` (for `p.repeatable = true` params) |
| `Singles()` | `const std::map<std::string,std::string>&` (raw access; useful for composite tokens) |

The default in each `GetX` accessor matches the legacy local-variable initial value used by the pre-migration parser. Use `bag.Has(key)` when you need to distinguish "parameter was explicitly set" from "parameter is absent" (e.g. for unit conversions like `DEG_TO_RAD` that should only apply on explicit input).

## Registered Chunk Families

The registry in `CreateAllChunkParsers()` ([AsciiSceneParser.cpp](AsciiSceneParser.cpp)) is the authoritative map from scene syntax to implementation. Current families:

| Family | Count | Examples |
|--------|-------|----------|
| Painters | 27 | `uniformcolor_painter`, `image_painter`, `voronoi3d_painter`, `iridescent_painter` |
| Functions | 2 | `piecewise_linear_function`, `piecewise_linear_function2D` |
| Materials | 23 | `lambertian_material`, `dielectric_material`, `ggx_material`, `composite_material` |
| Cameras | 5 | `pinhole_camera`, `onb_pinhole_camera`, `thinlens_camera`, `fisheye_camera`, `orthographic_camera` |
| Geometry | 15 | `sphere_geometry`, `mesh3DS_geometry`, `displaced_geometry`, `bezier_patch_geometry` |
| Modifiers | 1 | `bumpmap_modifier` |
| Media | 3 | `homogeneous_medium`, `heterogeneous_medium`, `painterheterogeneous_medium` |
| Objects | 2 | `standard_object`, `csg_object` |
| Shader ops | 12 | `pathtracing_shaderop`, `final_gather`, `direct_lighting`, `area_light_shaderop` |
| Shaders | 4 | `standard_shader`, `advanced_shader`, `direct_volume_rendering_shader` |
| Rasterizers | 10 | `pixelpel_rasterizer`, `pathtracing_pel_rasterizer`, `mlt_spectral_rasterizer` |
| Rasterizer outputs | 1 | `file_rasterizer_output` |
| Lights | 4 | `ambient_light`, `omni_light`, `spot_light`, `directional_light` |
| Photon maps | 12 | 6 generate + 6 gather (caustic / global / shadow / translucent — Pel and spectral) |
| Irradiance cache | 1 | `irradiance_cache` |
| Animation | 3 | `keyframe`, `timeline`, `animation_options` |

**Total: 124 chunk parsers, 125 chunk keywords** (`mis_pathtracing_shaderop` shares an implementation class with `pathtracing_shaderop`). Read `CreateAllChunkParsers()` in [AsciiSceneParser.cpp](AsciiSceneParser.cpp) for the canonical list.

`realistic_camera` is intentionally **not** registered — the keyword is reserved for the future multi-element lens-system camera (see [docs/CAMERAS_ROADMAP.md](../../../docs/CAMERAS_ROADMAP.md) Phase 4). Until that lands, scenes that want photographic depth-of-field use `thinlens_camera`.

## `thinlens_camera` Sensor-Format Presets

### Unit story (read this first)

`thinlens_camera` has three lengths — `sensor_size`, `focal_length`, `focus_distance` — and **all three must be in the same unit**, which must also match the unit of your scene geometry. The FOV formula `2 * atan(sensor_size / (2 * focal_length))` is unit-free (the ratio is what matters), but the lens equation `v = f·u/(u−f)` used for depth-of-field is *not* — `focal_length` and `focus_distance` are subtracted, which only makes sense in matching units.

`focus_distance` is **required** with no default — you must set it to the focus-plane distance in your scene's unit. The parser also enforces `focus_distance > focal_length` (otherwise the lens equation gives a negative film distance — physically a virtual image, no real photograph).

The presets below are the **mm-equivalent** sensor widths. They work directly when scene geometry is in mm. For other scene units, scale all three lengths by the same factor:

| Scene unit | Full-frame `sensor_size` | "Natural" `focal_length` | Example `focus_distance` |
|---|---|---|---|
| millimetres (default) | 36 | 35 | 1000 (i.e. 1 m focus) |
| centimetres | 3.6 | 3.5 | 100 |
| metres | 0.036 | 0.035 | 1 |

The aperture diameter is derived as `focal_length / fstop` and inherits the same unit. `fstop` is dimensionless. Aperture-shape parameters (`aperture_blades`, `aperture_rotation`, `anamorphic_squeeze`) shape the bokeh; defaults give a perfect circular disk.

### Sensor format presets (mm-equivalent — scale per the table above for other scene units)

| Format | sensor_size (mm) | Notes |
|---|---|---|
| **Full-frame 35mm still** *(default)* | 36.0 | 24×36 — Leica / DSLR / mirrorless |
| APS-C (Sony/Nikon/Fuji) | 23.6 | 15.7×23.6 |
| APS-C (Canon) | 22.3 | 14.9×22.3 |
| Super 35 (cinema) | 24.89 | 18.66×24.89 — most-common digital cinema sensor |
| Micro Four Thirds | 17.3 | 13×17.3 |
| Vista Vision | 37.72 | 25.17×37.72 — 8-perf 35mm cinema |
| IMAX 70mm | 70.41 | 52.63×70.41 — 15-perf |
| 645 medium format | 56.0 | 41.5×56 |
| 6×7 medium format | 70.0 | 56×70 |
| 6×9 medium format | 84.0 | 56×84 |
| 4×5 large format | 121.0 | 96×121 sheet film |
| 8×10 large format | 254.0 | 203×254 sheet film |

## Adding A New Chunk Parser

1. **Implement the parser class** in the `RISE::Implementation::ChunkParsers` namespace inside [AsciiSceneParser.cpp](AsciiSceneParser.cpp). Convention: place it next to other parsers in the same family. Skeleton:

   ```cpp
   struct MyAsciiChunkParser : public IAsciiChunkParser
   {
       bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
       {
           std::string name = bag.GetString( "name", "noname" );
           double      x    = bag.GetDouble( "x",    0.0 );
           // ... read every parameter from the bag with the
           //     default the user expects when omitted.
           return pJob.AddMyThing( name.c_str(), x, ... );
       }

       const ChunkDescriptor& Describe() const override
       {
           static const ChunkDescriptor d = []{
               ChunkDescriptor cd;
               cd.keyword     = "my_chunk";
               cd.category    = ChunkCategory::Painter; // or whichever
               cd.description = "What this chunk does.";
               auto P = [&cd]() -> ParameterDescriptor& {
                   cd.parameters.emplace_back();
                   return cd.parameters.back();
               };
               { auto& p = P(); p.name = "name"; p.kind = ValueKind::String;
                 p.description = "Unique name"; p.defaultValueHint = "noname"; }
               { auto& p = P(); p.name = "x";    p.kind = ValueKind::Double;
                 p.description = "What x means"; p.defaultValueHint = "0"; }
               return cd;
           }();
           return d;
       }
   };
   ```

2. **Register it** in `CreateAllChunkParsers()` (in the same file): `add("my_chunk", new MyAsciiChunkParser());`

3. **Make the feature constructible** through `Job` and/or `RISE_API` if it isn't already. The `pJob.AddMyThing(...)` call must exist.

4. **Add a representative `.RISEscene` sample.** Use `scenes/Tests/` for isolated parser checks and `scenes/FeatureBased/` for richer showcase scenes.

5. **Add a deterministic test** if the parsing or supporting math can be exercised without a full render.

6. **Decide whether `CURRENT_SCENE_VERSION` should change** if the new chunk introduces a backwards-incompatible scene change.

The chunk now appears automatically in:
- syntax highlighting on Windows + macOS
- the right-click context menu and inline autocomplete in both GUI scene editors
- any future grammar consumer

No second site to update.

## Adding A Parameter To An Existing Chunk

1. **Add one descriptor entry** in that parser's `Describe()`:

   ```cpp
   { auto& p = P(); p.name = "new_param"; p.kind = ValueKind::Double;
     p.description = "What it does"; p.defaultValueHint = "0.5"; }
   ```

   - Set `p.repeatable = true` for parameters that accept multiple values (e.g. `cp` control points, `shaderop` entries).
   - Populate `p.enumValues = {"a","b","c"}` for `ValueKind::Enum`.
   - Populate `p.referenceCategories = {ChunkCategory::Material, ChunkCategory::Painter}` for `ValueKind::Reference` so the suggestion engine can offer completions of the right type.

2. **Read it in `Finalize`**: `double y = bag.GetDouble("new_param", 0.5);` (default must match the descriptor's hint — and match what the user expects when the param is omitted).

3. **Pass it through** to the appropriate `pJob.AddX(...)` / `pJob.SetX(...)` call.

That's it. The new parameter automatically appears in:
- the right-click context menu when the cursor is inside a `my_chunk` block
- inline autocomplete after typing a partial param name
- syntax-highlighter tooltips (when implemented)

## Removing A Parameter

1. Delete the descriptor entry. Every consumer updates in lock-step.

2. **Backwards compatibility**: if existing scene files might still set the parameter, keep the descriptor entry and set its description to `"Legacy — ignored"`; skip reading it in `Finalize`. The dispatcher will accept the parameter; `Finalize` ignores it.

## Helper Templates

Parameter sets shared across many chunks live in [AsciiSceneParser.cpp](AsciiSceneParser.cpp) just after `DispatchChunkParameters` and just before the painter parsers. Reuse them rather than copy-pasting:

| Helper | Used by | Adds |
|--------|---------|------|
| `AddCameraCommonParams` | All 5 cameras | `location`, `lookat`, `up`, `width`, `height`, `pixelAR`, `exposure`, `pitch`, `yaw`, `roll`, `target_orientation`, … |
| `AddPixelFilterParams` | All 10 rasterizers | `pixel_filter`, `pixel_filter_width`, `pixel_filter_height`, `pixel_filter_paramA/B` |
| `AddSpectralCoreParams` | All 5 spectral rasterizers | `spectral_samples`, `nmbegin`, `nmend`, `num_wavelengths`, `hwss` |
| `AddSpectralRGBSpdParams` | `pixelintegratingspectral_rasterizer` only | `integrate_rgb`, `rgb_spd`, `rgb_spd_wavelengths`, `rgb_spd_r/g/b` |
| `AddStabilityConfigParams` | All non-MLT rasterizers | `direct_clamp`, `indirect_clamp`, `rr_min_depth`, `rr_threshold`, `max_*_bounce`, `light_bvh`, `branching_threshold` |
| `AddPathGuidingParams` | PT/BDPT pel + PT spectral | `pathguiding`, `pathguiding_iterations`, `pathguiding_spp`, `pathguiding_alpha`, `pathguiding_max_depth`, … |
| `AddAdaptiveSamplingParams` | PT/BDPT pel + PT spectral | `adaptive_sampling`, `adaptive_threshold`, `adaptive_min_samples`, `adaptive_max_samples` |
| `AddRadianceMapParams` | `pixelpel_rasterizer` | `radiance_map`, `radiance_scale`, `radiance_orient` |
| `AddProgressiveParams` | All progressive rasterizers | `progressive_samples_per_pass` |
| `AddSMSConfigParams` | PT pel + PT spectral + BDPT spectral | `sms_enabled`, `sms_max_iterations`, `sms_threshold`, `sms_max_chain_depth`, `sms_biased`, `sms_bernoulli_trials`, `sms_multi_trials`, `sms_photon_count` |
| `AddOptimalMISParams` | PT pel + BDPT pel | `optimal_mis`, `optimal_mis_training_iterations`, `optimal_mis_tile_size` |
| `AddBaseRasterizerParams` | All rasterizers | `defaultshader`, `oidn_denoise`, `show_luminaires`, … |
| `AddNoisePainterCommonParams` | All Perlin/Worley/Simplex/Gabor 3D painters | `frequency`, `octaves`, `persistence`, `lacunarity`, `seed`, … |
| `AddPhotonMapGenerateCommonParams` | All 6 photon-map generate parsers | `num`, `power_scale`, `max_recursion`, `min_importance`, `branch`, `reflect`, `refract`, … |
| `AddPhotonMapGatherCommonParams` | All 6 photon-map gather parsers | `radius`, `ellipse_ratio`, `min_photons`, `max_photons` |

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
