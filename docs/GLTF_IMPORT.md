# glTF 2.0 Import for RISE — Analysis & Plan

This document started life as a forward-looking plan; the design rationale is
still useful, but the **Implementation status** table below is the canonical
view of what has actually shipped.  Sections that describe pending work are
marked.

**Scope decision (revised 2026-04-30):** Phase 1 (mesh-only import) ships
first as a self-contained commit.  Tangent-space normal mapping —
originally scoped to Phase 2 — was pulled forward because the v3 tangent
storage added in Phase 1 had no runtime exercise without it; the
adversarial Khronos test corpus made it possible to verify normal mapping
end-to-end immediately.  Full scene import (`gltf_import`), PBR material
wrappers, and the rest of Phase 2 stay deferred.  Phase 3+ (animation,
skinning, KHR extensions) stays deferred.

**Phase 2 status (2026-04-30, this branch, uncommitted):** the bulk of
Phase 2 has shipped — `gltf_import`, `pbr_metallic_roughness_material`,
`channel_painter`, optional `emissive` on `ggx_material`, and embedded-
texture extraction (sidecar-cache form).  Two items were deferred to
Phase 3 by design (`alpha_test_modifier` — RISE modifiers run after hit
commit so alpha mask needs new architecture; `quaternion` / `matrix` on
`standard_object` — current Euler XYZ decomposition is empirically
adequate for the test corpus).  One **architectural correctness gap**
identified in adversarial review remains open: the GGX BRDF's conductor
Fresnel double-counts F0 when the PBR mapping plugs `rs = lerp(0.04,
baseColor, metallic)` on top of it, which makes metals render ~25× too
dim.

**Phase 3 status (2026-04-30, this branch, uncommitted):** all
Phase-2-deferred work has been delivered.  Concretely:

  - **Schlick-from-F0 Fresnel mode on `GGXBRDF` / `GGXSPF`** (fixes
    P1-1 / P1-2 / P1-3) — new `fresnel_mode` parameter on
    `ggx_material` (default `conductor`, preserves existing scenes
    byte-identical).  `pbr_metallic_roughness_material` now flips this
    to `schlick_f0`, which treats the `specular` painter as F0
    directly, evaluates `F = F0 + (1-F0)(1-cosθ_h)^5`, and modulates
    diffuse by `(1 - max(F0))` per glTF spec.  Three test programs
    cover it: `tests/GGXFresnelModeTest.cpp` (analytical oracles),
    `tests/GGXMetalRoughGridTest.cpp` (4-corner grid), and Test 7 of
    `tests/GGXWhiteFurnaceTest.cpp` (energy / PDF / SPF–BRDF
    consistency under Schlick mode).  Metals now render with full F0
    reflectance.
  - **`alpha_test_shaderop`** — new shader op + chunk + Job /
    RISE_API entry points for glTF `alphaMode = MASK`.  Importer
    auto-wires it per-material when MASK is set.  Documented caveat:
    works only under integrators that route through `IShader::Shade()`
    (the path tracer + legacy direct shaders); BDPT, VCM, MLT, and
    photon tracers bypass the shader-op pipeline and treat MASK
    surfaces as opaque.  Tracked as a Phase 4 candidate to promote
    into a hit-time concern.  Per-pixel alpha currently reads
    `max(R,G,B)` of baseColor as a proxy because the painter system
    doesn't expose the A channel — Phase 4 work to add an alpha-aware
    painter would close this.
  - **Quaternion / matrix on `standard_object`** — new optional
    `quaternion` (xyzw) and `matrix` (16 doubles, column-major)
    parameters with documented precedence (`matrix` > `quaternion` >
    `orientation`).  Mutual-exclusion warnings if multiple are set.
    `Job::AddObjectMatrix` constructs the world transform from a
    column-major 4×4 directly, bypassing `Position*Orientation*
    Stretch*Scale` composition.  Importer now passes the cgltf node-
    world matrix verbatim — no Euler decomposition, no gimbal-lock
    failure mode, no orthonormality assumption.  `DecomposeAffine`
    deleted (lost its only caller).
  - **In-memory PNG / JPEG painters** — new
    `Job::AddInMemoryPNGTexturePainter` /
    `AddInMemoryJPEGTexturePainter` that take a byte buffer directly.
    Importer uses these for embedded `.glb` images (and `data:` URIs
    cgltf already decoded into a bufferView), retiring the
    `.gltf_cache/` sidecar round-trip.  External-URI textures in the
    `.gltf` JSON form continue to use the file-path APIs.
  - **`mkFromQuaternion` bug** at
    `Math3D/MatricesOps.h:215-217` — `_2y` and `_2z` were both
    computing `2*a.v.x` instead of `a.v.y` / `a.v.z`.  Fixed (was
    incorrect regardless of glTF and could have produced silently
    wrong rotations elsewhere).

**Test results (Phase 3, run 2026-04-30):** 74 / 75 RISE test
executables pass.  The one failure (`SceneEditorCancelRestartTest`)
is a pre-existing timing-flaky assertion about render-thread cancel
behaviour, unrelated to Phase 3.  All four glTF showcase scenes load
+ render correctly (PBR smoke, Avocado, DamagedHelmet,
MetalRoughSpheres); the matrix path produces correct asset
orientations.  See §13 for the full delivered-vs-deferred breakdown.

## Implementation status

| Area | Committed | This branch (uncommitted) | Pending |
|---|---|---|---|
| **Library: cgltf v1.15** | `extlib/cgltf/{cgltf.h,cgltf.cpp}` (db65457) | — | — |
| **JPEG painter** (glTF prereq) | `jpg_painter` chunk + `JPEGReader` + `stb_image` (cbab01f) | — | — |
| **v3 mesh interface** (`Tangent4`, `ITriangleMeshGeometryIndexed3`) | Header + impl (db65457) | Triangle-mesh intersection now interpolates the tangent + bitangent sign and stamps `bHasTangent` on `RayIntersectionGeometric`; `Object::IntersectRay` transforms the tangent to world space with `m_mxFinalTrans` | — |
| **Mesh loader** (`gltfmesh_geometry`) | `TriangleMeshLoaderGLTF` + chunk + Job/RISE_API (db65457) | `flip_v` default flipped from FALSE to TRUE — see §4 V-convention reconciliation | — |
| **Test corpus** | Box (db65457) | 9 Khronos Sample-Assets `.glb` files committed under `scenes/Tests/Geometry/assets/`: BoxTextured, Duck, Avocado, NormalTangentTest, NormalTangentMirrorTest, VertexColorTest, MultiUVTest, OrientationTest, AlphaBlendModeTest | — |
| **Test program** | `tests/GLTFLoaderTest.cpp` with 5 cases (db65457) | Extended to 14 cases — adversarial coverage of every Phase 1 attribute path (TANGENT, TEXCOORD_1, COLOR_0 v2 cast, multi-mesh boundary, alpha-mode metadata ignore) | — |
| **Tangent-space normal mapping** | — | `Modifiers/NormalMap.{h,cpp}` + `normal_map_modifier` chunk + `Job::AddNormalMapModifier` + `RISE_API_CreateNormalMapModifier`; visual-regression scene `gltf_normal_mapped.RISEscene`; sidecar normal-map PNG + extraction helper script | — |
| **PBR material wrapper** (`pbr_metallic_roughness_material`) | — | `Job::AddPBRMetallicRoughnessMaterial` constructs a GGX + painter graph; chunk parser registered. **Phase 3 update**: switched to Schlick-from-F0 mode in the BSDF, removed the `0.96 * baseColor` retention pre-multiplier (the (1−max(F0)) factor is now applied at evaluation time per the glTF spec).  Metals render at full F0 reflectance.  See §13. | — |
| **Schlick-from-F0 Fresnel mode on GGX BSDF** | — | `enum FresnelMode { eFresnelConductor, eFresnelSchlickF0 }` in `Interfaces/IMaterial.h`; new `fresnel_mode` parameter on `ggx_material` chunk (default `conductor`, byte-identical to pre-Phase-3 behaviour); `pbr_metallic_roughness_material` flips it to `schlick_f0`. Multiscatter uses closed-form Schlick hemisphere average `F0 + (1-F0)/21`. | — |
| **`channel_painter`** for MR-texture extraction | — | Single-header `Painters/ChannelPainter.h` (R/G/B selector + scale + bias); chunk parser registered | — |
| **Bulk scene import** (`gltf_import`) | — | `Importers/GLTFSceneImporter.{h,cpp}` walks scene tree, emits per-primitive geometries, materials, lights, cameras; `Job::ImportGLTFScene` + `RISE_API_ImportGLTFScene`; chunk parser registered. **Phase 3 update**: embedded `.glb` images go directly through `Job::AddInMemoryPNG/JPEGTexturePainter` (no disk round-trip); `.gltf_cache/` sidecar retired; node-world matrices flow through `Job::AddObjectMatrix` verbatim (no Euler decomposition); skinning / animation / morph targets warn-and-skip; alphaMode = MASK auto-wires per-material alpha-test shader. | — |
| **In-memory PNG / JPEG painters** | — | `Job::AddInMemoryPNGTexturePainter` / `AddInMemoryJPEGTexturePainter` consume a byte buffer (no disk path) and reuse the existing painter pipeline.  Used by `gltf_import` for embedded-image bytes. | — |
| **Alpha modes** (`alpha_test_shaderop` for MASK) | — | `Shaders/AlphaTestShaderOp.{h,cpp}` + `alpha_test_shaderop` chunk + `Job::AddAlphaTestShaderOp` + `RISE_API_CreateAlphaTestShaderOp`.  glTF importer auto-wires per-material when `alphaMode = MASK`.  **Caveat**: shader-op is honoured only by integrators that route through `IShader::Shade()` — PT and legacy direct shaders.  BDPT, VCM, MLT, and photon tracers bypass the shader-op pipeline and treat MASK as opaque (no runtime warning). Per-pixel alpha currently uses `max(R,G,B)` of baseColor as a proxy because `IPainter` does not expose the A channel. | Phase 4: alpha-aware painter, alpha mask under BDPT/VCM/MLT, `alphaMode = BLEND` |
| **Quaternion / matrix on `standard_object`** | — | New optional `quaternion` (xyzw) and `matrix` (16 doubles, column-major) parameters on `standard_object`; `Job::AddObjectMatrix` consumes a 4×4 directly.  Mutual-exclusion warnings if multiple are set; precedence is `matrix` > `quaternion` > `orientation` (Euler).  glTF importer uses the matrix path; `DecomposeAffine` deleted. | — |
| **`mkFromQuaternion` bug** | — | `Math3D/MatricesOps.h:215-217` — `_2y` and `_2z` were both computing `2 * a.v.x` instead of `a.v.y` / `a.v.z`.  Fixed; needed by the new `standard_object { quaternion ... }` path. | — |
| **Emissive on `ggx_material`** | — | Second `GGXMaterial` ctor takes optional `emissive` painter + `emissive_scale`, builds a `LambertianEmitter` and exposes it via `GetEmitter()`; `Job::AddGGXEmissiveMaterial`; parser params `emissive` / `emissive_scale` on `ggx_material` chunk | — |
| **Animation / skinning / morph targets** | — | — | Phase 4+ |
| **KHR_materials_*** extensions (clearcoat, transmission, sheen, ior, emissive_strength, unlit) | — | — | Phase 4+ |

