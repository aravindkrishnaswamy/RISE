# Blender → RISE Material Translation

How the Blender bridge converts Cycles / EEVEE node graphs into RISE
materials and painters.  This is the canonical reference for both
sides of the translation: what's supported today, what's tracked, and
how to extend either RISE core or the bridge when new Blender node
types come up.

## Two paths

The bridge classifies every material into one of two paths:

1. **Direct procedural translation** ("simple" graphs).  Walk the
   Blender node graph, map each node 1:1 to a RISE painter, hand a
   painter graph to RISE.  Cheap, animation-friendly (procedural
   noise updates with time / frame).  Loses bit-for-bit Cycles parity
   because Blender's noise basis isn't identical to RISE's, but the
   visual character is preserved.

2. **Bake-on-export** ("complex" graphs).  Drive Blender's own bake
   API (`bpy.ops.object.bake`) to render each BSDF channel to a PNG
   under the temp dir, then hand those PNGs to RISE as image
   texture painters.  Matches Cycles bit-identically by construction
   (Cycles does the bake).  Static — animated procedurals would
   need a per-frame bake.

The bridge tries path (1) first.  If the material's node graph uses
anything in the "force-bake" list below, the bridge falls back to
path (2).

## Classifier rules

A material is **simple** (path 1) iff EVERY one of these holds:

- The Material Output's `Surface` socket reaches exactly **one**
  `ShaderNodeBsdfPrincipled` (transitively through `NodeReroute`s).
  Mix Shaders, Add Shaders, multiple Principleds, Glass + Diffuse
  combinations — all force-bake.

