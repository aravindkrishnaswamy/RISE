# Parser Guide

This directory defines the chunk-parser registry, script, command, and options parsers. For most user-visible feature work, `ChunkParserRegistry.cpp` (the chunk-parser registry) is the most important file in the directory.

> **Historical note (Model-B CST cutover, 2026-07):** the legacy streaming scene parser (`AsciiSceneParser.cpp` — `ParseAndLoadScene` + the `>`/macro/`FOR`/`> load` language below) has been **retired and deleted**. Scenes now load exclusively through the canonical CST path: `Cst::ParseToCst` → `Cst::DeriveToJob`, which drives the SAME descriptor-driven chunk parsers via the shared registry. The chunk-parser registry (`CreateAllChunkParsers` / `DispatchChunkParameters` / the default `ParseChunk` / every `IAsciiChunkParser` subclass / the helper templates) was relocated to `ChunkParserRegistry.cpp` (slice 6b) and SURVIVES; only the streaming front-end and its top-level language (next section) are gone. The corpus was flattened to native-v7 by the migrator, so `FOR`/`> load`/`> run`/macros no longer appear in tracked scenes.

## Canonical Files

- Chunk-parser registry (the authoritative scene-syntax → implementation map): [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp)
- Derive-time scene loading (the ONLY scene-load path): `../Cst/Cst.cpp` (`ParseToCst` + `DeriveToJob`)
- Command parser: `AsciiCommandParser.*` (the console `load`/`run`/`modify` commands — `ICommandParser`)
- Script parser: `AsciiScriptParser.*`
- Legacy expression evaluation for the v6 migration tool: [LegacyMathExpression.h](../../../tools/LegacyMathExpression.h)
- Options parser: [../Options.h](../Options.h) (constructed via `RISE_API_CreateOptionsParser`)

## Scene File Basics

The current parser expects scene files to begin with:

```text
RISE ASCII SCENE 7
```

`7` is the post-CST-cutover header that new output (save + the migrator) emits. The
CST loader is version-agnostic, so the transitional `6` header is still accepted on
read — un-migrated scenes keep loading.

Chunk syntax rules:

- A chunk is identified by its name on a line by itself.
- The opening `{` must be on its own line.
- The closing `}` must be on its own line.
- Comments are stripped before the chunk-specific parser sees the chunk body.
- This is enforced, not just documented (since task_7f42984d): a `{`/`}` sharing a line with the
  keyword or a parameter is a hard PASS-1 derive error ("chunk braces must be on their own lines",
  naming the source line) via `ChunkBraceViolations` in `Cst.cpp`'s `ResolveChunkParams` -- not a
  silently-accepted second syntax. `ParseToCst` itself stays lossless/permissive about the bytes
  (an editor must still be able to open a file that violates this), so the rejection happens at
  derive time, the same PASS-1 layer that rejects an unknown chunk type or a value-less parameter.

Numeric descriptor fields use checked conversion. Missing fields retain their
specified defaults. Non-numeric `Int` and `Double` values, non-finite doubles,
and integers outside the destination range fail derivation. `UInt .5` fails
because it has no decimal integer prefix. Explicit signs and historical
integer-prefix conversion remain supported where the descriptor's numeric
syntax accepts them. Values above `UINT_MAX` fail instead of wrapping or
reading an uninitialized value.

`spectral_painter` and `piecewise_linear_function` use the same table rule.
Inline `cp` entries require two finite, whitespace-separated values and can
include additional columns. File-backed tables consume the leading required
values, ignore additional columns, and warn and skip headers or malformed
rows. `nmfile` and `ampfile` consume the first finite value on each physical
row. Empty results, embedded NUL bytes, file I/O failures, and mismatched
wavelength/amplitude counts are errors.

Console commands and ASCII scripts recognize a standalone unquoted `#`
token as a trailing comment and any leading `#` token as a full-line comment.
Quoted hashes, embedded hashes, and value tokens such as `#3` and `#Dir`
remain literal. Options and the public tokenizer APIs do not strip comments. Malformed numeric command arguments,
incomplete render rectangles, and unknown `mediapath` subcommands are hard
errors. A plain `render # comment` renders the current frame.

Note on light colour (2026-09-02): `omni_light`, `spot_light`,
`directional_light` and `ambient_light` each take a `colorspace`
parameter, defaulting to `Rec709RGB_Linear` — their `color` is a LINEAR
triple, matching `uniformcolor_painter`.  They used to gamma-decode it as
sRGB with nothing in the descriptor saying so; pre-existing scenes are
carried across by `tools/migrate_scenes_light_colorspace.py`.

For semantic conventions (light direction sign, colour space defaults,
transform precedence on `standard_object`, V-axis flip, alpha-mask
integrator caveats, …), see
[docs/SCENE_CONVENTIONS.md](../../../docs/SCENE_CONVENTIONS.md).
That document is the canonical reference for "why does my scene
render unexpectedly?" — the chunk parsers themselves only enforce
syntactic correctness.

## Language Features Of The (RETIRED) Top-Level Streaming Parser

**These features are GONE.** The legacy streaming parser (`AsciiSceneParser.cpp`) implemented a top-level scene language on top of chunk dispatch:

- Embedded commands with `>` (incl. `> load` / `> run` recursive includes)
- Macro definitions with `!`, `define`, or `DEFINE`
- Macro removal with `~`, `undef`, or `UNDEF`
- Arithmetic expressions of the form `$(...)`
- A small set of inline math functions such as `sin`, `cos`, `tan`, `sqrt`, and `hal` (a stateful QMC Halton sequence)
- `FOR` / `ENDFOR` loop expansion
- Block and line comments

The whole streaming front-end was **deleted in the Model-B CST cutover** (slice 6c). Native-v7 scenes do not use `>`/macros/`FOR`/`> load`; the migrator flattened the corpus. The CST derive path (`Cst.cpp`) supports a separate, smaller `expr( ... )` value sublanguage (arithmetic + `let`-bound named constants) — NOT the legacy `$(...)`/`hal`/`FOR` machinery. Block and line comments are handled by the CST tokenizer (`ParseToCst`).

### Migrating an out-of-tree legacy scene

For a v5 scene, first move camera dimensions into a `film` chunk:

```sh
python tools/migrate_scenes_v5_to_v7.py path/to/scene.RISEscene
```

That script only performs the camera/film rewrite. If the file also contains
retired includes, macros, `$(...)`, `hal()`, or `FOR` loops, build and run the
canonical CST migrator, which folds those constructs into native declarative
scene text:

```sh
make -C build/make/rise MigrateScenesV6toV7
bin/tools/MigrateScenesV6toV7 path/to/scene.RISEscene > /tmp/scene.RISEscene
mv /tmp/scene.RISEscene path/to/scene.RISEscene
```

Despite its historical v6-to-v7 name, the migrator's body transform is
header-blind and can finish a v5 file after the film script has stamped it
v7. It writes migrated text to stdout, so always redirect to a separate file;
never redirect onto the input path. Inspect the diff and perform a short render
after migration.

## Chunk-Parser Architecture (descriptor-driven)

Every chunk parser derives from `IAsciiChunkParser` ([IAsciiChunkParser.h](IAsciiChunkParser.h)) and overrides exactly two virtual methods:

| Method | Purpose |
|--------|---------|
| `Describe()` | Returns a `ChunkDescriptor` enumerating every parameter the chunk accepts (name, kind, enum values, reference categories, defaults, descriptions). The descriptor IS the parser's accepted-parameter set. |
| `Finalize(const ParseStateBag&, IJob&)` | Reads typed values out of the bag and emits the corresponding `pJob.AddX(...)` / `pJob.SetX(...)` call. |

No chunk parser overrides `ParseChunk` directly. The default `ParseChunk` impl (in [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp), just after `CreateAllChunkParsers`) walks the input lines, validates each name against `Describe().parameters`, stores matched values in a `ParseStateBag`, then invokes `Finalize` to emit the AddX call. An input parameter whose name is not in the descriptor fails the parse.