The forward-looking design rationale still lives in §§ 1–6; the original
phased plan ships in §7.  §10 has a per-row status column matching the
table above.  §11 marks the open questions resolved by current work.

---

## 1. What's in a glTF file

glTF 2.0 is a JSON-described scene with binary buffer attachments. Two on-disk
forms: `.gltf` (JSON + sidecar `.bin` + sidecar images) and `.glb` (single
binary container with embedded JSON, buffers, images). Both encode the same
logical structure:

| Concept | Data | RISE counterpart | Notes |
|---|---|---|---|
| **buffer / bufferView / accessor** | Raw bytes → typed strided views → typed arrays | n/a — internal to the loader | Pure plumbing; the library handles it. |
| **mesh.primitives[]** | Vertex attributes + indices + material ref + topology | `IndexedTriangle` via `Job::AddIndexedTriangleMeshGeometry` | RISE only supports triangles. STRIPS/FANS get converted; LINES/POINTS rejected. |
| **mesh.morphTargets** | Per-vertex deltas for blend shapes | None | Phase 4+. |
| **material** (PBR metallic-roughness) | baseColor, metallic, roughness, normal, occlusion, emissive, alphaMode, doubleSided | `ggx_material` + painter composition (see §6) | Already three-lobe + multiscatter + Fresnel. PBR mapping is a parser-level decomposition. |
| **texture / sampler / image** | Image + filter/wrap settings | `png_painter` / `hdr_painter` / `exr_painter` / `tiff_painter` / **`jpg_painter`** (new) | Most glTF in the wild ships JPEG. JPEG support spun off as a separate task. |
| **camera** (perspective / orthographic) | yfov, znear, zfar / xmag, ymag — **no position** (comes from node) | `pinhole_camera` / `orthographic_camera` | Position from flattened node transform. RISE has only one active camera; first wins. |
| **node** (scene graph) | TRS or 4×4 matrix, plus optional mesh/camera/light/skin reference and child list | `standard_object` (flat — no hierarchy) | Hierarchy must be flattened by multiplying parent matrices. Quaternion → Euler is lossy; see §10 enhancement. |
| **scene[]** | Top-level scene picks roots from the node list | RISE has one implicit scene per Job | Honor `scene` field; ignore others or warn. |
| **animation** | Channels target node TRS or morph weights; LINEAR / STEP / CUBICSPLINE samplers | `Keyframe` / `Timeline` | Only LINEAR straightforward. CUBICSPLINE needs baking. Morph weights have no target. Phase 4+. |
| **skin** | Joint nodes + inverse-bind matrices; JOINTS_0/WEIGHTS_0 vertex attrs | None | Per-vertex CPU skinning + BVH refit (`UpdateVertices` exists). Phase 4+. |
| **KHR_lights_punctual** | point / spot / directional lights on nodes | `omni_light` / `spot_light` / `directional_light` | Direct mapping. |
| **KHR_materials_*** | `unlit`, `transmission`, `clearcoat`, `sheen`, `volume`, `ior`, `specular`, `emissive_strength`, `iridescence`, `anisotropy`, `dispersion`, `pbrSpecularGlossiness` | Mostly mappable to RISE materials | Warn-and-skip in v1 except `KHR_materials_emissive_strength` (trivial scalar) and `KHR_materials_unlit` (skip BSDF, use luminaire). |
| **KHR_draco_mesh_compression / EXT_meshopt_compression** | Compressed buffers | None — neither cgltf nor fastgltf decodes these | Reject with clear error in v1. |

**Byte-level reality:** an accessor + bufferView + buffer triple resolves to a
typed strided pointer into a byte blob. Decoding it is mechanical: read
positions as 3 floats, indices as ushort/uint, etc. Components can be
normalized (UNORM/SNORM) and need rescaling. None hard, but the kind of
detail you outsource to a library.

---

## 2. C++ glTF library survey