- Every Principled BSDF input socket's upstream chain consists ONLY
  of nodes from the [supported-node table](#supported-node-table)
  below.  Any other node type (Ambient Occlusion, Geometry,
  Layer Weight, Wireframe, custom group, Image Sequence input,
  etc.) forces a bake.

- No socket chain goes deeper than 16 nodes (heuristic — extremely
  deep chains are usually pathological generators that will be more
  reliable to bake).

Otherwise the material is **complex** and goes through the bake
path.

## Supported node table

Listed by Blender's `bl_idname`.

| Blender node | RISE equivalent | Notes |
|--------------|-----------------|-------|
| `NodeReroute` | — | Transparently traversed |
| `ShaderNodeRGB` | `uniformcolor_painter` | Constant colour |
| `ShaderNodeValue` | `scalar_painter` | Constant scalar |
| `ShaderNodeTexImage` | `png_painter` / `jpg_painter` / `exr_painter` / `tiff_painter` / `hdr_painter` | Already wired (Round 1 of the bridge) |
| `ShaderNodeTexCoord` | UV / Object / Generated / Camera | Object → 3D painter chains; UV → 2D painter chains.  Most other outputs force-bake. |
| `ShaderNodeMapping` (POINT) | `RISE_BLENDER_PAINTER_UV_TRANSFORM` wrapper | KHR_texture_transform — already implemented for 2D image textures (ABI v6) |
| `ShaderNodeTexNoise` | `perlin3d_painter` (with `turbulence3d_painter` for high-detail variants) | Blender's noise ≠ Perlin bit-exactly; visual approximation only |
| `ShaderNodeTexVoronoi` | `voronoi3d_painter` | F1 distance, cell colour, etc. — direct mapping |
| `ShaderNodeTexChecker` | `checker_painter` | Two-colour checker; Blender's `Scale` socket maps to `size` |
| `ShaderNodeValToRGB` (Color Ramp) | `colorramp_painter` | Scalar → colour via stops; multi-stop, multi-interpolation.  **Added to RISE core for this translation** (`docs/RISE_API.h` + parser). |
| `ShaderNodeMix` (Color blend) | `blend_painter` | A / B + Factor → 2-stop blend.  When `clamp_factor=True` matches RISE behaviour. |
| `ShaderNodeMixRGB` (legacy) | `blend_painter` | Identical to ShaderNodeMix in Color mode |
| `ShaderNodeMath` (Add/Multiply/Multiply Add when one input is constant) | folded inline at export time | Math nodes that fold to scaling / offsetting of a single value are inlined into the downstream painter (e.g. a Multiply on a noise output absorbed into a ColorRamp position).  Other Math ops force-bake. |
| `ShaderNodeRGBCurve` / `ShaderNodeHueSaturation` / `ShaderNodeBrightContrast` / `ShaderNodeGamma` / `ShaderNodeInvert` | pass-through (currently lossy) | Existing bridge walks through these and ignores the filter; future work could honour them via a remap painter. |
| `ShaderNodeBump` | `BumpModifier` on the material | Driven by a painter (which must itself be a supported chain) |
| `ShaderNodeNormalMap` | `NormalMapModifier` on the material | Requires a `ROMM_Linear` image painter (tangent vectors must bypass colour-space conversion) |

## Force-bake list

Any of these in a graph forces the bake fallback:

- `ShaderNodeAmbientOcclusion` — Cycles' AO is a screen-/scene-space
  query (EEVEE uses SSAO; Cycles ray-traces).  RISE has no
  per-pixel-AO painter and synthesising one from RISE's photon /
  irradiance machinery is out of scope.
- `ShaderNodeMixShader` / `ShaderNodeAddShader` — multiple BSDFs.
  The bridge would have to ALSO bake the secondary BSDF separately
  and recombine via colour-space tricks; baking the final shader
  output is the reliable answer.
- `ShaderNodeGeometry` — incidence, position, true normal, etc.
  Each output has a different semantic; bake.
- `ShaderNodeLayerWeight` / `ShaderNodeFresnel` — view-dependent
  weights that bake-to-texture can capture as a flat approximation
  but procedural mapping would mis-render at off-axis views.
- `ShaderNodeWireframe` — bake.
- `ShaderNodeTexWave` — RISE has no equivalent (Gerstner is wave-y
  but different parameterisation).  Bake for now.
- `ShaderNodeTexMagic`, `ShaderNodeTexBrick`, `ShaderNodeTexGradient`,
  `ShaderNodeTexIES`, `ShaderNodeTexPointDensity` — no RISE
  equivalent yet.
- Any `ShaderNodeGroup` (custom node group) — bake.
- Any node type not listed in either table above.

If a node type is on the force-bake list but a clean direct mapping
shows up later, move it to the supported-node table.  Add a row to
[Capabilities to extend](#capabilities-to-extend-later) so the
decision history is captured.

## Bake-on-render contract

The bake runs automatically inside the RISE render engine's
``render()`` callback BEFORE ``exporter.export_scene`` is invoked.
The user just hits F12 — no manual operator, no separate step.  The
auto-bake driver (``material_bake.auto_bake_complex_materials``):

1. Iterates every material in the scene; classifies each.  Materials
   that classify as **simple** are skipped — the exporter translates
   their node graphs directly into RISE painters (Option A path).
2. For each **complex** material, checks the cache:
   - If the material has no bake metadata at all → bake.
   - If the material has bake metadata AND
     `rise_baked_graph_hash` matches the current node graph hash AND
     the cached PNG files still exist on disk → skip (cache hit).
   - Otherwise (hash mismatch, missing PNG, etc.) → bake.
3. Before baking ANY material, the driver snapshots the scene's
   current render engine (typically `RISE_RENDER`), switches to
   `CYCLES` (required by `bpy.ops.object.bake`), runs the bakes,
   and restores the original engine in a `try/finally` so any
   exception during a single bake leaves the scene's engine setting
   correct.
4. For each material to bake, finds a proxy object — a MESH in the
   scene that uses the material AND has real source-mesh vertices
   (i.e. not a geometry-nodes-only output).  Falls back to a
   diagnostic when no suitable proxy exists; that material renders
   as flat colour for the time being (see "Geometry-nodes proxy
   bakes" in [Capabilities to extend](#capabilities-to-extend-later)).
5. Ensures the proxy has a usable UV layer.  If the mesh has no UV
   layer, runs Smart UV Project (`bpy.ops.uv.smart_project`) on it
   once; the new UV layer persists so subsequent renders don't
   re-unwrap.
6. Creates a temporary `Image` per channel (Diffuse / Roughness /
   Normal), drives `bpy.ops.object.bake`, saves each as a PNG under
   `<temp_dir>/rise_baked/<material_name>_<channel>.png`.
7. Stores the resulting paths and the captured node-graph hash on
   the Material's ID properties:
   - `rise_baked_diffuse_path`
   - `rise_baked_roughness_path`
   - `rise_baked_normal_path`
   - `rise_baked_resolution`
   - `rise_baked_frame`
   - `rise_baked_graph_hash` — the content hash captured at bake time

The ID-property metadata persists across .blend save / load cycles,
so a baked scene survives close-and-reopen without re-baking — as
long as the PNG files on disk also persist (most users keep them in
the persistent Blender temp dir or move them to a project-local
folder).

Auto-bake-on-render is invoked from
``RISEBlenderRenderEngine.render()`` ([engine.py](../src/Blender/addons/rise_renderer/engine.py)):

```python
material_bake.auto_bake_complex_materials(
    scene, resolution=1024,
    report=lambda msg: self.report({"INFO"}, msg))
```

The render engine emits per-material INFO lines so the user sees in
the status bar / info panel exactly which materials baked and which
were cache-hits — without having to look at a separate panel.

The (manual) `RISE_OT_bake_materials` operator and the
`RISE_OT_clear_baked_materials` operator are retained as diagnostic
affordances under Properties → Render → "RISE Material Baking":

- **Force Re-Bake All** — re-bakes every complex material in the
  scene, ignoring the hash cache.  Useful when an external image
  has changed but its filepath stayed identical (Blender doesn't
  re-hash image contents — only filepath + name).
- **Clear RISE Bake Cache** — removes all `rise_baked_*` ID
  properties.  The next render rebakes from scratch.

### Cache invalidation

`material_bake.baked_cache_is_stale(mat)` returns True when:

- The stored `rise_baked_graph_hash` doesn't match the current
  `_material_graph_hash(mat)`, OR
- Any of the cached PNG paths point at a file that no longer
  exists on disk.

`_material_graph_hash(mat)` captures everything that would change
the baked output: per-node `bl_idname` + `name`, every input
socket's `default_value`, every link, image identity for
`ShaderNodeTexImage`, and the colour-ramp stops for
`ShaderNodeValToRGB`.  Pixel contents of linked images aren't
hashed (would be too slow) — `Force Re-Bake All` handles that case
when needed.

Limitations:

- Static at the bake frame.  Animated procedurals (time-varying
  Noise) need per-frame bakes — out of scope for the first cut.
- The unwrap quality depends on the mesh.  Smart UV Project is good
  for most procedural materials on opaque solids; UV-charged meshes
  (a character with hand-painted Cycles textures using existing
  UVs) keep their existing UV layout.
- Memory: 1024² × 3 channels per material per frame.  A scene with
  30 procedural materials at 1024² uses ~360 MB of disk during the
  bake — manageable.  4K bakes are an opt-in per-material setting.
- Emission, transmission, alpha — not in the first bake set.  Emission
  on procedural materials forces a bake of an `EMISSION` channel
  separately; transmission / alpha falls back to the Principled
  default values.

## Capabilities to extend later

These would enable more graphs to take the direct path.  Each is
self-contained and small enough to land as one PR.

- **`remap_painter`** — applies a 1D piecewise-linear function to a
  scalar source painter.  Maps Cycles `ShaderNodeMath` (Add /
  Multiply Add / Power / etc.) without baking.
- **`hsv_painter`** — applies a Hue/Saturation/Value remap to a
  colour-producing painter.  Maps `ShaderNodeHueSaturation` directly
  (currently lossy pass-through).
- **`gradient_painter`** — Cycles `ShaderNodeTexGradient` (linear,
  spherical, easing).  Removes Gradient from the force-bake list.
- **`wave_painter`** — Cycles `ShaderNodeTexWave` (sine, saw, banded,
  ringed).  Currently Gerstner approximates one mode but not the
  others.
- **`brick_painter`** — Cycles `ShaderNodeTexBrick`.  Common in
  architecture / interior scenes.
- **`fresnel_painter`** — view-dependent IOR-based weight.  Would
  also remove `ShaderNodeLayerWeight` from the force-bake list
  partially.
- **`ao_painter`** — a screen / world-space AO query.  The hardest
  on the list; depends on the integrator being able to issue a
  short-distance occlusion query per shading point.  Defer to a
  dedicated review.

### Geometry-nodes proxy bakes

`_find_object_using_material` skips MESH objects whose source mesh
has zero vertices (their visible geometry comes entirely from a
Geometry Nodes modifier; no UV layout to bake against).  Materials
used only on geometry-nodes-instanced objects therefore can't be
auto-baked today — they render with the existing fallback (default
Principled defaults from the wrapper).

To handle those:

1. Detect a geometry-nodes material with no proxy.
2. Create a hidden 1×1 cube proxy, assign the material, smart-UV-unwrap
   it, bake against the cube.  The bake captures the procedural
   pattern at unit-scale UVs; the runtime UV mapping on the actual
   geo-nodes-instanced object then samples the procedural pattern
   like any image texture.
3. Hide / delete the proxy after baking.

This is small enough to be a follow-up (~50 lines) but isn't
implemented today.  Tracked in the
[`rise_bake_geometry_nodes_proxy` task](#).

When adding a new painter to remove a node from the force-bake list,
also add the row to the [Supported node table](#supported-node-table)
in the same commit.

## Per-painter colour-space contract

| Painter slot | Colour space tag |
|--------------|------------------|
| Base Color (diffuse) | `sRGB` (Cycles' default) |
| Roughness | `Linear` (`Rec709RGB_Linear`) — scalar data |
| Metallic | `Linear` — scalar data |
| Normal map | `ROMM_Linear` — bypasses gamma + colour matrix, preserves tangent vectors bit-exactly |
| Emission Color | `sRGB` |
| Transmission Weight | `Linear` |

For procedural painters (Perlin, Voronoi, Checker, etc.) the
colour-space tag has no effect — the painter generates float values
directly without going through the texture color-management pipeline.

## Mapping node (UV transforms)

`ShaderNodeMapping` (vector_type=POINT) between a Texture Coordinate
and an Image Texture is supported via the `RISE_BLENDER_PAINTER_UV_TRANSFORM`
wrapper (ABI v6, commit `30f85ea2`).  The wrapper carries
`uv_offset_u`, `uv_offset_v`, `uv_rotation`, `uv_scale_u`,
`uv_scale_v` — exactly the KHR_texture_transform set RISE's
`UVTransformPainter` consumes.

For procedural painters that read 3D coordinates (`perlin3d`,
`voronoi3d`, `simplex3d`, etc.), Cycles' Mapping node is more
expressive (3D rotation, 3D location).  The bridge translates the
Mapping into a per-painter `xform_translate` / `xform_rotate` /
`xform_scale` triple on the procedural's input.

## Hair / fur export

Slice P2-C of the hair/fur arc (`docs/HAIR_FUR_DESIGN.md`).  Two
independent pieces: exporting a Blender **Curves** object's strand
geometry, and translating a **Principled Hair BSDF** material.  Both
are implemented in `src/Blender/addons/rise_renderer/exporter.py`; the
bpy-free math (the `.hair` binary writer and the melanin conversion
formula) lives in two sibling modules so it can be unit-tested without
a running Blender — see [Testing](#testing) below.

### Object path: Curves → `.hair` file

A Blender object of type `CURVES` (the modern hair/fur curves system —
**not** the legacy NURBS/Bezier `CURVE` type already handled by the
regular mesh path) is exported as:

1. A binary `.hair` file (the Cem Yuksel format `HairFileLoader.h`
   reads — see that header's own comment for the authoritative byte
   layout), written to a staging directory under Blender's own temp
   dir (`<bpy.app.tempdir>/rise_blender_hair/`), the same convention
   `_unpack_image_to_disk` already uses for packed images.  One file
   per Curves object, named `<safe object name>_<id>.hair`.
2. A `HairObjectData` record (`exporter.py`) carrying the file path,
   the resolved material binding, and the object's world transform —
   the export-side equivalent of a `hair_geometry { file ... }` chunk
   bound to a `standard_object`, in the shape this bridge's own
   `SceneData` model uses for everything else (see
   [Native-bridge status](#native-bridge-status) below for why it
   stops there today).

**Staged files are not cleaned up.**  Every export writes a fresh
`.hair` file (`<safe object name>_<id(original_object):x>.hair`) into
the staging directory; nothing in this add-on ever deletes an old one.
This mirrors `_unpack_image_to_disk`'s own practice for its packed-
image temp dir (`rise_blender_unpack/`) — that function also never
prunes stale files, it only re-uses a cache hit within one session.
Re-exporting the same groom across renders in one Blender session
reuses the same filename (`id()` is stable for the life of the Python
object), but a fresh Blender session — or a depsgraph re-evaluation
that hands back a new evaluated-object instance — mints a new `id()`
and therefore a new file, so both staging directories grow unbounded
over a long working session.  Not a correctness issue (each file is
self-contained and harmless to leave behind), but worth knowing before
assuming the temp dir stays small; an artist can safely clear
`<bpy.app.tempdir>/rise_blender_hair/` (and `rise_blender_unpack/`) by
hand between sessions.

**Points and thickness.**  Point positions come from the Curves
datablock's own `position` attribute (local/object space, exactly like
mesh vertices — the object's world transform is applied separately, so
placement works the same way as every other geometry type here).
Thickness comes from the `radius` point attribute when present:
`hair_geometry`'s file-mode thickness (and `HairFileLoader.h`'s own
reading of the ambiguous published spec) is a **FULL WIDTH**, while
Blender's curve `radius` is, as the name says, a radius — so the
writer multiplies by 2.  When a Curves object carries no `radius`
attribute at all, the `.hair` file is written with no thickness array;
RISE's own loader then falls back to its human-hair default width
(with a warning), exactly as it does for any other `.hair` file that
omits thickness.  `width_root` / `width_tip` are left at their default
`1.0` (verbatim) on the RISE side — the radius→width conversion above
already produces the real thickness, so no additional multiplier is
needed.

**Legacy particle-hair systems (`ParticleSettings.type == 'HAIR'`) are
NOT exported.**  Getting their render-time strand geometry needs
either a removed API (`co_hair()`) or a `bpy.ops.object.
modifier_convert` / "Convert Hair to New System" call — an `bpy.ops`
invocation, which this add-on's own bake pipeline
(`material_bake.py`'s "Workflow" docstring) already documents as
hazardous to run from inside a render callback (context-override
requirements, hidden-object refusals, teardown crashes).  Converting a
particle system mid-export would import that same risk into every
render that happens to contain one.  Instead, the exporter emits one
warning per object (the established `_warn_once` idiom every other
unsupported-feature notice in this file uses) pointing the artist at
Blender's own conversion operator (Particle properties → Convert, or
Object → Convert → Curves) — after which the result is a normal
Curves object and exports through the path above like any other
groom.

### Material path: Principled Hair BSDF → `hair_material`

A material whose Material Output.Surface is (transitively through
`NodeReroute`s only) a single `ShaderNodeBsdfHairPrincipled` becomes a
`HairMaterialData` via **direct mapping** — there is no bake path for
hair at all (baking is meaningless for curve geometry: there is no UV
unwrap to bake against). Consequently, where the mesh classifier falls
back to a full-scene bake on anything unsupported, the hair classifier
**refuses the whole material** (falls back to a plausible default
brown-black groom, with a warning) when:

- the Surface chain isn't a single Hair BSDF (a Mix Shader / Add
  Shader / any other node between it and the output), or
- any node feeding one of the Hair BSDF's inputs is outside the same
  node set the mesh path's classifier uses (`material_bake.
  SUPPORTED_UPSTREAM_NODES`, exported publicly from `_SIMPLE_
  TRAVERSABLE_NODES` for this reuse — there is no hair-specific node
  support beyond what the regular translator already handles: an
  Image Texture, a Value node, a Color Ramp, etc. all translate the
  same way regardless of which slot they land in).

Parameter mapping, once a Hair BSDF is found and its upstream graph is
supported:

| Blender input | RISE `hair_material` field | Notes |
|----------------|------------------------------|-------|
| `Roughness` | `beta_m` | Direct value; texture-driven via the same `_scalar_or_texture_painter` path as Principled roughness |
| `Radial Roughness` | `beta_n` | Same |
| `IOR` | `ior` | Same |
| `Offset` (radians) | `alpha` (**degrees**) | Converted via `hair_material_math.offset_radians_to_alpha_degrees` (`math.degrees`). Blender's own default, 2°, is stored as ~0.0349066 rad. Linked inputs are read at their socket default only (warned). |
| `Random Color` / `Random Roughness` / `Random` | *(unsupported)* | RISE's `hair_material` is one BCSDF instance for the whole groom — there is no per-strand attribute plumbing to carry per-strand randomisation. Warned and ignored — including the standard Cycles wiring for these (a `ShaderNodeHairInfo` node feeding them), which is exempted from the upstream-graph support check specifically so it degrades to this warn-and-ignore instead of refusing the whole material. |

The three **parametrizations** (`ShaderNodeBsdfHairPrincipled.
parametrization`) each bind a different `hair_material` tier — exactly
one tier may be bound, matching the chunk's own "exactly one of
`color`/`sigma_a`/`eumelanin`+`pheomelanin`" rule:

- **`COLOR`** ("Direct coloring") — the `Color` input maps straight to
  `hair_material`'s Tier 3 `color`, with full texture support (Image
  Texture / Color Ramp / etc. chains, same as any other colour slot).
- **`ABSORPTION`** ("Absorption coefficient") — the `Absorption Coefficient` input
  (an RGB triple) maps to Tier 2 `sigma_a`.  Read as a constant colour
  at the socket's default value; a linked input is not walked for a
  texture (warned) — sigma_a is authored numerically far more often
  than painted.
- **`MELANIN`** (Blender's own default parametrization) — maps to
  Tier 1 `eumelanin` / `pheomelanin`.  Blender's `Melanin` (`m`, in
  [0, 1]) and `Melanin Redness` (`r`, in [0, 1]) don't correspond
  directly to RISE's two melanin concentrations; they're converted via
  `hair_material_math.melanin_to_eumelanin_pheomelanin`:

  ```
  melanin_qty = -log(max(1 - m, 1e-4))
  eumelanin   = melanin_qty * (1 - r)
  pheomelanin = melanin_qty * r
  ```

  This matches Cycles' own internal conversion in
  `bsdf_hair_principled.h`: the artist-facing `[0, 1]` melanin slider
  isn't itself a physical concentration (Chiang et al. 2016's
  parametrization has no upper bound on melanin), so Cycles first
  log-remaps it into an unbounded absorption-scale quantity, then
  splits that quantity between the two pigments by redness.  The
  `1e-4` floor keeps `m = 1.0` finite (`melanin_qty` caps at
  `-log(1e-4) ≈ 9.21` instead of diverging).  Both inputs are clamped
  to `[0, 1]` before conversion, so an out-of-range value from an
  upstream node edit can't produce a negative or superlinear result.
  Linked `Melanin` / `Melanin Redness` sockets are read at their
  default value only (warned) — same reasoning as `Absorption
  Coefficient` above.

  **Units disclosure — the remap matches Cycles exactly, the resulting
  absorption does not.**  `melanin_to_eumelanin_pheomelanin` converts
  Blender's `Melanin`/`Melanin Redness` sliders into the same
  eumelanin/pheomelanin *concentration* pair Cycles would compute
  (confirmed: identical formula, identical `1e-4` floor). But the
  downstream *concentration → sigma_a* coefficient sets differ between
  the two renderers:

  | pigment | Cycles (`bsdf_hair_principled.h`) | RISE (OMLC, G-anchored) |
  |---------|-----------------------------------|--------------------------|
  | eumelanin | (0.506, 0.841, 1.653) | (0.518, 0.697, 1.293) |
  | pheomelanin | (0.343, 0.733, 1.924) | (0.232, 0.400, 1.067) |

  (RISE's triples are implemented in `src/Library/Materials/HairBSDF.cpp`
  from the in-tree OMLC extinction tables.) At equal concentrations
  this makes RISE absorb roughly **17% less green light for
  eumelanin** (0.697 / 0.841) and roughly **45% less for pheomelanin**
  (0.400 / 0.733) than Cycles — a groom that matches a Cycles reference
  by eye will render slightly lighter / less saturated in RISE at the
  same `Melanin` / `Melanin Redness` values. A per-pigment rescale for
  Blender-parity (`eumelanin *= 0.841/0.697`, `pheomelanin *=
  0.733/0.400`) is a known, straightforward fix, deliberately **not**
  applied — it's left as an open decision for the native-bridge slice
  (Blender-visual-parity vs. RISE's-own-physical-anchoring as the
  intended contract).

A hair-curves object with no material, a non-node material, or a
material that gets refused by the rules above falls back to a
plausible **default groom**: melanin tier, `eumelanin = 1.3` (brown-
black, per the chunk's own parameter description), `pheomelanin = 0`,
and RISE's own `beta_m`/`beta_n`/`alpha`/`ior` defaults
(`0.3`/`0.3`/`2.0`/`1.55`).

### Limitations

- **No per-strand colour or transparency.**  `HairFileLoader.h` reads
  and discards both (RISE has no per-vertex strand opacity or
  albedo) — fibre colour always comes from the bound `hair_material`.
- **Root UVs are always `(0, 0)`.**  The `.hair` format carries no
  per-strand surface parameterization, so a `hair_material` driven by
  a painter over the root UV (a scalp-space tint map, say) would not
  vary across the groom — not that this matters yet, since neither
  `eumelanin`/`pheomelanin` texture-driving nor a scalp-varying `color`
  painter position is wired up on the export side (see the mapping
  table above: only `Color` supports a texture chain at all).
- **Thickness is `2 × Blender radius`.**  See the object-path section
  above.
- **Legacy particle-hair systems are not exported.**  See the
  object-path section above — convert to Curves first.

### Native-bridge status

**None of this is sent to the live-render bridge yet.**
`src/Blender/native/rise_blender_bridge.{h,cpp}` (this add-on's ctypes
ABI, currently version 8 — see `bridge.py`'s `_EXPECTED_API_VERSION`)
has no hair fields: no `_HairObject` / `_HairMaterial` struct, no
`hair_objects` / `hair_materials` arrays on `_Scene`.  That native
surface is out of scope for this slice (it's C++, owned separately —
see `docs/HAIR_FUR_DESIGN.md`'s phase plan).  Concretely, today:

- `exporter.export_scene()` fully computes `SceneData.hair_objects`
  and `SceneData.hair_materials`, and **does** write real `.hair`
  files to disk with correct strand/thickness data — the object and
  material translation logic described above is complete and real,
  not a stub.
- `bridge.py`'s `_marshal_*` functions (the code that packs
  `SceneData` into the ctypes structs the native library actually
  reads) were deliberately **not** touched — inventing ctypes struct
  layouts for an ABI that doesn't exist yet on the C++ side would be
  worse than not shipping them at all (a silent, unverifiable
  contract with nothing to check it against).
- When a scene has any hair objects, `export_scene()` emits one
  warning (surfaced through Blender's own report/info bar, same as
  every other exporter warning) explaining that the groom(s) were
  staged but won't render yet, naming the staging directory.
- Wiring this in is future work: extend `rise_blender_bridge.{h,cpp}`
  with hair struct(s), bump `RISE_BLENDER_API_VERSION` /
  `_EXPECTED_API_VERSION` together, then mirror the new fields into
  `bridge.py`'s ctypes structures and `_marshal_*` functions.

## Testing

End-to-end material parity is regression-checked via:

- `scenes/Tests/Materials/*.RISEscene` (hand-authored)
- The Blender side has no auto-regression yet; visual diffs against
  Cycles / EEVEE are by-hand for now.  A bake-cache snapshot test
  would be a natural addition (compare current bake to a stored
  reference).

### Hair export unit tests

`src/Blender/addons/rise_renderer/test_hair_export.py` is the first
Python-level test harness in this add-on (there was none before —
checked for `test`/`pytest` under `src/Blender` before adding it).  It
covers exactly the two modules that don't import `bpy`
(`hair_file_writer.py`, `hair_material_math.py`) — the bpy-dependent
glue (`_extract_curves_arrays`, the node-graph walking in
`_hair_material_payload` / `_hair_graph_supported` /
`_find_hair_bsdf_through_surface`) has no bpy available in this
repo's test environment and stays **manually validated only** (build
the native bridge, load a scene with a Curves object + a Principled
Hair BSDF material in an actual Blender, render, and inspect the
warnings/output — there is no automated coverage for that half).

Run directly:

```sh
python3 src/Blender/addons/rise_renderer/test_hair_export.py
```

The suite includes: a hand-computed byte-offset check against the
128-byte `.hair` header layout, a roundtrip test against an
independent minimal reader written into the test file itself (not
shared code with the writer, so it actually exercises the byte
layout), ragged strand-length coverage, the radius→full-width
thickness conversion, `write_hair_file`'s error paths (empty groom,
single-point strand, mismatched/inconsistent thickness arrays), the
melanin/redness→eumelanin/pheomelanin conversion (zero/one bounds,
out-of-range clamping, the `1e-4` floor staying finite, non-negativity
across a value grid), and the offset-radians-to-alpha-degrees
conversion (including Blender's own 2° default).