**The invariant:** drift between "what the parser parses" and "what the descriptor advertises" is structurally impossible. Both are read from the same `ChunkDescriptor`. The same descriptor feeds:

- the parser (via `DispatchChunkParameters` in [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp))
- the syntax highlighters (Qt + AppKit, via `SceneGrammar` in [SceneEditorSuggestions/](../SceneEditorSuggestions/))
- the scene-editor suggestion engine (right-click context menu and inline autocomplete in both GUI apps)
- any future grammar consumer (linters, doc generators, …)
- the deprecation surface (`ChunkDescriptor::deprecated` + `replacement`, set via `MarkDeprecated()` in [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp)): `DeriveToJob` logs one warning per deprecated chunk type per load, the agent `read_schema` JSON carries `"deprecated":true` + `"replacement"`, and the description is prefixed `DEPRECATED (...)` so completion popups and the property panel show it. A deprecated chunk still parses and renders exactly as before (docs/SCENE_CONVENTIONS.md "Deprecated materials"). Since legacy-deprecation Phase 1 (2026-10-09) there is a stronger tier, FROZEN (`ChunkDescriptor::frozen`, set by `MarkFrozen()`; implies `deprecated`): the warning says "LEGACY and UNSUPPORTED", `read_schema` adds `"frozen":true`, the agent refuses to insert one, and the palettes sort it last.  Every non-supported chunk except the seven legacy BRDF materials is listed in ONE table, `kLegacyTierTable`, applied by a descriptor-copying wrapper in `CreateAllChunkParsers()` (Finalize forwards unchanged, so rendering is bit-identical); a deprecated keyword ALIAS that shares a supported descriptor (`mis_pathtracing_shaderop`) is deprecated by dispatch keyword via `DeprecatedChunkAliasReplacement()`.  Tiers: docs/SCENE_CONVENTIONS.md §11.4.

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
| `GetVec2(key, double[2])` | `bool` (true if present, fills the array) |
| `GetVec3(key, double[3])` | `bool` (true if present, fills the array) |
| `GetVec4(key, double[4])` | `bool` (true if present, fills the array) |
| `GetMat4(key, double[16])` | `bool` (true if present, fills the array) |
| `Has(key)` | `bool` (was the param explicitly set?) |
| `GetRepeatable(key)` | `const std::vector<std::string>&` (for `p.repeatable = true` params) |
| `Singles()` | `const std::map<std::string,std::string>&` (raw access; useful for composite tokens) |

The default in each `GetX` accessor matches the legacy local-variable initial value used by the pre-migration parser. Use `bag.Has(key)` when you need to distinguish "parameter was explicitly set" from "parameter is absent" (e.g. for unit conversions like `DEG_TO_RAD` that should only apply on explicit input).