Three serious contenders. Avoid Assimp (60+ formats, large transitive deps,
bigger than RISE's whole extlib combined).

### cgltf — https://github.com/jkuhlmann/cgltf

**Pros**
- **Zero dependencies.** Single header. C99. Drops in next to libpng/zlib/libtiff.
- **Doesn't impose an image loader.** Parses image references but leaves bytes for the caller. Lets us route JPEG/PNG/HDR/EXR through RISE's existing painter system instead of duplicating decode paths.
- **Small and auditable.** ~5k LOC. Easy to read, easy to debug.
- **Well-maintained and battle-tested.** Used by raylib, bgfx, ozz-animation, Tracy. Regular releases.
- **Supports GLB + most KHR extensions.** Including KHR_lights_punctual, KHR_materials_unlit, KHR_materials_pbrSpecularGlossiness, etc. (extension *parsing* only — interpretation is on us).
- **MIT licensed.** Compatible with RISE.

**Cons**
- **C API.** Pointer-heavy. Parsed structs use `cgltf_*` types that the caller must traverse manually. Not "ergonomic" by C++17 standards.
- **No automatic image decode.** A pro for us, but means we write the wiring.
- **No validation by default.** Will happily accept a malformed file. Need to call `cgltf_validate()` after parse.

### tinygltf — https://github.com/syoyo/tinygltf

**Pros**
- **C++11 STL-friendly API.** Returns `std::vector` / `std::string` / `std::map`-shaped data.
- **Auto-loads images** via stb_image — which means JPEG support comes for free.
- **Mature.** Used by Falcor, NVIDIA samples, Filament tools.
- **Single header** (well — header + a few separable helpers).

**Cons**
- **Adds a parallel image path.** stb_image bypasses RISE's `png_painter`/`hdr_painter`/`exr_painter`. Either we use it (and have two PNG decoders — disk inflation, behavior drift risk) or we disable its image decode and write the wiring anyway (losing the JPEG-for-free benefit). Loses the cgltf advantage either way.
- **Brings nlohmann/json + stb_image** as transitive deps. ~50k LOC of dependencies for our use case.
- **Heavier compile time** because of nlohmann/json + STL containers in the public API.
- **Larger surface to misuse.** API exposes JSON and binary state more directly.

### fastgltf — https://github.com/spnda/fastgltf

**Pros**
- **Fastest of the three** (3–10× cgltf for cold parse). Uses simdjson under the hood.
- **Cleanest modern C++17/20 API.** `std::expected`-style error handling, type-safe accessor reads.
- **Active development.** Used by Vulkan-Samples, several modern realtime engines.
- **Optional image decoding.** Same flexibility as cgltf.

**Cons**
- **Adds simdjson** (~10k LOC dep) and requires CMake to build.
- **C++17 minimum.** RISE compiles cleanly but verifying portability across the five build projects (Make, CMake/Android NDK, VS2022, Xcode) is real work.
- **Less battle-tested.** Younger than the other two; smaller user base in offline rendering.
- **Speed advantage is irrelevant** for our use case. We parse a glTF file once at scene load, then spend hours rendering. simdjson saves milliseconds we don't need.

### Decision: cgltf, vendored

cgltf wins on three axes that matter for an offline path tracer:

1. **No transitive deps** — fits the existing extlib pattern (libpng/zlib/libtiff/openexr are all small vendored C libs).
2. **No imposed image loader** — keeps RISE's painter system as the single decode path; JPEG support added via the spun-off `jpg_painter` task uses the same path.
3. **C99 portability** — slots into all five build projects without grief.

The C-API ergonomics cost is bounded: we write one `TriangleMeshLoaderGLTF.cpp`
that wraps the C structs, and one `GLTFSceneImporter.cpp` for Phase 2. Vendor
as `extlib/cgltf/cgltf.h` + a one-liner `cgltf.c`.

---

## 3. Mesh vertex attributes — what glTF supplies, what RISE stores, what to add

### glTF standard attributes per primitive

| Attribute | Type | Required? | What it's for |
|---|---|---|---|
| `POSITION` | vec3 float | yes | Vertex position |
| `NORMAL` | vec3 float | no | Smooth-shading normal |
| `TANGENT` | **vec4 float** | no | Tangent direction (xyz) + bitangent sign (w = ±1). For tangent-space normal mapping. |
| `TEXCOORD_0` | vec2 (float / u8 normalized / u16 normalized) | no | Primary UV |
| `TEXCOORD_1` | vec2 | no | Second UV — typically lightmap or AO map |
| `COLOR_0` | vec3 or vec4 (float / u8 / u16 normalized) | no | Vertex color |
| `JOINTS_0` | uvec4 (u8 / u16) | no | Skin joint indices |
| `WEIGHTS_0` | vec4 (float / u8 / u16 normalized) | no | Skin joint weights |
| `TEXCOORD_n`, `COLOR_n`, `JOINTS_n`, `WEIGHTS_n` | varies | no | Additional sets — UVs up to ~9 in practice, additional joint sets when >4 influences/vertex |
| `_CUSTOM` (underscore prefix) | varies | no | Application-defined attributes (barycentric, random-per-vertex, etc.) |

Plus **morph target deltas** per primitive (POSITION/NORMAL/TANGENT × N targets).

### What RISE stores today

`ITriangleMeshGeometryIndexed` ([src/Library/Interfaces/ITriangleMeshGeometry.h](../src/Library/Interfaces/ITriangleMeshGeometry.h)) supports:

- positions (separate index)
- normals (separate index)
- texcoords — **one set** (separate index)
- vertex colors via `ITriangleMeshGeometryIndexed2::AddColor` (color index = position index by glTF convention; the existing comment at line 130 already names "glTF COLOR_0" as the target)

### What to add to RISE for v1

Two new attribute storages, both small extensions to the existing pattern.

- **TANGENT (vec4).** High-value addition.  Without it, glTF normal maps either get ignored or computed from screen-space derivatives (low quality).  Add via a new `ITriangleMeshGeometryIndexed3` interface that adds `AddTangent(const Tangent4&)` / `AddTangents(...)` / `getTangents()`, parallel to how v2 added colors.  Tangent index follows position index (same convention as colors).  ~~Surface this at hit-time via a new `tangent_painter` (parallel to `vertex_color_painter`) so a tangent-space normal map modifier can read it.~~ The tangent-space `NormalMap` modifier reads the interpolated tangent + bitangent-sign directly off `RayIntersectionGeometric` (`vTangent` / `bitangentSign` / `bHasTangent`, populated by `TriangleMeshGeometryIndexed::IntersectRay`); no separate `tangent_painter` chunk is needed.
- **TEXCOORD_1 (vec2).** Lower-value but cheap.  Path tracers compute lighting from scratch so lightmaps are pointless, but **occlusion textures** (glTF's `occlusionTexture`, often packed into the R channel of the metallic-roughness texture but sometimes in a separate map keyed off TEXCOORD_1) are useful as a multiplier on diffuse.  Add via the same v3 interface (`AddTexCoord1` / `getTexCoords1`).  If we don't add this, the importer falls back to TEXCOORD_0 for occlusion and warns.

**Status:** delivered in db65457 (storage + loader path) and this branch
(intersection-time interpolation + Object-level tangent transform).
Empirically verified end-to-end via the `NormalTangentMirrorTest` adversarial
asset (mirror-UV diagnostic — see §4 V-convention reconciliation).

### What to skip in v1 (warn-and-discard at load time)

- **JOINTS_n / WEIGHTS_n** — no skinning infrastructure.
- **COLOR_n** for n ≥ 1 — almost no real assets use this.
- **TEXCOORD_n** for n ≥ 2 — vanishingly rare.
- **`_CUSTOM` attributes** — application-specific, no general interpretation.
- **Morph target deltas** — needs animation infrastructure that doesn't exist.

### Mesh interface changes

Add `ITriangleMeshGeometryIndexed3 : public virtual ITriangleMeshGeometryIndexed2` to [src/Library/Interfaces/ITriangleMeshGeometry.h](../src/Library/Interfaces/ITriangleMeshGeometry.h):

```cpp
class ITriangleMeshGeometryIndexed3 : public virtual ITriangleMeshGeometryIndexed2
{
public:
    // Tangent vec4 — xyz = object-space tangent, w = bitangent sign (±1).
    // Tangent index follows position index (the glTF convention; matches
    // colors).  For meshes that lack tangents, the renderer computes them
    // on first request from positions+normals+UVs and caches.
    virtual void AddTangent ( const Tangent4& t ) = 0;
    virtual void AddTangents( const Tangent4ListType& ts ) = 0;
    virtual unsigned int numTangents() const = 0;
    virtual Tangent4ListType const& getTangents() const = 0;

    // Optional secondary UV set (TEXCOORD_1).  Same indexing as TEXCOORD_0
    // (i.e. uses iCoords[] from IndexedTriangle — second UV uses the same
    // face-vertex index, NOT a separate index list).
    virtual void AddTexCoord1 ( const TexCoord& t ) = 0;
    virtual void AddTexCoords1( const TexCoordsListType& ts ) = 0;
    virtual unsigned int numTexCoords1() const = 0;
    virtual TexCoordsListType const& getTexCoords1() const = 0;
};
```

The v3 interface follows the v2 pattern: separate sub-interface preserves the
v1/v2 vtable for any out-of-tree subclass. Loaders `dynamic_cast` to v3 and
silently fall back if the cast fails.

---

## 4. Material strategy — `ggx_material` + `blend_painter` ≈ glTF metallic-roughness

The PBR delta turned out to be **much smaller** than initially estimated.

### What `ggx_material` already implements

From [src/Library/Materials/GGXSPF.h](../src/Library/Materials/GGXSPF.h) (header comment):

> Three-lobe mixture model:
>   1. Diffuse: cosine hemisphere sampling
>   2. Specular: anisotropic VNDF sampling (Dupuy & Benyoub 2023)
>   3. Multiscatter: cosine hemisphere sampling
>
> Height-correlated Smith G2 (Heitz 2014), Kulla-Conty multiscattering (2017).

Chunk parameters ([AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp), keyword `ggx_material`):

- `rd` — diffuse reflectance (painter)
- `rs` — specular reflectance / **F0** (painter)
- `alphax` / `alphay` — anisotropic roughness (painters)
- `ior` — Fresnel IOR (painter)
- `extinction` — Fresnel extinction (painter)

That is **already the metallic-roughness BRDF** — diffuse lobe + GGX specular lobe with painter-driven F0 and roughness, with proper multiscattering compensation. The glTF spec prescribes:

```
c_diff = lerp(baseColor.rgb * (1 - 0.04), 0, metallic)        // diffuse color
f0     = lerp(0.04, baseColor.rgb, metallic)                  // F0
α      = roughness * roughness                                 // GGX α
```

then `BRDF = c_diff/π + GGX_specular(f0, α)`. RISE's GGX evaluates exactly that
shape. We just need to construct the right `rd`, `rs`, `alpha` painters.

### `blend_painter` does the lerp

From [src/Library/Painters/BlendPainter.h:60](../src/Library/Painters/BlendPainter.h):

```cpp
return ca*cmask + cb*(RISEPel(1.0,1.0,1.0)-cmask);
```

Per-channel `lerp(cb, ca, mask)`. Exactly the operation glTF metallic-roughness
needs. Combined with `uniformcolor_painter` for the constants (0.04, white,
black), we can express the entire glTF PBR mapping with **zero new BSDF code**.

### Concrete mapping

For a glTF material with baseColorTexture, metallicRoughnessTexture (G=roughness, B=metallic), and metallicFactor / roughnessFactor:

```
# Base color texture (sRGB → linear handled by png_painter color_space)
png_painter   bc_tex      { file baseColor.jpg  color_space sRGB }

# Metallic and roughness from packed MR texture — needs channel extraction
# (see §10 enhancement: channel_painter)
channel_painter metal_p   { source mr_tex  channel B  scale $metallicFactor }
channel_painter rough_p   { source mr_tex  channel G  scale $roughnessFactor }

# Constants
uniformcolor_painter zero  { color 0   0   0 }
uniformcolor_painter f0    { color 0.04 0.04 0.04 }
uniformcolor_painter bc096 { color 0.96 0.96 0.96 }   # (1 - 0.04)
blend_painter bc_diffuse  { colora bc_tex  colorb zero  mask bc096 }   # bc * 0.96

# diffuse: 0 for metals, baseColor*0.96 for non-metals
blend_painter rd_painter  { colora zero    colorb bc_diffuse  mask metal_p }

# F0: baseColor for metals, 0.04 for non-metals
blend_painter rs_painter  { colora bc_tex  colorb f0          mask metal_p }

# Final material
ggx_material gltf_pbr {
    rd         rd_painter
    rs         rs_painter
    alphax     rough_p
    alphay     rough_p
    ior        f0           # any constant works; F0 dominates the Fresnel
    extinction zero
}
```

That's the entire mapping. **No new BSDF.** The required additions are:

1. **`channel_painter` chunk** — extract a single R/G/B/A channel from another painter, optionally scaled. Small new painter class (~50 lines) + chunk parser. Or: add a `channel` parameter to the existing image painters.
2. **Convenience: `pbr_metallic_roughness_material` chunk** — single-chunk authoring sugar. Takes `base_color`, `metallic`, `roughness`, `normal_map`, `emissive`, `ior` painters (with sensible defaults) and constructs all of the above internally. Pure parser-side decomposition; the runtime is still a `ggx_material`. ~100 lines of parser code + a small `Job::AddPBRMetallicRoughnessMaterial` API method that wires up the painters.

### Emissive

glTF emissive = additive RGB, optionally textured. RISE has `LambertianEmitter`,
`PhongEmitter`, and `CompositeEmitter` ([src/Library/Materials/](../src/Library/Materials/)).
Use `composite_emitter` (chunk parser exists for the underlying `lambertian_luminaire_material`
+ regular material via the `composite_material` path) **OR** add a small
`emissive` parameter to `ggx_material` that adds an emitter on top. The latter
is cleaner and avoids two chunks per PBR material.

### Alpha modes

glTF `alphaMode = OPAQUE / MASK / BLEND` + `alphaCutoff`. In a path tracer:

- OPAQUE → no-op.
- MASK → stochastic alpha test: at hit time, sample alpha; if `alpha < alphaCutoff` continue the ray. Implementable as a new `alpha_test_modifier` (parallels `bumpmap_modifier`).
- BLEND → stochastic transparency through `transparency_shaderop`.

Recommend: implement MASK in v1 (it's the common case for foliage / grates),
defer BLEND to a later phase. Both can be added later without breaking v1.

### Normal maps

Tangent-space normal maps need (a) tangent storage (added in §3) and (b) a new
`normal_map_modifier` that takes a normal-map painter and perturbs the
geometric normal at hit time.  RISE has `bumpmap_modifier` already, which
handles single-channel height maps; the normal-map version is a sibling.

**Status:** **delivered in this branch.**  See `Modifiers/NormalMap.{h,cpp}`,
the `normal_map_modifier` chunk in `AsciiSceneParser.cpp`, and the
visual-regression scene `scenes/Tests/Geometry/gltf_normal_mapped.RISEscene`.
Implementation follows the glTF 2.0 §3.7.2.1.4 spec form `B = cross(N, T) *
tangent.w`, with two extra runtime details worth knowing:

1. **Object-transform handedness is folded into the bitangent sign.** When a
   `standard_object` is instanced with an orientation-reversing transform
   (e.g. `scale -1 1 1` or any reflective transform with `det(M) < 0`),
   `Object::FinalizeTransformations` caches `m_tangentFrameSign = sign(det)`
   and `Object::IntersectRay` multiplies it into `ri.geometric.bitangentSign`
   before the modifier runs.  Without this, mirrored instances of a
   normal-mapped mesh would shade inverted relative to identity-transform
   instances.  The `gltf_normal_mapped.RISEscene` regression scene renders
   one identity instance + one `scale -1 1 1` instance side-by-side; both
   must show the same bump shading direction.
2. **Three-tier tangent-frame fallback** when the hit has no imported
   TANGENT:
   - Imported TANGENT (best path; honours mirrored UVs via `tangent.w`).
   - `ri.derivatives.dpdu` re-orthogonalised against `N` (silent fallback;
     correct on any connected UV chart, populated by triangle-mesh
     intersection — this is the case for any glTF asset whose source mesh
     happened to ship without TANGENT but did ship TEXCOORD_0).  Bitangent
     comes from `cross(N, T)` with no imported sign source.
   - ONB-derived tangents (`ri.onb.u()` / `.v()`) with a one-warning-per-
     process diagnostic — only reached when neither TANGENT nor valid
     surface derivatives exist.  ONB orientation is essentially never
     aligned with the normal map's UV axes, so this path is wrong for
     virtually any real normal map; the warning text says so.

The modifier is independent of the future `pbr_metallic_roughness_material` —
users can attach it to any object today.

### V-convention reconciliation (Phase 1 finding)

While verifying normal mapping against `NormalTangentMirrorTest` we found a
silent V-axis convention mismatch between glTF and RISE that affects
**every image-based painter**, not just normal maps:

- **glTF authors with V increasing upward** (OpenGL convention) — V=0 is the
  bottom of the texture, V=1 the top.
- **RISE samples with V increasing downward** (DirectX convention).  See
  `BilinRasterImageAccessor::GetPel` (and the NNB / Bicubic siblings):
  pixel-row index is computed as `V * height`, indexing from row 0 = top of
  the loaded image.  PNG / JPEG / EXR / HDR / TIFF readers all populate
  rows top-to-bottom, so the in-memory storage matches "row 0 = top".

Without compensation, every glTF-sourced texture renders V-mirrored.  For
plain colour textures the mirroring is sometimes invisible (the bumps don't
care which way is up).  For normal maps it produces a subtler symptom: in
mirrored-UV regions of an asset (where the asset's own TANGENT.w flips), the
combined effect of the asset's mirror + RISE's silent V flip leaves the
mirrored half rendering with inverted bumps relative to the non-mirrored
half — exactly the failure mode `NormalTangentMirrorTest` is designed to
catch.

**The fix lives at the right layer.**  We compensate at glTF mesh-load time:
`gltfmesh_geometry`'s `flip_v` parameter defaults to **TRUE**, so the loader
stores `(u, 1−v)` instead of `(u, v)` in `pCoords`.  Other mesh formats
(PLY, 3DS, RAW2) keep their native V conventions; the texture painters keep
their consistent V-down sampling; the only place that knows about glTF's
convention difference is the only place that crosses a format boundary.
Override with `flip_v FALSE` only for atypical glTF assets that were
exported with DirectX V already baked in.

This finding **must propagate into Phase 2's `gltf_import`**: the bulk
importer needs to apply the same V flip.  Trying to "fix RISE" by flipping
V in the painter or image readers would break every existing PLY/3DS-based
scene that's been authored against the V-down convention.

### Material story summary

| RISE addition | Status | Lines | Purpose |
|---|---|---|---|
| `channel_painter` chunk + class | pending (Phase 2) | ~80 | Extract single channel for MR textures |
| `pbr_metallic_roughness_material` chunk + Job API | pending (Phase 2) | ~150 | Authoring sugar; constructs the GGX + blend painter graph |
| Optional `emissive` param on `ggx_material` | pending (Phase 2) | ~30 | Avoid double-chunk for emissive PBR |
| `alpha_test_modifier` chunk + class | pending (Phase 2) | ~100 | glTF alphaMode = MASK |
| `normal_map_modifier` chunk + class | **delivered (this branch)** | ~120 | glTF normalTexture |

No new BSDF. No new SPF. No new Material class. The remaining Phase 2
material work is parser-side composition of pieces RISE already has.

---

## 5. Camera, light, and transform mapping

### Cameras

| glTF | RISE | Mapping |
|---|---|---|
| `perspective` (yfov, znear, zfar, aspectRatio?) | `pinhole_camera` | yfov → vertical FOV. aspectRatio → set width/height ratio if present, else use rasterizer's. znear/zfar — RISE doesn't use them (path tracer); accept and ignore. |
| `orthographic` (xmag, ymag, znear, zfar) | `orthographic_camera` | xmag/ymag → frustum half-extents. |

Camera position comes from the **node transform** (glTF cameras are typed
without position). RISE has only one active camera per Job — first imported
camera wins; subsequent cameras get a warning. Imported camera names get
`name_prefix.cam.<index>`.

### Lights (`KHR_lights_punctual`)

| glTF | RISE | Notes |
|---|---|---|
| `point` (color, intensity) | `omni_light` | Position from node. Intensity is in candela (cd) per spec — convert to RISE's units. |
| `spot` (innerConeAngle, outerConeAngle, color, intensity) | `spot_light` | Position + direction from node transform. RISE uses single cone angle; map to the glTF outer cone, with a soft falloff approximating the inner-to-outer transition. |
| `directional` (color, intensity) | `directional_light` | Direction from node transform's -Z axis (per spec). Intensity is in lux (lm/m²). |

Range-based attenuation (`range`) — RISE uses physical inverse-square; ignore
the cutoff or warn if it's set to a non-default value (it's an artistic
falloff, not physical).

### Node transform flattening

Walk the node tree depth-first, accumulating world matrices:

```
world_matrix(node) = parent_world_matrix × local_matrix(node)
local_matrix(node) = node.matrix  if node has matrix
                   = T(node.translation) × R(node.rotation) × S(node.scale)  otherwise
```

Decompose final 4×4 into translation + rotation + scale. **The lossy step:**
RISE's `standard_object` takes Euler angles in degrees. Quaternion → Euler
conversion is lossy at gimbal-lock configurations.

**Recommended enhancement (§10):** Add a `quaternion` parameter to
`standard_object` (vec4, xyzw) that takes precedence over `orientation` when
present. Importer emits quaternion form; users authoring by hand can stick with
Euler. Minimal change to the chunk parser; storage is the same internally
(orientation is already stored as a matrix once finalized).

---

## 6. What's deliberately out of scope for v1

- **Animation.** glTF samplers + RISE Timeline; LINEAR mappable, STEP/CUBICSPLINE need baking, morph weights have no target.
- **Skinning.** No CPU skin pass + BVH refit pipeline yet (refit hook exists but unused for skinning).
- **Morph targets.** Same.
- **`KHR_draco_mesh_compression` / `EXT_meshopt_compression`.** Reject with clear error.
- **`KHR_materials_*`** beyond `unlit`, `emissive_strength`, `ior` (which feeds GGX directly). Warn-and-skip with the extension name in the warning so users know what was lost.
- **alphaMode = BLEND.** Only OPAQUE and MASK in v1.
- **Multi-camera.** First camera wins.
- **Multi-scene.** Honor `scene` field; ignore others.

Each of these is a separate phase, not a v1 limitation — the v1 importer logs
exactly what it skipped and why.

---

## 7. Phased implementation plan

### Phase 1 — `gltfmesh_geometry` (mesh-only import) — **DELIVERED**

Single-chunk surface; user assembles the rest of the scene with existing
chunks.  Proves out the cgltf dependency and the mesh-construction path.

**Shipped in commit db65457** (initial drop) plus this branch's
adversarial-test extension and tangent plumbing.  The original 2-day
estimate held; the adversarial-test corpus + tangent-space normal mapping
extension was the bulk of the additional work.  See the **Implementation
status** table at the top of this doc for what's committed where.

**Surface:**
```
gltfmesh_geometry {
    name         my_mesh
    file         models/duck.glb       # .gltf or .glb auto-detected
    mesh_index   0                     # which mesh in the file (default 0)
    primitive    0                     # which primitive in the mesh (default 0)
    double_sided FALSE
    face_normals FALSE
    flip_v       TRUE                  # default; flips glTF V-up to RISE V-down
                                       # (see §4 V-convention reconciliation).
                                       # Override to FALSE only for atypical
                                       # exports with DirectX V already baked in.
}
```

**What it handles:** POSITION / NORMAL / TANGENT / TEXCOORD_0 / TEXCOORD_1 /
COLOR_0 / indices. Triangle topology (TRIANGLES, plus STRIPS/FANS converted to
TRIANGLES). UNORM/SNORM normalized accessors.

**What it warns-and-skips:** every other attribute, every KHR_* material
extension, every animation/skin/morph reference.

**Files to touch (per CLAUDE.md "five build projects" rule):**

1. **New:** `extlib/cgltf/cgltf.h` (vendored single header) + `extlib/cgltf/cgltf.c` (one-line implementation TU).
2. **Update:** `extlib/README.TXT` — add cgltf section with license, source URL, version.
3. **New:** `src/Library/Geometry/TriangleMeshLoaderGLTF.{h,cpp}` — parallels `TriangleMeshLoader3DS`.
4. **Extend:** `src/Library/Interfaces/ITriangleMeshGeometry.h` — add `ITriangleMeshGeometryIndexed3` (tangents + TEXCOORD_1).
5. **Extend:** `src/Library/Geometry/TriangleMeshGeometryIndexed.{h,cpp}` — implement v3 interface.
6. **Extend:** `src/Library/RISE_API.{h,cpp}` — `RISE_API_CreateGLTFTriangleMeshLoader(...)`.
7. **Extend:** `src/Library/Job.{h,cpp}` + `src/Library/Interfaces/IJob.h` — `Job::AddGLTFTriangleMeshGeometry(name, file, meshIndex, primIndex, doubleSided, faceNormals, flipV)`.
8. **Extend:** `src/Library/Parsers/AsciiSceneParser.cpp` — `GLTFMeshGeometryAsciiChunkParser` next to `PLYMeshGeometryAsciiChunkParser` (~line 3503), registered in `CreateAllChunkParsers()`.
9. **New:** `tests/GLTFLoaderTest.cpp` — load known glTF, verify vertex/triangle counts, attribute presence, position accuracy.
10. **New scenes:** `scenes/Tests/Geometry/gltf_box.RISEscene`, `scenes/Tests/Geometry/gltf_duck.RISEscene` (parser regression + visual check).

**Build wiring (5 places):**
- `build/make/rise/Filelist`
- `build/cmake/rise-android/rise_sources.cmake`
- `build/VS2022/Library/Library.vcxproj` (`<ClCompile>` for `.c`/`.cpp`, `<ClInclude>` for `.h`)
- `build/VS2022/Library/Library.vcxproj.filters`
- `build/XCode/rise/rise.xcodeproj/project.pbxproj` (4 sections × 2 targets per CLAUDE.md)

Plus include path entries pointing at `extlib/cgltf` in each.

**Estimated effort:** 2 days.

---

### Phase 2 — full scene import + PBR materials + tangent / normal-map support — **DELIVERED (this branch, uncommitted)**

Tangent storage (v3 interface), v3 intersection plumbing, and the
`normal_map_modifier` shipped in Phase 1 / pre-Phase 2.  Phase 2 itself
delivers `gltf_import`, `pbr_metallic_roughness_material`, `channel_painter`,
optional `emissive` on `ggx_material`, embedded-texture extraction (sidecar
form), and the per-primitive node-transform decomposition.

**Deferred from Phase 2 to Phase 3 (see §13):**
- `alpha_test_modifier` — needs new pre-commit hook on `IModifier`; current
  modifiers run after hit commit which is too late for stochastic alpha.
- `quaternion` / `matrix` parameter on `standard_object` — current Euler XYZ
  decomposition (`DecomposeAffine`) is empirically adequate for the test
  corpus.  Revisit when an asset surfaces a gimbal-lock failure.
- BRDF correctness fix — see §13 P1-1/P1-2/P1-3.

Adds `gltf_import` (the bulk import chunk), `pbr_metallic_roughness_material`
(authoring sugar), `channel_painter` (MR texture extraction),
`normal_map_modifier` (already shipped), and optional `emissive` /
`emissive_scale` parameters on `ggx_material`.

**Surfaces:**

```
# Bulk import: instantiates many named objects from one chunk
gltf_import {
    file              scenes/sponza.glb
    name_prefix       sponza             # all created objects prefixed
    # `scene_index` is OMITTED here so the importer uses the file's
    # default scene (the glTF JSON's top-level `"scene"` field).
    # Pass an explicit zero-based index only to force a non-default
    # scene from a multi-scene asset.
    import_cameras    TRUE
    import_lights     TRUE
    import_materials  TRUE
    on_name_collision suffix             # 'suffix' | 'replace' | 'error'
}

# Authoring sugar: hand-author a PBR material
pbr_metallic_roughness_material {
    name           bronze
    base_color     bc_painter            # painter (texture or uniform)
    metallic       metal_painter         # painter, scalar 0..1
    roughness      rough_painter         # painter, scalar 0..1
    normal_map     normal_painter        # painter (optional)
    emissive       emissive_painter      # painter (optional)
    ior            ior_painter           # default 1.5
    alpha_mode     OPAQUE                # OPAQUE | MASK
    alpha_cutoff   0.5                   # MASK only
    double_sided   FALSE
}

# Channel extraction for packed metallic-roughness textures
channel_painter rough_p {
    source     mr_tex
    channel    G                          # R | G | B | A
    scale      1.0                        # multiplied with the extracted value
    bias       0.0                        # added after scale
}

# Tangent-space normal mapping
normal_map_modifier nm {
    normal_map    normal_painter
    strength      1.0
}

# glTF alphaMode = MASK
alpha_test_modifier am {
    alpha_painter alpha_p
    cutoff        0.5
}
```

**Files to touch (incremental on Phase 1):**

- New: `src/Library/Importers/GLTFSceneImporter.{h,cpp}` — orchestrates the full import (or do this as a static function in `Job.cpp`; new `Importers/` subdir is cleaner long-term).
- New: `src/Library/Materials/PBRMetallicRoughnessMaterial.{h,cpp}` — authoring helper class (constructs a `GGXMaterial` + emitter under the hood from PBR-shaped inputs).
- New: `src/Library/Painters/ChannelPainter.{h,cpp}`.
- New: `src/Library/Modifiers/NormalMapModifier.{h,cpp}`.
- New: `src/Library/Modifiers/AlphaTestModifier.{h,cpp}`.
- Extend: `Job.cpp` + `IJob.h` + `RISE_API.{h,cpp}` — `AddPBRMetallicRoughnessMaterial`, `AddChannelPainter`, `AddNormalMapModifier`, `AddAlphaTestModifier`, `ImportGLTFScene`.
- Extend: `AsciiSceneParser.cpp` — five new chunk parsers, registered in `CreateAllChunkParsers()`. Optional `quaternion` parameter on `StandardObjectAsciiChunkParser`.
- Five-project build wiring.
- Tests: `tests/GLTFImportTest.cpp` (full scene import — count objects, materials, lights), `tests/PBRMaterialTest.cpp` (verify the GGX construction matches the spec for a few canonical inputs).
- Scenes: `scenes/Tests/Materials/pbr_metallic_roughness.RISEscene`, `scenes/Tests/Importers/gltf_sponza.RISEscene`, `scenes/FeatureBased/gltf_showcase.RISEscene` for a curated render.

**Estimated effort:** 1 week.

---

### Phase 1.5 / Phase 2 cleanups (carried over from Phase 1)

| Item | Where it lives now | Phase 2 target | Phase 2 outcome |
|---|---|---|---|
| **Embedded-texture extraction for normal-map test** | `scenes/Tests/Geometry/assets/extract_embedded_texture.py` writes a sidecar PNG that the test scene `gltf_normal_mapped.RISEscene` references via `png_painter`. | Folded into `gltf_import` — when the importer encounters a glTF material with a `normalTexture` whose `source.image.bufferView` points into the .glb, it should extract that image to an in-memory `IRasterImage` (or auto-spilled tempfile) and create the painter without a sidecar.  The committed sidecar PNG and the helper script can be deleted at that point. | **Partially done** — `gltf_import` extracts embedded images to a sidecar `<asset-dir>/.gltf_cache/<hash>.<ext>` (with `/tmp/RISE_gltf_<hash>` fallback if the asset dir is read-only), gitignored.  The `extract_embedded_texture.py` helper is no longer needed for `gltf_import`-driven scenes, but stays in tree because the standalone `gltf_normal_mapped.RISEscene` test still uses the sidecar PNG it produces.  In-memory `IRasterImage` path remains Phase 3. |

### Phase 3+ — deferred work

| Feature | Estimate | Notes |
|---|---|---|
| LINEAR animation (TRS keyframes → Timeline) | 3 days | Map node TRS to existing keyframable parameters on `standard_object`. |
| STEP / CUBICSPLINE animation | 2 days | Bake CUBICSPLINE into LINEAR or add native support. |
| `alphaMode = BLEND` | 2 days | Stochastic transparency via `transparency_shaderop`. |
| Skinning | 2 weeks | CPU skin pass each frame + BVH refit via existing `UpdateVertices` hook. |
| Morph targets | 1 week | Same skinning machinery, different inputs. |
| `KHR_materials_clearcoat` | 3 days | Layered: clearcoat GGX over base GGX. RISE has `composite_material`. |
| `KHR_materials_transmission` / `volume` / `ior` | 1 week | Maps to dielectric + medium. Some support exists. |
| `KHR_materials_sheen` | 3 days | New BRDF lobe. |

---

## 8. Build & dependency wiring summary

**New extlib entry:** `extlib/cgltf/` — header + 1-line implementation TU.
Pattern matches libpng/zlib/libtiff/openexr (small vendored C). Update
`extlib/README.TXT` with license + source URL + version.

**No new transitive deps.** cgltf has none. JPEG support comes via the
spun-off `jpg_painter` task (stb_image vendor in the same `extlib/` style).

**Compile-time impact:** cgltf adds ~5k LOC of C99 to the library compile.
Negligible. No template-heavy C++ overhead like fastgltf would impose.

**Five-project add list per file (CLAUDE.md):** `Filelist`,
`rise_sources.cmake`, `Library.vcxproj` (compile + include), `.vcxproj.filters`,
`pbxproj` (4 sections × 2 targets).

---

## 9. Test plan

- **Phase 1 unit:** `tests/GLTFLoaderTest.cpp` loads a hand-crafted `box.glb`, verifies 8 unique positions, 12 triangles, normals match expected face normals, COLOR_0 round-trips through the v2 interface, TANGENT round-trips through the new v3 interface.
- **Phase 1 scene regression:** `gltf_box.RISEscene` renders a flat-shaded box at known positions; output PNG byte-compared against checked-in reference.
- **Phase 2 PBR oracle:** `tests/PBRMaterialTest.cpp` constructs a `pbr_metallic_roughness_material` with known base_color/metallic/roughness, samples the BRDF at known angles, compares against the glTF spec's reference equation. (Strong oracle per `docs/skills/write-highly-effective-tests.md`.)
- **Phase 2 import regression:** `gltf_sponza.RISEscene` (or smaller scene) round-trips through `gltf_import`, asserts created object/material/light counts.
- **Phase 2 visual regression:** `gltf_showcase.RISEscene` renders the Khronos sample DamagedHelmet (or similar) and compares to a checked-in reference image.

---

## 10. RISE enhancements summary

Net new chunks / interfaces / runtime hooks this work introduces.  "Phase"
records original-plan phase; "Status" records what has actually shipped.

| Item | Where | Phase | Status | Justification |
|---|---|---|---|---|
| `gltfmesh_geometry` chunk | `AsciiSceneParser.cpp` | 1 | **delivered db65457** | Mesh import surface |
| `Job::AddGLTFTriangleMeshGeometry` | `Job.{h,cpp}`, `IJob.h` | 1 | **delivered db65457** | Construction API |
| `RISE_API_CreateGLTFTriangleMeshLoader` | `RISE_API.{h,cpp}` | 1 | **delivered db65457** | Public C-style API |
| `TriangleMeshLoaderGLTF` class | `Geometry/` | 1 | **delivered db65457** | Loader implementation |
| `ITriangleMeshGeometryIndexed3` interface | `Interfaces/` | 1 | **delivered db65457** | Tangent + TEXCOORD_1 storage |
| ~~`tangent_painter` chunk~~ | — | 1 | **dropped — not needed** | Replaced by direct `RayIntersectionGeometric` fields (see "v3 intersection plumbing" below) |
| `RayIntersectionGeometric::vTangent / bitangentSign / bHasTangent` | `Intersection/` | 1 | **this branch** | Hit-time tangent surface for the normal-map modifier |
| Tangent interpolation in `TriangleMeshGeometryIndexed::IntersectRay` | `Geometry/` | 1 | **this branch** | Per-hit barycentric interpolation of v3 storage |
| Tangent transform in `Object::IntersectRay` | `Objects/` | 1 | **this branch** | Object→world transform via `m_mxFinalTrans` |
| `flip_v` default = TRUE on `gltfmesh_geometry` | `AsciiSceneParser.cpp` | 1 | **this branch** | V-convention reconciliation (see §4) |
| `normal_map_modifier` chunk + `NormalMap` class | `Modifiers/` | 2 → 1.5 | **this branch (early)** | Tangent-space normal maps |
| `Job::AddNormalMapModifier`, `RISE_API_CreateNormalMapModifier` | `Job/RISE_API` | 2 → 1.5 | **this branch (early)** | Construction API for the modifier |
| Embedded-texture extraction helper (`extract_embedded_texture.py`) | `scenes/Tests/Geometry/assets/` | 1.5 | **this branch (stopgap)** | One-shot script + sidecar PNG until `gltf_import` auto-extracts |
| `gltf_import` chunk | `AsciiSceneParser.cpp` | 2 | **this branch** | Full scene import surface |
| `pbr_metallic_roughness_material` chunk | `AsciiSceneParser.cpp` | 2 | **this branch** | PBR authoring sugar |
| ~~`PBRMetallicRoughnessMaterial` class~~ | — | 2 | **dropped — not needed** | Implemented as a `Job::AddPBRMetallicRoughnessMaterial` parser-side decomposition into GGX + painter graph; no new Material class required |
| `channel_painter` chunk + class | `Painters/` | 2 | **this branch** | Extract MR-texture channels |
| `GLTFSceneImporter` class + `ImportGLTFScene` API | `Importers/` | 2 | **this branch** | Orchestrates full scene import |
| `alpha_test_modifier` chunk + class | `Modifiers/` | 2 → 3 | **deferred to Phase 3** | RISE modifiers run post-hit-commit; alpha mask needs pre-commit hook |
| Optional `quaternion` / `matrix` param on `standard_object` | `AsciiSceneParser.cpp` | 2 → 3 | **deferred to Phase 3** | Empirical Euler XYZ adequate; revisit on gimbal-lock failure |
| Optional `emissive` param on `ggx_material` | `Materials/` + parser | 2 | **this branch** | Avoid double-chunk for PBR + emissive |
| Schlick-from-F0 mode in GGX BSDF | `Materials/GGXSPF` | — → 3 | **pending — Phase 3 (P1-1)** | Required to fix metals rendering 25× too dim under PBR mapping |

---

## 11. Open questions

1. **`KHR_materials_emissive_strength`** — multiplies emissive by a scalar.  Easy.  Honor in v1?
2. **`KHR_materials_unlit`** — bypass BSDF, render baseColor as luminaire.  Maps to `lambertian_luminaire_material`.  Honor in v1?
3. **Color space.** glTF spec says baseColor and emissive are sRGB; metallic / roughness / normal / occlusion are linear.  Our `png_painter` has a `color_space` param — confirm the auto-mapping logic when the importer creates painters.  Phase 1 finding for one slice of this: **normal maps need `color_space ROMMRGB_Linear`** (NOT `Rec709RGB_Linear`) to bypass the Rec709 → ROMM colour-matrix conversion that would otherwise warp the encoded vector.  Documented in `gltf_normal_mapped.RISEscene` and the `normal_map_modifier` chunk description.
4. **JPEG dependency timing.** ~~Phase 1 doesn't strictly need JPEG (we can test with PNG-textured glTF assets), but the curated showcase scene in Phase 2 will. Should the JPEG task be a hard dependency for Phase 2, or do we ship Phase 2 without JPEG and document it as "PNG-textured glTF only for now"?~~ **RESOLVED 2026-04-29** — `jpg_painter` shipped, so Phase 2 can use JPEG-textured assets from Khronos Sample-Assets directly without conversion.
5. **Naming convention for imported objects.** Proposal: `<prefix>.<glTFNodeName>.<primIdx>` for objects, `<prefix>.mat.<materialIdx>` for materials, `<prefix>.tex.<textureIdx>` for textures, `<prefix>.cam.<cameraIdx>` for cameras, `<prefix>.light.<lightIdx>` for lights.  Predictable enough that users can override individual items by re-declaring them after the `gltf_import` chunk.
6. **Where does the importer live?** Options:
   - As a static function on `Job` (simple, keeps `Job.cpp` from sprawling further).
   - As a new `src/Library/Importers/` subsystem (extensible — future FBX/USD importers slot in).

   Recommend the latter; Importers is a natural sibling to `Parsers/`.
7. **Should `standard_object` get a `matrix` parameter** (16 doubles) **as well as `quaternion`?** A matrix is even more general than quaternion+position+scale and would let the importer emit the node transform verbatim.  Cost: small — the parser converts to internal matrix form anyway.  Benefit: lossless for any glTF input including those with non-uniform / sheared / mirrored transforms.
8. **Validation strictness.** ~~Reject malformed glTF outright (cgltf's `cgltf_validate()`), or log warnings and try to render what we can?~~  **RESOLVED 2026-04-30 (Phase 1+2)** — both the per-mesh loader and the bulk importer call `cgltf_validate()` and bail on failure with an `eLog_Error` line naming the file and the cgltf result code; same treatment for `cgltf_parse_file` / `cgltf_load_buffers`.  Per-primitive failures inside `GLTFSceneImporter::Import` log a warning and skip the primitive rather than aborting the whole import.
9. **V-axis convention compensation lives at the loader.** ~~No question, just a finding — see §4 V-convention reconciliation.~~  **RESOLVED in Phase 1+2** — `gltf_import` applies the same `flip_v=TRUE` default per primitive when calling `Job::AddGLTFTriangleMeshGeometry`, so all texture-bearing assets render with consistent V orientation regardless of which import surface created the geometry.

---

## 12. Test assets

The canonical source is **Khronos glTF-Sample-Assets**
(https://github.com/KhronosGroup/glTF-Sample-Assets) — official, 100+ models,
organized by purpose, individual per-model licenses (almost all CC0 or CC-BY).
Each model ships in multiple subdir variants; grab `glTF-Binary/` for `.glb`
(single file, preferred for the test corpus) or `glTF/` for the
JSON + `.bin` + textures sidecar form.  The repo is ~1 GB checked out — better
to curl individual `.glb` files via raw GitHub URLs than to clone the whole
thing.

### Curated picks

**Phase 1 mesh-loader sanity checks** (small, well-known, simple but fully-featured):

- `Box` — minimal triangle mesh, no textures.
- `BoxTextured` — adds TEXCOORD_0 and a baseColor texture.
- `Duck` — the classic; small with a real PBR material.
- `Avocado` — small but uses the full PBR texture set (baseColor + MR + normal).

**Phase 2 PBR validation** (strong oracles per [docs/skills/write-highly-effective-tests.md](skills/write-highly-effective-tests.md)):

- `MetalRoughSpheres` — grid sweeping every (metallic, roughness) combination.
  Render it, then byte-check the diagonal: (metallic=1, roughness=0) → mirror
  reflection of baseColor; (metallic=0, roughness=1) → pure diffuse.  If the
  PBR mapping is wrong, this scene tells you *which* corner.
- `DamagedHelmet` — community-favorite PBR showcase; good visual sanity.
- `BoomBox` — high-quality PBR exercising every channel.

**Attribute / edge-case coverage** (intentionally adversarial — `Models/Testing/`):

- `NormalTangentTest` — tangent-space normal mapping; critical for verifying the
  new `ITriangleMeshGeometryIndexed3` tangent storage end-to-end.
- `NormalTangentMirrorTest` — mirrored UVs; exercises the w = ±1 bitangent-sign
  handling on TANGENT.  Easy to get wrong; this scene catches the bug
  immediately.
- `VertexColorTest` — COLOR_0 round-trip.
- `MultiUVTest` — TEXCOORD_1.
- `AlphaBlendModeTest` — alpha modes; v1 supports OPAQUE + MASK only, so this
  partially fails by design.  Useful regression for what we *do* support.
- `OrientationTest` — coordinate-system sanity; catches Y-up vs Z-up bugs fast.

**Bigger scenes for the Phase 2 showcase render:**

- `Sponza` (Intel reimport, in `Models/Sponza/`) — the canonical "is your
  renderer real" scene.
- `ABeautifulGame` — chess set, good PBR variety.

### Other sources (not needed for v1)

- **Poly Haven** (https://polyhaven.com/models) — CC0 photoreal assets, ships
  glTF and Blender source.
- **Khronos Sample Viewer** built-in test list — a curated subset of
  Sample-Assets.
- **Sketchfab** has a huge library but the license-filter UX is a chore.

### Where to put fetched assets in the repo

Test scenes need predictable asset paths.  Proposed layout:

- `scenes/Tests/Importers/assets/<ModelName>.glb` — single-file `.glb` for the
  small test corpus.  Commit them; they're each well under 1 MB.
- `scenes/FeatureBased/assets/<ModelName>/` — sidecar form for larger showcase
  models where individual texture inspection matters.  Showcase assets larger
  than ~10 MB should NOT be committed; instead, the `.RISEscene` file has a
  header comment with the fetch URL and SHA-256, and `scenes/README.md` lists
  the optional download.

## 13. Phase 2 adversarial review — what got fixed, what's deferred

A round of three orthogonal adversarial reviewers (correctness / API
robustness / fit-and-finish) hammered the Phase 2 work before this
document update.  Findings, in priority order, with what was fixed in
this branch and what was punted to Phase 3.

### P1 — correctness (highest priority)

**P1-1: Conductor Fresnel double-counts F0 (NOT FIXED — Phase 3).**
RISE's `GGXBRDF` evaluates Fresnel via
`Optics::CalculateConductorReflectance(ior, ext, cosθ)`.  At normal
incidence with the default `ior=1.5` / `ext=0`, that reflectance is
≈ 0.04 — exactly the dielectric F0.  The PBR mapping in
`AddPBRMetallicRoughnessMaterial` plugs `rs = lerp(0.04, baseColor,
metallic)` into the BRDF as the specular reflectance painter, so for
metals the rendered specular ends up `baseColor * F_conductor(1.5, 0)
≈ baseColor * 0.04` — i.e., **metals render with ~4 % of their intended
specular reflectance**, ~25× too dim.  The fix requires a Schlick-from-
F0 evaluation mode on `GGXBRDF` (multiply `1.0` by `F_schlick(F0,
cosθ)` instead of `F_conductor(ior, ext, cosθ)`); the PBR material would
opt into that mode while hand-authored `ggx_material` chunks keep the
conductor path.

This is an architectural correctness gap, not a bug in the import
pipeline — the importer hands the right F0 painter to `GGXMaterial`,
the BRDF then re-Fresnels it.  Fixing it touches `GGXSPF.cpp` and adds
a chunk parameter.  Phase 3 work.

**P1-2: Multiscatter lobe Fresnel-double-counts the same way (NOT FIXED — Phase 3).**
Same root cause as P1-1 — the multiscatter compensation path also
evaluates conductor Fresnel.  Will be fixed by the same Schlick-from-F0
mode.

**P1-3: PBR mapping ignores the (1−F) energy split between diffuse and specular (NOT FIXED — Phase 3).**
The glTF spec mathematically writes `diffuse = (1 − F(cosθ)) ·
baseColor·(1−metallic) / π`, so diffuse energy is dynamically modulated
by Fresnel.  The current mapping uses a static `c_diff = baseColor *
(1 − 0.04) * (1 − metallic)`, i.e. the (1−F) factor is collapsed to
(1−0.04) at normal incidence.  This is the standard "static-F0
approximation" used by every realtime renderer — visually subtle but a
spec-correctness gap on grazing angles.  Phase 3 fix is part of the
Schlick-from-F0 work.

**P1-4: glTF light colors not gamma-encoded (FIXED).**  glTF
KHR_lights_punctual specifies linear sRGB (linear Rec.709) for `color`,
but RISE's `Add*Light` methods treat the supplied triple as
already-encoded sRGB and gamma-decode it internally before lighting.
Without compensation, glTF lights came out roughly twice as dark as
authored.  Fixed in `GLTFSceneImporter::CreateLightForNode` by applying
the Rec.709 OETF (linear → sRGB) to `light->color` before passing to
`AddDirectionalLight` / `AddPointOmniLight` / `AddPointSpotLight`.

### P2 — API robustness / observable behavior

**P2-7: `Import()` docstring oversells atomicity (FIXED).**  The header
comment claimed materials would be cleaned up if the scene walk failed.
They aren't — the painter manager and material manager retain entries
created in the up-front pass.  Reworded to "NOT all-or-nothing" and
documented that callers needing atomic import should reset the Job
themselves.

**P2-9: Sidecar extraction fails on read-only asset directories (FIXED).**
`ExtractImageToSidecar` previously returned an empty path when the
asset directory was read-only (e.g., system-installed sample assets,
network-mounted scenes).  Now falls back to the OS temp dir
(`GetTempPath` on Windows, `$TMPDIR` / `/tmp` elsewhere) with the same
hashed filename when the sibling `.gltf_cache/` cannot be created.

**P2-1, P2-2, P2-3, P2-4, P2-5, P2-6, P2-8, P2-10:** Documented as
known limitations (multi-camera silent drop, lossy Euler decomposition,
no morph/skin warning when present, etc.).  Not worth a code change for
Phase 2 — every one is either covered elsewhere in this doc or
deliberately out of scope for v1 (see §6).

### P3 — fit and finish

**P3-2: `DecomposeAffine` claimed orthonormal rotation submatrix
(FIXED).**  The implementation makes no orthonormality check — sheared
or non-uniform-scaled inputs produce skewed Euler angles silently.
Reworded the comment to admit the assumption + describe the failure
mode + flag adding a check as Phase 3 work.

**P3-6: Unused `<set>` include (FIXED).**  Replaced with `<cstdlib>`
which is actually needed for `getenv` / `_dupenv_s`.

**P3-1, P3-3, P3-4, P3-5:** Style / micro-cleanups deferred — they
don't affect correctness or robustness and would add commit churn
without proportional value.

### Open Phase 3 work (BRDF correctness + alpha + transforms) — DELIVERED

| Item | Phase | Resolution |
|---|---|---|
| Schlick-from-F0 mode on `GGXBRDF` | 3 | **Delivered.**  New `fresnel_mode` param on `ggx_material` (default `conductor`); `pbr_metallic_roughness_material` flips it to `schlick_f0`.  Fixes P1-1 / P1-2 / P1-3.  Three new test programs cover it (see §14). |
| Direct (1−F) modulation of diffuse | 3 | **Delivered.**  Diffuse is multiplied by `(1 − max(F0))` per glTF spec inside the BRDF/SPF Schlick branch. |
| `alpha_test_modifier` for glTF MASK | 3 | **Delivered as `alpha_test_shaderop`** (renamed from the Phase 2 plan after review of `IModifier`'s lifecycle confirmed shader-ops are the right hook).  Caveat: works only under integrators that route through `IShader::Shade()` (PT + legacy direct shaders); BDPT, VCM, MLT, and photon tracers bypass and treat MASK as opaque.  Phase 4 candidate to promote into a hit-time concern. |
| `quaternion` / `matrix` parameter on `standard_object` | 3 | **Delivered.**  Both supported, with documented precedence (`matrix` > `quaternion` > `orientation`).  `Job::AddObjectMatrix` consumes a column-major 4×4 directly; the importer feeds the cgltf node-world matrix verbatim. |
| In-memory `IRasterImage` path for embedded textures | 3 | **Delivered.**  `Job::AddInMemoryPNGTexturePainter` / `AddInMemoryJPEGTexturePainter` accept byte buffers; importer feeds them with the cgltf bufferView directly.  `.gltf_cache/` sidecar is retired. |
| Orthonormality check in `DecomposeAffine` | 3 | **Resolved by deletion.**  After the matrix path landed, `DecomposeAffine` lost its only caller and was deleted from the importer; the gimbal-lock + shear failure mode it was paving over no longer exists. |
| **NEW: `mkFromQuaternion` bug** | 3 | Fixed at `Math3D/MatricesOps.h:215-217` — `_2y` and `_2z` were both computing `2 * a.v.x` instead of `a.v.y` / `a.v.z`.  Pre-existing latent bug surfaced when `standard_object { quaternion … }` exercised the function for the first time. |

### Phase 3 adversarial review — what got fixed, what's deferred

A second round of three orthogonal adversarial reviewers (BRDF
correctness / API robustness / glTF importer end-to-end) hammered
the Phase 3 work.  Findings:

- **P1-B (FIXED):** `GGXBRDF::albedo()` Schlick branch was returning
  the hemispherical Schlick average instead of `Schlick(F0, cosθo)`,
  inconsistent with the conductor branch's intent and the function's
  comment.  Now evaluates Schlick at the actual outgoing-cosine.
- **P2-B (FIXED):** `ResolveFresnelMode` warning could spam the log
  for scenes with many materials sharing the same typo.  Added a
  `static std::set` to dedup per-string.
- **P1-C / P2-A / P3 items:** documented or deferred — see the
  relevant code comments and the per-reviewer findings recorded in
  the Phase 3 commit notes.

### Out-of-scope follow-ups (Phase 4 candidates)

| Item | Why Phase 4 |
|---|---|
| Per-pixel alpha via alpha-aware painter (extract A channel from RGBA PNG) | The current alpha mask uses `max(R,G,B)` of baseColor as a proxy because `IPainter` only exposes RGB.  Adding an alpha-aware painter or an A-channel `IRasterImageAccessor` would close the gap for foliage textures whose transparent regions don't have low luminance. |
| Alpha mask under BDPT / VCM / MLT | Requires promoting alpha-test to a hit-time geometry concern; touches the intersector and every integrator's path-sampling loop.  Substantial refactor. |
| Animation / skinning / morph targets | Original Phase 3+ scope; unchanged. |
| `KHR_materials_*` extensions (clearcoat, transmission, sheen, …) | Original Phase 3+ scope; unchanged. |
| `alphaMode = BLEND` (stochastic transparency) | Phase 3 deliberately implemented MASK only; BLEND requires either path-tracer transparency_shaderop wiring or a different sampling strategy. |

---

## 14. References

- glTF 2.0 spec: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- glTF sample assets: https://github.com/KhronosGroup/glTF-Sample-Assets
- cgltf: https://github.com/jkuhlmann/cgltf
- tinygltf: https://github.com/syoyo/tinygltf
- fastgltf: https://github.com/spnda/fastgltf
- KHR_lights_punctual: https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual
- KHR_materials extensions index: https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos
