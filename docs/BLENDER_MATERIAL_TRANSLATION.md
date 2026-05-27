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

## Testing

End-to-end material parity is regression-checked via:

- `scenes/Tests/Materials/*.RISEscene` (hand-authored)
- The Blender side has no auto-regression yet; visual diffs against
  Cycles / EEVEE are by-hand for now.  A bake-cache snapshot test
  would be a natural addition (compare current bake to a stored
  reference).