**`GetVec2`/`GetVec3`/`GetVec4`/`GetMat4` hard-error on wrong arity (DL-32
round 2, round 3, [docs/DEBT_LEDGER.md](../../../docs/DEBT_LEDGER.md)).**
Each accessor requires EXACTLY 2 / 3 / 4 / 16 space-separated finite
numbers; a value with fewer or more tokens logs `kVectorArityFmt` (names
the chunk, the parameter, and expected-vs-actual) and latches
`ParseStateBag::HadHardError()` instead of silently zero-filling the
missing components or dropping the extras. `IAsciiChunkParser::ParseChunk`'s
default implementation and Cst.cpp's `Finalize()` call sites AND
`!bag.HadHardError()` into their success check, so a chunk that trips this
fails the derive even though `Finalize()` may have already run to
completion (most `Finalize()`s call `bag.GetVec3(...)` unconditionally and
never checked its return value, even before this existed — see
`Finalize()`'s own contract note above and `Cst.h`'s "KEPT LIVE BUT LOUD"
canonical statement for what that means for a Job the chunk partially
mutated before the failure was noticed). If a new field genuinely needs a
shorthand this strict arity would refuse (the way
`standard_object`/`override_object`'s `scale` and `orthographic_camera`'s
`viewport_scale` each accept a single number as a uniform broadcast), do
NOT loosen the shared accessor — write a dedicated resolver that
pre-validates the shorthand's own arity and only calls
`GetVec2`/`GetVec3`/`GetVec4`/`GetMat4` once the full arity is already
confirmed (`ResolveScaleVec3` / `ResolveVec2UniformBroadcast` in
[ChunkParserRegistry.cpp](ChunkParserRegistry.cpp) are the templates), and
mark the parameter's descriptor entry with `allowsUniformScalarBroadcast =
true` so the editor's suggestion engine can advertise it.

**This protection covers only a `Finalize()` that actually calls one of
these accessors** (DL-32 round 3, 2026-09-17) — a `Finalize()` that
hand-rolls its own `sscanf` on `bag.GetString(key).c_str()` instead of
calling `GetVec2`/`GetVec3`/`GetVec4`/`GetMat4` bypasses this arity check
entirely, and `DispatchChunkParameters` does not catch it either (it
finite-checks a numeric-kind parameter's tokens but never counts them
against the declared `ValueKind`). Round 3 found and fixed 15 such
sites — every one of them a genuinely 2-component field (a UV pair, a
`(theta, phi)` pair) that had been declared `DoubleVec3` (and so read only
2 of its "3" components) or, for `orthographic_camera`'s `viewport_scale`,
declared plain `Double` while being read as 2 components. **When you add a
new fixed-arity numeric parameter, always read it through the matching
`GetVecN`/`GetMat4` accessor — never a bare `sscanf` on the bag's raw
string** — that is what makes this section's guarantee true for your new
parameter too.

## Registered Chunk Families

The registry in `CreateAllChunkParsers()` ([ChunkParserRegistry.cpp](ChunkParserRegistry.cpp)) is the authoritative map from scene syntax to implementation. The table below is generated by counting `add(...)` calls in `CreateAllChunkParsers()`; if you add or remove a chunk parser, update the relevant row. Current families:

| Family | Count | Examples |
|--------|-------|----------|
| Painters | 40 | `uniformcolor_painter`, `png_painter`, `voronoi3d_painter`, `iridescent_painter`, `channel_painter`, `function2d_painter` (the `*_function2d_painter` painters live here, not under Functions), `expression_painter` (the doc-88 texture-expression VM colour pipe; `scalar_painter`'s sibling `expression` and `painter` FORMs are not separate keywords), `ramp_painter` (doc-88 S3 P2.2, the universal scalar->colour remap), `mapping_painter` (doc-88 S7 P2.3, the domain scale/rotate/translate/reproject wrapper -- uv/world/object/triplanar), `stochastic_tile_painter` (doc-88 P3.1 S8, hex-tiling with histogram-preserving blending), `scatter_painter` (doc-88 P3.2 S8, texture-bombing / FX-map-lite) |
| Functions | 3 | `piecewise_linear_function`, `piecewise_linear_function2d`, `expression_function2d` |
| Materials | 34 | `null_boundary_material`, `lambertian_material`, `diffuse_transmission_material`, `mix_material`, `dielectric_material`, `rough_dielectric_material`, `ggx_material`, `pbr_metallic_roughness_material`, `sheen_material`, `composite_material`, `coated_material`, `fabric_material`, `weave_material`, `hylios_skin_material` |
| Cameras | 8 | `scene_options`, `camera_defaults`, `film` (scene-level config), `pinhole_camera`, `onb_pinhole_camera`, `thinlens_camera`, `fisheye_camera`, `orthographic_camera` (all 5 cameras accept optional `near_clip` in scene units) |
| Geometry | 29 | `sphere_geometry`, `3dsmesh_geometry`, `gltfmesh_geometry`, `gltf_import`, `displaced_geometry`, `bezierpatch_geometry`, `sdf_geometry`, `skeleton_geometry`, `cartesian_disk_geometry`, `sweep_geometry`, `lathe_geometry`, `skin_geometry`, `path_instances_geometry`, `hair_geometry`, `simulated_mesh_geometry`, `simulated_points_geometry` (deforming mesh / particle outputs read from a simulation cache or producer `source <simulation>.<output>` with motion blur; see `docs/SIMULATION_SOLVER_CONTRACT.md`) |
| Hair guides | 1 | `hair_guides` |
| Simulations | 6 | `cloth_simulation`, `fire_simulation`, `simulation_cache` (M4), `rigid_simulation`, `simulation_group` (M6), `liquid_simulation` (M7) (`ChunkCategory::Simulation`; see "Simulation producers", "Coupling" and "Liquid" below) |
| Modifiers | 5 | `relief_modifier`, `modifier_stack`, `blended_modifier`, `normal_map_modifier`, `glint_modifier` (`bumpmap_modifier` was REMOVED 2026-09-06 -- a scene still carrying one is refused with a diagnostic naming `relief_modifier` and `tools/migrate_scenes_relief.py`; see `docs/RELIEF_MODIFIER_DESIGN.md` §7) |
| Media | 6 | `homogeneous_medium`, `heterogeneous_medium`, `painter_heterogeneous_medium`, `multichannel_heterogeneous_medium`, `fire_medium`, `global_medium` |
| Objects | 3 | `standard_object`, `csg_object`, `override_object` |
| Shader ops | 13 | `pathtracing_shaderop`, `mis_pathtracing_shaderop` (legacy alias), `finalgather_shaderop`, `directlighting_shaderop`, `arealight_shaderop`, `alpha_test_shaderop` |
| Shaders | 4 | `standard_shader`, `advanced_shader`, `directvolumerendering_shader`, `spectraldirectvolumerendering_shader` |
| Rasterizers | 13 | `pixelpel_rasterizer`, `pathtracing_pel_rasterizer`, `bdpt_pel_rasterizer`, `vcm_pel_rasterizer`, `mlt_spectral_rasterizer`, `auto_rasterizer`, `auto_spectral_rasterizer`, `hyperspectral_radiometric_rasterizer` |
| Rasterizer outputs | 1 | `file_rasterizeroutput` (attaches to the active rasterizer — its rasterizer chunk must be declared before it, else the load fails with a "no rasterizer is set" diagnostic; a LATER rasterizer chunk replaces the active one and drops its outputs with a load warning) |

| Lights | 7 | `rect_light`, `shape_light`, `ambient_light`, `omni_light`, `spot_light`, `directional_light`, `hosek_wilkie_skylight` |
| Photon maps | 12 | 6 generate + 6 gather (caustic / global / shadow / translucent — Pel and spectral) |
| Irradiance cache | 1 | `irradiance_cache` |
| Animation | 4 | `keyframe`, `timeline`, `animation_options`, `animation` |
| Scene variants | 2 | `scene_variant`, `active_scene_variant` (the CST named-overlay feature) |
| Global config | 1 | `light_rr_threshold` (standalone RR-threshold chunk, mirrors `global_medium`) |

**Total: 193 chunk keyword entries** -- counted directly from the 193 `add(...)` registrations in `CreateAllChunkParsers()` (pinned by `tests/SceneEditorSuggestionsTest.cpp`; 192 unique descriptors since `mis_pathtracing_shaderop` shares an implementation class with `pathtracing_shaderop`). Note that `expression_function2d` is registered adjacent to the painter chunks in code and counted under `Functions` above (`40 + 3 = 43`), and `scalar_painter`'s `painter NAME channel RGB` form is a channel option on `scalar_painter` (not a separate keyword). Read `CreateAllChunkParsers()` in [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp) for the canonical list.

### Composite chunks (parse-time sugar)

Almost every chunk's `Finalize` makes ONE `pJob.AddX(...)` call. The **area
lights** (2026-08-12) are the exceptions worth knowing about — `rect_light` (a
rectangular panel) and `shape_light` (a sphere / ellipsoid / box / cylinder that
emits). Each is written as one chunk and each `Finalize` makes the FOUR calls the
hand-authored area-light chain makes — `AddUniformColorPainter`,
`AddLambertianLuminaireMaterial`, the shape's own geometry call
(`AddClippedPlaneGeometry` / `AddSphereGeometry` / `AddEllipsoidGeometry` /
`AddBoxGeometry` / `AddCylinderGeometry`), `AddObject` (see
[docs/SCENE_CONVENTIONS.md](../../../docs/SCENE_CONVENTIONS.md) §3.5 for the
chain, and the two parsers' header comments in
[ChunkParserRegistry.cpp](ChunkParserRegistry.cpp) for `rect_light`'s corner math
and sidedness contract, and for why `shape_light` needs no `facing` at all). The
renderer core learns nothing new; the CST keeps the compact text, so save
round-trips it verbatim and every derive expands it identically.

Two consequences for anyone adding another composite chunk:

- The three helper entities get **derived names** (`<name>__pnt`, `<name>__mat`,
  `<name>__geo`) — the same three for both chunks, deliberately — documented in
  the descriptor's `description` so a collision is diagnosable. Any of the four
  failing returns `false` from `Finalize`, which fails the derive with the
  manager's ordinary duplicate-name error.
- **The incremental derive must refuse it.** `Cst::DeriveToJobIncremental` drops
  and re-applies an edited chunk by category (`DropChunkByCategory`), which
  cannot undo a chunk that produced entities in four managers. `rect_light` and
  `shape_light` are both named in that function's refusal list alongside
  `gltf_import`, so editing one falls back to a full derive.

## Simulation producers (M4)

A simulation is a scene element that produces time-indexed data (docs/SIMULATION_IN_RISE_PROPOSAL.md).
Three producer chunks, `ChunkCategory::Simulation`, all **data only** (the `hair_guides` precedent):
`Finalize` records the simulation in the Job's table (`IJob::AddSimulation`); loading a scene never
runs a solver.  `rise-sim bake|resume|status <scene.RISEscene> [simulation ...]` bakes, resumes and
reports them; the editor runs an interactive one live (`LiveSimulationAttachSimulation`).

```text
cloth_simulation                      # solver cloth.shell
{
    name            flag              # unique; no `.` (consumers write `flag.surface`)
    cache           caches/flag       # default: per-user cache folder, not beside the scene (see below); `~/`, absolute, scene-relative too
    mode            interactive       # every cloth.shell parameter by its solver name
    vertices_u      31
    frames          91
    pin             v0 follow pole    # <selection> [depth N] [fixed|translate ..|oscillate ..|rotate ..|follow <object>]
    follow_object   pole              # scene input: the object's keyframed transform
    collider        sphere 0 0.3 0 0.3    # plane | sphere | box | cylinder, world space
    collider_object ball              # scene input: the object's geometry as a moving mesh collider
    material        denim-twill       # plus material_<field> overrides
}

fire_simulation                       # solver fire.methane (case + result-affecting options)
{
    name            campfire
    tier            6
    mode            fast
    backend         metal             # metal (default) | cpu: the experimental CPU backend; keyed only as cpu
    frame_origin_s  1.0               # state the window so the scene can key the fire before a bake
    end_time_s      3.0
}

simulation_cache                      # an imported or pre-baked cache, no solver (preview)
{
    name            fabric
    cache           ~/.cache/rise/fabric
}

simulated_mesh_geometry { name geo_flag   source flag.surface   ... }   # (braces on their own lines in a file)
fire_medium { name flame   fidelity_mode preview   source campfire.volume   channel_carbon carbon ... }
```

- **One source of truth.** `cloth_simulation` and `fire_simulation` are generated from the solver's
  parameter schema (`Simulation/SimulationParameterSchema.h`, registered beside the factory): each
  schema parameter is a chunk parameter of the same name, type, default, range and units.  The
  structured cloth fields (`material*`, `pin`, `collider`, `collider_object`, `follow_object`) are the
  schema's chunk fields, translated by `Cloth/ClothParameterSchema.cpp`.
- **Scene inputs** (`collider_object`, `follow_object`) are `Reference` parameters to `{Object}`, so the
  editor draws the connection; their content enters the cache key (geometry, keyframed transform; not
  material or name).  `source` is a `Reference` to `{Simulation}`; the `.output` suffix names an output.
  Only a mesh output binds `simulated_mesh_geometry`, only a fire grid output binds `fire_medium`, only a
  `points.v1` output binds `simulated_points_geometry` (M7 prerequisites: particles as spheres).
- **Default cache location.** A producer with no `cache` writes to the per-user cache folder:
  `<base>/scene-caches/<scene-stem>-<16 hex of SHA-256 of the scene's canonical path>/<name>` (macOS base
  `~/Library/Caches/RISE`; `RISE_SIMULATION_CACHE_DIR` replaces it), not beside the scene; one function
  (`DefaultSimulationCachePath`, `Simulation/SimulationProcess.h`) serves every process.  An explicit
  `cache` is unchanged.  A cache left at the old `caches/<name>` is reported once and never read.
- **Declaration order.** A producer is declared before its consumers (the editor's insert tier puts it
  with geometry); scene inputs resolve after the derive, so a producer may name objects declared later.
- **States** (`IJobPriv::DescribeSimulations`, C API `GetSimulation`): missing, baking, partial,
  complete, stale (the cache holds another key), failed.  A stale or partial cache loads and plays; a
  missing one gives an empty, hidden consumer with a warning, unless the consumer requests predictive
  fidelity (then the load fails).  An edit of a producer never starts a bake.
- **Incremental edits.** An edited producer chunk falls back to a full re-derive (the incremental path
  does not yet capture and restore a simulation record); its consumers are re-derived with it.

### Cloth that reads as fabric (weight, air, light)

Measured on the showcase scenes (docs/SIMULATION_SHOWCASE.md, "Cloth weight and shimmer"); gravity, mass
and the drag law of `cloth.shell` check out (a sheet dropped flat with `drag_coefficient 1.2` reaches
0.980 m/s against the predicted sqrt(2 m'' g / (rho C_d)) = 0.977 m/s; edge-on it falls at g; a sheet
hung from an edge comes to rest vertical).  What made cloth look weightless was the authoring:

- **`drag_coefficient` defaults to 0: the cloth swings in a vacuum.**  With no air a released sheet is a
  rigid-looking pendulum: hung from an edge and let go from level it swings to 77 degrees on the far side and
  keeps swinging at about 60 degrees for seconds (the Rayleigh mass damping, 0.05/s, has a 20 s time
  constant), and stays a flat panel because nothing pushes on its face.  Real fabric is dominated by air
  broadside (the drag pressure at 1.5 m/s, 1.6 Pa, is more than twice the weight of a 70 g/m^2 silk).  Set
  `drag_coefficient` (about 1.0-1.2, a flat plate broadside; the model pushes only along the normal, so a
  sheet still slices edge-on) for any cloth that moves through open air.  Drag marks the cache preview.
- **Weight is mass relative to drag.**  Without drag the areal density cancels out of the motion; with it,
  light fabric is genuinely floaty (70 g/m^2 at C_d 1.2: falls from level to hanging in 1 s with no
  overshoot).  For a "heavy, swinging" drape use 0.1-0.2 kg/m^2 (charmeuse, satin, crepe; `material_density`
  with `material_bend_*` around 2-4e-6 N m): at 0.15 kg/m^2 and C_d 1.0 the same sheet reaches vertical in 0.7 s,
  overshoots by 16 degrees and settles in two swings.  Keep the Rayleigh damping small: it damps rigid
  motion too (free fall is 2.5% slow after 1 s at 0.05/s), so a larger `material_damping_mass` reads as syrup.
- **A corner-pinned canopy bounces like a trampoline without drag** (`conjurer` before: the centre rose and
  fell at 2 Hz, still 0.16 m peak to peak after 3 s; with `drag_coefficient 1.0` and a 0.14 kg/m^2 satin,
  0.04 m).
- **A sheet pinned flat along a straight edge hangs as a flat panel.**  Curtains and banners hang in folds
  because their heading is gathered: author the sheet wider than the rod and gather it (`size_u 1.111`,
  `stretch_u 0.9` holds 1.11 m of fabric on 1.0 m; the pinned edge stays gathered).  The start is then a
  compressed sheet that must buckle: at 0.8 the Newton solve failed at once, at 0.9 it needed `substeps 16`
  and `newton_iterations 100` at 61 x 61 (13 step halvings, none unconverged).  Gather lightly.
- **Grazing hard light shows the mesh.**  A point or spot light skimming a gently undulating sheet (under
  10 degrees on a fifth of the triangles) casts shadows of the mesh-scale undulations whose edges follow the
  triangles: blocky light and dark patches that jump from frame to frame.  Light cloth with area lights,
  or from well above the surface's tangent plane; a finer mesh shrinks the steps.

`realistic_camera` is intentionally **not** registered — the keyword is reserved for the future multi-element lens-system camera (see [docs/CAMERAS_ROADMAP.md](../../../docs/CAMERAS_ROADMAP.md) Phase 4). Until that lands, scenes that want photographic depth-of-field use `thinlens_camera`.

## Liquid (M7)

docs/SIMULATION_SOLVER_CONTRACT.md.  `liquid_simulation` (solver `liquid.mac`, chunk generated from its schema) is data
only, like the other producers: a free-surface liquid in a box on a staggered grid, its surface published as a
changing-topology `mesh.v2` to show with `simulated_mesh_geometry` (a dielectric material for water).  Sample:
`scenes/Tests/Simulation/liquid_dam_break.RISEscene`.

```text
liquid_simulation
{
    name            pool
    frames          61
    domain_min      -0.5 0 -0.25              # the box (m); free-slip walls; `top open` for p = 0 on +y
    domain_size     1 0.5 0.5
    resolution      64                        # cells along the longest side (cubic cells)
    liquid          box -0.5 0 -0.25 -0.2 0.3 0.25   # or sphere cx cy cz r | fill h (repeatable: the union)
    solid           box 0.1 0 -0.1 0.2 0.1 0.1        # or plane | sphere | cylinder cx cy cz ax ay az r h (static)
    solid_object    rock                      # a scene object's closed mesh at t = 0, as a static solid
}
simulated_mesh_geometry { name water  source pool.surface }
```

Viscosity and surface tension (M7c, docs/SIMULATION_SOLVER_CONTRACT.md) are off by default: `viscosity` (Pa s;
`viscous_walls no_slip|free_slip`) and `surface_tension` (N/m), with `density` (kg/m^3) for both; sample
`scenes/Tests/Simulation/liquid_honey_water.RISEscene` (honey beside water).  Surface tension without viscosity is
labelled preview.

Rigid bodies (M7b, docs/SIMULATION_SOLVER_CONTRACT.md) move with the liquid in its pressure solve: `body <name>
sphere r | box sx sy sz | cylinder r h` (body frame at the centre of mass, cylinder axis z) then `motion dynamic`
(default: the liquid integrates it; `density d` or `mass m`, `lock <xyzr>`), `motion prescribed` (`velocity`,
`spin`, `amplitude ax ay az`, `frequency f`, `phase p`) or `motion port` (follows `rigid.bodies` in a group;
`coupling implicit|staggered`), and `at x y z`, `orientation w x y z`, `velocity`, `spin`.  The bodies are published
as `<simulation>.bodies` (transform.v1).  With `rigid.bodies` owning contact, the liquid owns the exchange
(sample: `scenes/Tests/Simulation/liquid_rigid_bodies.RISEscene`):

```text
rigid_simulation { name drop  frames 61  body buoy sphere 0.07 density 300 at -0.2 0.45 0  plane 0 0 0 0 1 0 }
liquid_simulation { name pool  frames 61  liquid fill 0.3  body buoy sphere 0.07 motion port at -0.2 0.45 0 }
simulation_group
{
    name            tank
    members         pool
    members         drop
    connect         drop.body_state -> pool.body_state
    connect         drop.body_mass_properties -> pool.body_mass_properties
    connect         pool.body_load -> drop.body_load
    order           gauss-seidel
    member_order    pool drop
    owned           pool drop                 # the liquid solves drop's bodies inside its step
}
standard_object { name buoy ... transform_source drop.bodies[buoy] }
```

M7d (docs/SIMULATION_SOLVER_CONTRACT.md): `body_object <object> [options]` makes a body from a scene object's closed
mesh (world space at t = 0; body frame at its centroid and principal axes, mass properties from the mesh; the
options of `body` but `at` and `orientation`; an open mesh is refused).  `sheet <name> port [port <accepted>]
[load_port <offered>] [relaxation w]` is a thin open sheet as a moving boundary of the liquid (preview), following a
mesh surface such as a cloth's: connect the cloth's `surface_state` to `<accepted>` (default `sheet_state`) and
`<offered>` (default `sheet_load`) to its `surface_load`, with the liquid first in `member_order`; the load is
lagged one macro step and unstable for light sheets (the results document gives the measured range).
`sheet <name> rect sx sy [divisions nx ny] [at x y z] [orientation w x y z] [velocity ..] [spin ..] [amplitude ..]
[frequency f] [phase p]` is a prescribed rectangle (its frame's xy plane, normal +z).  Sample:
`scenes/Tests/Simulation/liquid_cloth_flap.RISEscene`:

```text
cloth_simulation { name flap  frames 61  size_u 0.3  size_v 0.45  origin 0.6123 0.55 0.05  axis_u 0 0 1  axis_v 0 -1 0
                   material cotton-plain-weave  material_density 50  pin v0 }
liquid_simulation { name pool  frames 61  liquid box -1 -1 -1 0.3 0.45 2  sheet flap port relaxation 0.3 }
simulation_group
{
    name            flume
    members         pool
    members         flap
    connect         flap.surface_state -> pool.sheet_state
    connect         pool.sheet_load -> flap.surface_load
    order           gauss-seidel
    member_order    pool flap                 # the liquid reads the flap's state at the macro step's start
    macro_step      0.008333333333333333      # exactly 1/120 s: a macro step must divide the frame interval
}
```

M7e: `sheet <name> port coupling implicit [mechanics_port <m>] [stiffness_port <s>] [steps_port <t>] [response
stiffness|lumped]` couples the cloth implicitly (preview, stable for cloth of any weight): besides the two connections
above, connect the cloth's `surface_mechanics` and `surface_stiffness` to `<m>` and `<s>` (defaults `sheet_mechanics`,
`sheet_stiffness`) and `<t>` (default `sheet_load_steps`) to its `surface_load_steps`, and make the exchange owned
(`owned pool cloth`, the liquid first).  The cloth then takes the liquid's steps.  A macro end within 1e-6 of a macro
step of a frame time is snapped to it, so a macro step written a few digits short no longer leaves sliver steps; with
the cloth first in Gauss–Seidel a staggered sheet still reads the cloth's state at the macro step's start.  Sample:
`scenes/Tests/Simulation/liquid_cloth_wave.RISEscene`.  M7h (docs/SIMULATION_SOLVER_CONTRACT.md): also connect
`cloth.surface_elements -> pool.sheet_elements` (sheet option `elements_port`): inside the macro step the cloth's model
is corotational, and the liquid refuses an implicit sheet without its elements.  Rigid bodies (M7h) collide with each
other (spheres, boxes, cylinders; body options `restitution e`, `friction mu`, `contact on|off`; a `body_object` among
other bodies needs `contact off`), and `connect drop.body_contact -> pool.body_contact` gives the liquid's own copies
of the bodies rigid.bodies' planes and body-body contact inside the macro step.  M7i (docs/SIMULATION_SOLVER_CONTRACT.md):
liquid resting on an implicit sheet needs a layer about 2.5 cells deep, as on a rigid floor (M7l, the level set two-valued
at the sheet, docs/SIMULATION_SOLVER_CONTRACT.md: without the volume correction a held bowl's 1.3-cell rim sloshing loses 2.5%
in 2 s and a sagging bag's 2.4 cells 4%, where M7i's 5-cell statement had 2.4 cells losing 41%); the faces at a sheet's
free edge are split by the part the sheet covers; nothing to connect:

```text
cloth_simulation { name cloth  frames 61  size_u 0.3  size_v 0.45  vertices_u 13  vertices_v 19
                   origin 0.6123 0.55 0.05  axis_u 0 0 1  axis_v 0 -1 0
                   material cotton-plain-weave  material_density 0.2  pin v0 }
liquid_simulation { name pool  frames 61  resolution 64  liquid box -1 -1 -1 0.3 0.45 2  sheet cloth port coupling implicit }
simulation_group
{
    name            flume
    members         pool
    members         cloth
    connect         cloth.surface_state -> pool.sheet_state
    connect         cloth.surface_mechanics -> pool.sheet_mechanics
    connect         cloth.surface_stiffness -> pool.sheet_stiffness
    connect         pool.sheet_load -> cloth.surface_load
    connect         pool.sheet_load_steps -> cloth.surface_load_steps
    order           gauss-seidel
    member_order    pool cloth
    owned           pool cloth
    macro_step      0.008333333333333333
}
```

M7m (docs/SIMULATION_SOLVER_CONTRACT.md): `viscosity` works with prescribed and implicit sheets: the sheet is a no-slip
wall of the viscous solve on each wetted side (no viscous stress crosses it), and an implicit cloth's vertex velocities
are solved with the viscous step, so the liquid's drag reaches the cloth through `surface_load_steps` (nothing more
to connect).  A staggered or one-way port sheet with viscosity is refused (its load is the pressure only).  Sample:
`scenes/Tests/Simulation/liquid_honey_cloth.RISEscene` (honey poured onto a pinned cloth).  Stated limits (the
results' Known limits): a layer resting on a sheet needs about 2.5 cells; very light cloth (below about 0.1 kg/m^2)
with a free edge at a waterline on a coarse grid is kicked at the edge, which the progress detail flags as a rotation
stretch or linearisation correction above 1%: use heavier cloth, pin the edge, or refine the grid.

### Liquid inflow sources and drains (M9, preview)

`source <name> box cx cy cz sx sy sz |
cylinder cx cy cz ax ay az r h | sphere cx cy cz r` (centre and size; a cylinder is a nozzle: centre, axis, radius,
length) then `speed s` (m/s, default 1), `direction dx dy dz` (default the cylinder's axis, else `0 -1 0`),
`orientation w x y z` (box), `start t`, `end t` (the window `[start, end)`), `schedule t f [t f ...]` (a piecewise-linear
factor on the speed, held outside the table) and, for a box, `drops nx nz r period` (rain: nx x nz columns along the
box's local x and z each release a drop of radius r once per period at a phase and position hashed from the source,
column and period index, so no random state; the schedule is then the chance of a release).  The region is filled
with liquid at the end of every step of the window (after reinitialisation, particles, volume correction and the
sheets' guards; not beyond its exit plane) and its open faces take the velocity at the step's start, so the flux is
its exit area (seen along the direction) times the speed, plus its own volume at the first fill.  `drain <name> <shape>
[orientation ..] [start t] [end t]` removes the liquid inside its region (an overflow, a plughole).  The volume
correction's target gains the first fill and then the nominal flux (exact integral of the schedule; or each drop's
4/3 pi r^3) and loses what a drain measured; the level set's own refills are reported but not counted (they also
replace what the transport erodes inside the region: +41% / +33% / +21% of the added volume at 2 / 2.6 / 4 cells of
nozzle radius).  The progress detail (printed by a single-solver `rise-sim` bake when it finishes) reports `source <name>
added X L (the level set's refills Y L)`, `drain <name> removed Z L` and the liquid's volume against the target.  The
time step takes each source's speed times its largest factor.  Per-source state is in the checkpoint member `sources`
(only with sources).  Solver parameters `sources` / `drains` (lists of maps: `name`, `shape`, `center`, `size`,
`orientation`, `axis`, `radius`, `height`, `direction`, `speed`, `start`, `end`, `schedule`, `drops`) are written back
only when present; refused: unknown keys, a drain with a speed, direction, schedule or drops, drops on a non-box, a
schedule whose times do not increase or whose factors are negative, `end <= start`, duplicate names.  Always preview
(evidence condition `sources`).  Limits: emit into air (inside liquid a source adds only what flows out of it); keep a
region two cells or more from sheets and bodies (the refill does not know their sides); a jet narrower than about
three cells thins with its fall and its tip erodes, and drops below about two cells evaporate into the volume
correction; a drain removes everything inside it, so a basin's level is set by where the drain sits.  Not built: a
source inside liquid adding a set flux (needs a mass source in the pressure solve), drains with a rate.  Works with
viscosity, bodies and sheets (`LiquidSourceTest`: nominal volume exact, jets against free fall and continuity, rain
counts, a floating box rising with the level, rain on an implicitly coupled sheet without leakage, 1 vs 3 threads and
checkpoint restore identical).  Sample: `scenes/Tests/Simulation/liquid_inflow.RISEscene`:

```text
liquid_simulation
{
    name            basin
    liquid          fill 0.03
    source          tap cylinder -0.08 0.25 0 0 -1 0 0.02 0.04 speed 0.8 end 1.2 schedule 0 0.1 0.2 1
    source          shower box 0.09 0.27 0 0.18 0.03 0.26 speed 1.2 start 0.2 end 0.8 drops 3 4 0.022 0.15
    drain           overflow box 0.18 0.08 -0.13 0.04 0.04 0.04
}
```

## Coupling (M6)

docs/SIMULATION_SOLVER_CONTRACT.md.  `rigid_simulation` (solver `rigid.bodies`, chunk generated from its schema) and
`simulation_group` are data only, like the other producers.  `rise-sim bake <scene> <group>` (or any member's name)
bakes a group as ONE process running the group host; each member keeps its own cache.

```text
rigid_simulation
{
    name            drop
    frames          31
    body_object     ball mass 0.1 contact off # <object> [mass m|density d] [velocity ..] [spin ..] [contact off]: geometry at t = 0
    body            crate box 0.3 0.3 0.3 density 500 at 0 1 0 spin 0 2 0   # or box|sphere r|cylinder r h; [restitution e] [friction mu]
    plane           0 0 0 0 1 0 restitution 0.5 friction 0.4               # impulse contact (preview)
}
cloth_simulation { ...  collider_port ball }  # the object's geometry as a collider moved by port collider_surface
# M9: an analytic body coupled to cloth that also collides with other bodies and planes: `body ball sphere 0.1 mass 0.1
# at 0 0.36 0 surface ball` takes its coupling surface from object `ball` (world space at t = 0, the body at its initial
# pose), so `collider_port ball` has that surface's topology; the body keeps its sphere for mass and contact.
simulation_group
{
    name            trampoline
    members         drop                      # repeatable Reference to {Simulation}
    members         sheet
    connect         drop.body_state -> sheet.collider_surface via rigid_surface body=ball
    connect         sheet.collider_reaction -> drop.body_load via contact_reaction targets=ball
    policy          implicit                  # staggered | implicit
    order           gauss-seidel              # gauss-seidel | jacobi
    member_order    drop sheet
    tolerance       1e-6                      # implicit; also sub_iterations, relaxation, aitken,
    absolute_tolerance 1e-4                   #   on_non_convergence fail|halve, minimum_macro_step
    macro_step      0.0333333                 # default: the smallest member output interval
}
standard_object { name ball ... transform_source drop.bodies[ball] }   # follows the body (keyframes)
```

Analytic bodies on cloth with body-body contact (M9): a `sphere`, `box` or `cylinder` body takes an optional
coupling surface, `surface <object>` in the chunk (solver: `surface_input <scene input>`, or inline
`surface_vertices` + `surface_triangles`), world space at t = 0 with the body at its initial pose.  The record's
surface (what `rigid_surface` places) is then that mesh in the body frame, so a cloth `collider_port <object>` made
from the same object has its topology and the adapter drives it, while the body keeps its analytic shape for mass
properties, plane contact and body-body contact.  A mesh body (`body_object`) with a surface is refused; written back
only when given (the scene input then enters the key).  A body resting on another on cloth may need
`on_non_convergence halve` under `policy implicit`.  `ClothRigidPrimitiveTest`: momentum of sheet and two bodies
conserved to 5e-6 in a zero-gravity collision, a box resting on a sphere on a trampoline.  Cloth-body contact inside a
liquid group is not built (docs/SIMULATION_IN_RISE_PROPOSAL.md §5 lists what is missing).

One-way from another simulation's cache, on the downstream (any solver chunk):
`port_source <port> from <simulation>.<output> [via <adapter> [key=value ...]] [aux <member>.<port>] [time_offset s]`,
e.g. `port_source surface_load from fire.volume via aerodynamic_drag temperature_channel=temperature`.  The upstream
is baked first; its key and the adapter's identity enter the downstream's key.

## `thinlens_camera` Units & Sensor-Format Presets

### Unit story (read this first)

Cameras are split between two unit conventions on purpose:

- **MM (millimetres) for lens specs**: `sensor_size`, `focal_length`, `shift_x`, `shift_y`. These are photographic numbers — a 35mm full-frame lens is always written `focal_length 35`, regardless of how the scene's geometry is scaled. The editor surface stays stable on mm too.
- **Scene units for distances that match geometry**: `focus_distance` (matches the camera position and lookat coordinates).
- **Dimensionless**: `fstop`, `aperture_blades`, `anamorphic_squeeze`.
- **Degrees**: `tilt_x`, `tilt_y`, `aperture_rotation` (parser converts to radians internally).

The bridge between mm-input and the scene's geometry unit is the **`scene_options` chunk** (see below). Default is metres (`scene_unit 1.0`), so a default-scene camera with `focus_distance 5` is focused at 5 metres, with `focal_length 35` (mm) auto-converted to 0.035 scene-units inside the camera.

`focus_distance` is required (no default — depends on the scene). The parser validates `focus_distance > focal_length-converted-to-scene-units`; below that, the lens equation produces a negative film distance.

### `scene_options` chunk

Place near the top of the .RISEscene file (before any camera). Default values are used if you omit it.

```
scene_options
{
    scene_unit  1.0      # 1 scene unit = N metres; default 1 (metres)
}
```

Common values: `1.0` (metres, the default), `0.001` (mm), `0.01` (cm), `0.0254` (inches), `0.3048` (feet). The presets surface in the editor combo-box.

`scene_unit` governs the camera mm-input → scene-unit conversion and the
SI-coefficient conversion performed when a `multichannel_heterogeneous_medium`
is constructed.  Declare it before cameras and multichannel media so both
lock the same scale.

### Sensor format presets (mm — unit-stable regardless of scene_unit)

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

The descriptor surfaces this list as quick-pick presets in the editor's properties panel via [`ParameterDescriptor::presets`](ChunkDescriptor.h) (added Phase 1.1).  The Mac and Windows panels render a combo-box / menu next to the line edit when a parameter declares any presets; users can still type custom values.

### Scene-level `camera_defaults`

`thinlens_camera` consults a scene-level `camera_defaults` chunk (Phase 1.1) for `sensor_size`, `focal_length`, and `fstop` when those are omitted on the camera itself.  This is the natural way to express "this whole shot is on full-frame 35mm" once and have every camera in the file inherit it:

```
film
{
    width  800
    height 600
}

camera_defaults
{
    sensor_size  36
    focal_length 35
    fstop        2.8
}

thinlens_camera
{
    location       0 0 5
    lookat         0 0 0
    up             0 1 0
    focus_distance 5     # only param required per camera
}
```

Note: raster dims (`width`, `height`, `pixelAR`) live in the top-level
`film` chunk in scene format v6 (Phase B2 of the Camera/Film/Output
split). Camera chunks are imaging-only and reject `width` / `height`
parameters. Omit the `film` chunk entirely if the default qHD
(960 × 540) is acceptable.

**Declaration order matters:** `camera_defaults` must appear **before** the camera chunks that consume it (same convention as `standard_shader` and other scene-level config).  Cameras with explicit values override the defaults; `focus_distance` has no scene-level fallback (it's shot-specific).

**Multiple `camera_defaults` blocks:** if a scene declares more than one, the parser does a per-field "last write wins" overwrite — block B's `sensor_size` replaces block A's `sensor_size`, but block B's omission of `fstop` leaves block A's `fstop` in place. Cameras declared between blocks see the state-as-of-that-point. This is the simplest semantics and matches how the parser already handles other thread-local state (`s_painterColors`).

**Nested `> load` (RETIRED):** the `> load`/`> run` recursive-include machinery was deleted with the streaming parser (slice 6c), so the old `camera_defaults` / `scene_options` cross-include state-reset hazard no longer exists — native-v7 scenes are self-contained (the migrator flattened includes). `camera_defaults` / `scene_options` state is reset per top-level derive by `ClearChunkParserState()` (called at the top of `Cst::DeriveToJob`).

Tilt-shift (`tilt_x`, `tilt_y`, `shift_x`, `shift_y`) is intentionally NOT settable from `camera_defaults` — those parameters tend to need per-camera override even within a single scene.

### `thinlens_camera` Tilt-Shift Parameters (Phase 1.1)

`tilt_x` / `tilt_y` (degrees) rotate the **focal plane** around the camera's x / y axis (Scheimpflug formulation — sensor and lens stay perpendicular; only the focus plane tilts).  Defaults of 0 give a plain perpendicular-focus thin-lens.  With non-zero tilt the in-focus region becomes a wedge instead of a disc-perpendicular-to-axis, enabling the classic miniature-fake look and selective-focus across non-parallel surfaces. **Bound: `|tilt_x|` and `|tilt_y|` must each be < 80°**; the parser rejects larger values because chief rays go parallel to the focal plane and produce divide-by-zero in the focus-point math. Real tilt-shift lenses max out around ±15°.

`shift_x` / `shift_y` (**MILLIMETRES**, post-Phase 1.2) shift the **lens** along the camera's x / y axis. Sign convention matches Blender Cycles:
- Positive `shift_x` = lens shifts right (camera "looks right", image content moves left in frame).
- Positive `shift_y` = lens shifts up (camera "looks up", image content moves down in frame; standard architectural correction for tall buildings).

A regression scene at [scenes/Tests/Cameras/thinlens_tiltshift.RISEscene](../../../scenes/Tests/Cameras/thinlens_tiltshift.RISEscene) demonstrates the feature with a row of spheres at varying depths and tilt_y rotating the focal plane to follow them.

## Adding A New Chunk Parser

1. **Implement the parser class** in the `RISE::Implementation::ChunkParsers` namespace inside [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp). Convention: place it next to other parsers in the same family. Skeleton:

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

Parameter sets shared across many chunks live in [ChunkParserRegistry.cpp](ChunkParserRegistry.cpp) just after `DispatchChunkParameters` and just before the painter parsers. Reuse them rather than copy-pasting:

| Helper | Used by | Adds |
|--------|---------|------|
| `AddCameraCommonParams` | All 5 cameras | `location`, `lookat`, `up`, `exposure`, `pitch`, `yaw`, `roll`, `target_orientation`, … (raster dims — `width`, `height`, `pixelAR` — moved to the top-level `film` chunk in scene format v6 / Phase B2) |
| `AddPixelFilterParams` | 12 of 13 rasterizers (all except `hyperspectral_radiometric_rasterizer`) | `pixel_sampler`, `pixel_sampler_param`, `pixel_filter`, `pixel_filter_width`, `pixel_filter_height`, `pixel_filter_paramA`, `pixel_filter_paramB`, `blue_noise_sampler` |
| `AddSpectralCoreParams` | All 6 spectral rasterizers (`pixelintegratingspectral` [FROZEN], `bdpt_spectral`, `vcm_spectral`, `pathtracing_spectral`, `auto_spectral`, `mlt_spectral` [FROZEN]) | `spectral_samples`, `nmbegin`, `nmend`, `num_wavelengths`, `hwss` |
| `AddSpectralRGBSpdParams` | `pixelintegratingspectral_rasterizer` only (FROZEN since 2026-10-09; see [docs/SPECTRAL_PARITY_AUDIT.md](../../../docs/SPECTRAL_PARITY_AUDIT.md) §2.1–§2.5 and [docs/SCENE_CONVENTIONS.md](../../../docs/SCENE_CONVENTIONS.md) §11.4) | `integrate_rgb`, `rgb_spd`, `rgb_spd_wavelengths`, `rgb_spd_r/g/b` |
| `AddStabilityConfigParams` | All 10 non-MLT/non-radiometric rasterizers | `direct_clamp`, `indirect_clamp`, `rr_min_depth`, `rr_threshold`, `max_*_bounce`, `light_bvh`, … (`branching_threshold` was excised in 2026-05) |
| `AddPathGuidingParams` | 5 rasterizers: `pixelpel_rasterizer` [FROZEN], `pathtracing_pel_rasterizer`, `bdpt_pel_rasterizer`, `bdpt_spectral_rasterizer`, `auto_rasterizer`.  Path guiding is **not** wired on `pathtracing_spectral_rasterizer`, `auto_spectral_rasterizer`, VCM (pel + spectral), MLT, `hyperspectral_radiometric_rasterizer`, or `pixelintegratingspectral_rasterizer` — `pathguiding_*` lines authored against those chunks hard-fail at parse time. | `pathguiding`, `pathguiding_iterations`, `pathguiding_spp`, `pathguiding_alpha`, `pathguiding_max_depth`, … |
| `AddAdaptiveSamplingParams` | 9 rasterizers: `pixelpel_rasterizer` [FROZEN], `pathtracing_{pel,spectral}_rasterizer`, `bdpt_{pel,spectral}_rasterizer`, `vcm_{pel,spectral}_rasterizer`, `auto_rasterizer`, `auto_spectral_rasterizer` | `adaptive_max_samples`, `adaptive_threshold`, `show_adaptive_map` |
| `AddRadianceMapParams` | All 10 non-MLT/non-radiometric rasterizers | `radiance_map`, `radiance_scale`, `radiance_orient`, `radiance_background` |
| `AddProgressiveParams` | All progressive rasterizers | `progressive_rendering`, `progressive_samples_per_pass` |
| `AddSMSConfigParams` | `pathtracing_pel_rasterizer`, `pathtracing_spectral_rasterizer`, and `hyperspectral_radiometric_rasterizer` | `sms_enabled`, `sms_max_iterations`, `sms_threshold`, `sms_max_chain_depth`, `sms_biased`, `sms_bernoulli_trials`, `sms_multi_trials`, `sms_photon_count`, `sms_two_stage`, `sms_seeding`, `sms_target_bounces`, `sms_extended` (BDPT lost SMS support 2026-05-07; see [CLAUDE.md](../../../CLAUDE.md)) |
| `AddOptimalMISParams` | 3 rasterizers: `pixelpel_rasterizer` [FROZEN], `pathtracing_pel_rasterizer`, `auto_rasterizer`.  BDPT pel/spectral and PT spectral do not consume `rc.pOptimalMIS` (research not extended to BDPT's per-strategy-pair MIS; spectral parent never allocates the accumulator), so `optimal_mis*` lines authored against those chunks hard-fail at parse time; see [docs/SPECTRAL_PARITY_AUDIT.md](../../../docs/SPECTRAL_PARITY_AUDIT.md) §2.4, §2.10. | `optimal_mis`, `optimal_mis_training_iterations`, `optimal_mis_tile_size` |
| `AddBaseRasterizerParams` | All rasterizers | `defaultshader`, `oidn_denoise`, `show_luminaires`, … |
| `AddNoisePainterCommonParams` | Perlin / Turbulence / Wavelet / Worley / Simplex / Gabor 3D noise painters | `name`, `colora`, `colorb`, `persistence`, `octaves`, `scale`, `shift` |
| `AddVariantTagParam` | All 34 material chunks (plus variant-participating chunks) | `variant` (CST `scene_variant` named-overlay tag; `none` = base) |
| `AddPhotonMapGenerateCommonParams` | All 6 photon-map generate parsers | `num`, `power_scale`, `max_recursion`, `min_importance`, `branch`, `reflect`, `refract`, … |
| `AddPhotonMapGatherCommonParams` | All 6 photon-map gather parsers | `radius`, `ellipse_ratio`, `min_photons`, `max_photons` |

Registry-level parser wrappers in `CreateAllChunkParsers()` also decorate descriptors uniformly:

- **`AlphaMaterialParser`**: wraps every `ChunkCategory::Material` parser (all 34 material chunks) to add `alpha_mode` (`opaque|mask|blend`), `alpha_coverage` (`IScalarPainter` reference or inline scalar), and `alpha_cutoff` without duplicating them per material class (see `docs/ALPHA_COVERAGE.md`).
- **`LegacyTierParser`** (`MarkFrozen` / `MarkDeprecated`): wraps chunks listed in `kLegacyTierTable` with a descriptor copy carrying their legacy tier and replacement guidance so the CST validator and editor surface frozen/deprecated diagnostics while keeping `Finalize` bit-identical.

## Where Agent Authors Usually Get Tripped Up

- The older comment near the top lists eight primary chunk categories, but the real registry now covers more specialized families. The registration map is the source of truth.
- Chunk loading is now purely the CST derive path (`Cst::DeriveToJob` → the shared chunk-parser registry). There is no longer a separate top-level streaming syntax layer to change — a new chunk is a registry entry only.
- If a `chunk` parameter seems to disappear, inspect the `ChunkDescriptor` (the descriptor IS the accepted-parameter set) and the CST tokenization (`ParseToCst`) — not the old macro/token-substitution machinery, which is gone.

## MIS Weight Parameters (Rasterizer Blocks)

The following parameters control advanced MIS weight computation. They are **only accepted on `pixelpel_rasterizer` (FROZEN), `pathtracing_pel_rasterizer`, and `auto_rasterizer`** — the parser hard-fails on `optimal_mis*` lines authored against any other chunk. All default-off and fall back to the standard power heuristic. See [docs/SPECTRAL_PARITY_AUDIT.md](../../../docs/SPECTRAL_PARITY_AUDIT.md) §2.4 / §2.10 for why BDPT, VCM, MLT, and the spectral PT/BDPT/auto chunks reject these params (Kondapaneni 2019's single-step BSDF-vs-NEE formulation has not been extended to per-strategy-pair MIS or to the dVCM/dVC/dVM recurrence; spectral parents do not allocate the accumulator).

| Parameter | Rasterizers | Type | Default | Description |
|-----------|-------------|------|---------|-------------|
| `optimal_mis` | `pixelpel_rasterizer` [FROZEN], `pathtracing_pel_rasterizer`, `auto_rasterizer` | bool | FALSE | Run training passes to compute variance-minimizing MIS weights for direct illumination (Kondapaneni et al. 2019) |
| `optimal_mis_training_iterations` | `pixelpel_rasterizer` [FROZEN], `pathtracing_pel_rasterizer`, `auto_rasterizer` | uint | 4 | Number of 1-SPP training passes |
| `optimal_mis_tile_size` | `pixelpel_rasterizer` [FROZEN], `pathtracing_pel_rasterizer`, `auto_rasterizer` | uint | 16 | Spatial tile size for second-moment binning |

## Pure PT Rasterizer Chunks

Two rasterizer chunks bypass the shader-op dispatch chain and call `PathTracingIntegrator` directly:

| Chunk name | Description |
|-----------|------------|
| `pathtracing_pel_rasterizer` | Iterative PT, RGB output, per-sample OIDN AOVs |
| `pathtracing_spectral_rasterizer` | Iterative PT, spectral output, NM or HWSS modes |

These accept all the same parameters as their frozen shader-dispatch counterparts (`pixelpel_rasterizer` and `pixelintegratingspectral_rasterizer`) plus the full `AddSMSConfigParams` suite (`sms_enabled`, `sms_max_iterations`, `sms_threshold`, `sms_max_chain_depth`, `sms_biased`, `sms_bernoulli_trials`, `sms_multi_trials`, `sms_photon_count`, `sms_two_stage`, `sms_seeding`, `sms_target_bounces`, `sms_extended`). The pure PT rasterizers do not use or require a `defaultshader` because they bypass the shader pipeline entirely.

When `oidn_denoise TRUE` is set on `pathtracing_pel_rasterizer`, the filtered film resolve is automatically skipped to preserve raw MC noise for OIDN. See `docs/ARCHITECTURE.md` for details.

See `docs/IMPROVEMENTS.md` item 8 for algorithmic details.

## Good Cross-References

- High-level assembly target: [../Job.cpp](../Job.cpp)
- Public API constructors: [../RISE_API.h](../RISE_API.h)
- Scene taxonomy: [../../../scenes/README.md](../../../scenes/README.md)
- Showcase scenes: [../../../scenes/FeatureBased/README.md](../../../scenes/FeatureBased/README.md)
- Regression scenes: [../../../scenes/Tests/README.md](../../../scenes/Tests/README.md)

### Rough dielectric interface (Tier 1.1)

`rough_dielectric_material` is an additive native-v7 material chunk:

```text
rough_dielectric_material
{
name frosted_glass
ior 1.5
alphax 0.2
alphay 0.4
}
```

All three slots accept single-scalar literals or scalar painters. Spectral
curves resolve to single-scalar slot views: RGB uses the representative green
sample, while NM/HWSS preserve the full curve. Authored RGB triples are refused.
`alphax`/`alphay` are GGX widths, not perceptual roughness; both must be in
(0,1]. The defaults are IOR 1.5 and widths 0.2. Invalid per-hit widths/indices
produce null scattering. Use `dielectric_material` for a smooth interface.
The model includes GGX reflection/refraction with reciprocal two-sided
Kulla–Conty multiscatter compensation, anisotropy,
aggregate solid-angle PDFs, nested-media IOR tracking, and hero-density spectral
companion evaluation. Current PT/BDPT/VCM/MLT HWSS transport conservatively
terminates companions at every rough interface, even without dispersion: these
walkers share one hero medium stack and radiance eta factor. This reduces four
active wavelengths to one until wavelength-specific medium tracking is added.
Nested composites propagate this requirement. Mixed terminated/unterminated
bundles receive equal weight: each bundle is normalized by its own active lanes
before bundles are averaged (also for VCM merging and MLT splats/target).
The local spectral evaluator remains available with caller-supplied wavelength
stacks and hero density. Index-matched interfaces report their exact transparent
delta behavior to shadow queries. SMS caustic construction for the new material
is not supported in this increment. The aggregate sampler retains VNDF null events while restoring their missing
energy through the compensation lobes. Geometric-horizon clipping remains
additional loss. LUT interpolation makes energy restoration approximate; see
[ROUGH_DIELECTRIC_ENERGY.md](../../../docs/ROUGH_DIELECTRIC_ENERGY.md).
Bulk absorption requires a separately assigned medium. World-position IOR
fields are unsupported and refused. Importers still use their existing glass
mappings. See `scenes/Tests/Materials/rough_dielectric.RISEscene`.
