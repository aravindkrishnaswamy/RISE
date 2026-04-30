# RISE Scene-Authoring Conventions

This doc captures the scene-file conventions that have caused recurring
"why does my scene look wrong?" bugs.  When a scene renders unexpectedly
(too dark, geometry oriented wrong, reflectance off, etc.) before
opening the integrator code, walk this list.  When authoring a new
scene, lean on the **anti-patterns** at the bottom of each section
to avoid the standard traps.

The companion skill is
[docs/skills/effective-rise-scene-authoring.md](skills/effective-rise-scene-authoring.md);
this doc is the reference, the skill is the procedure.

---

## 1. Directional light `direction` is FROM-surface-TO-light

This has bitten us at least twice; capturing it loudly so it stops.

```text
directional_light
{
    name        key
    power       3.14
    color       1 1 1
    direction   X Y Z      # vector pointing FROM any surface TO the light source
}
```

Source of truth: [src/Library/Lights/DirectionalLight.cpp:48](../src/Library/Lights/DirectionalLight.cpp:48):

```cpp
Scalar fDot = Vector3Ops::Dot( vDirection, ri.vNormal );
if( fDot <= 0.0 ) {
    return;        // surface is in shadow, nothing reflected from this light
}
```

A surface is lit when `N · direction > 0`.  In other words, `direction`
points **toward** the light source from the geometry's perspective.
This is the OpenGL / glTF *to-light* convention, **not** the
*shine-direction* convention some other engines use.

### Worked example

Camera at `(0, 0, +5)` looking at the origin.  A typical asset (sphere,
helmet, …) at the origin has camera-facing surface normals close to
`(0, 0, +1)`.  To light the visible side of the asset:

| Light placement (mental model) | `direction` value | `N · dir` for camera-facing N=(0,0,1) | Lit? |
|---|---|---|---|
| Light source above-front-right of camera (where you'd put a key) | `0.4 0.4 0.7` | +0.7 | yes |
| Light source dead in front of camera | `0 0 1` | +1.0 | yes (matches `scenes/Tests/Cameras/realistic.RISEscene`) |
| Light source above-back-left of asset | `-0.4 -0.4 -0.7` | -0.7 | NO — asset front renders dark |

If a Lambertian-white sphere with `power 3.14` and ambient 0.25 comes
out dark gray, the direction is wrong.

### Cross-check: the importer must do the same flip

`KHR_lights_punctual` directional lights shine down their local `-Z`
axis.  After applying the node's world transform, you have a
*shine direction* — exactly opposite of what RISE wants.  The glTF
importer at
[src/Library/Importers/GLTFSceneImporter.cpp](../src/Library/Importers/GLTFSceneImporter.cpp)
in `CreateLightForNode` negates the shine direction before handing
it to `Job::AddDirectionalLight`.  **Always do that conversion when
importing from a foreign format.**

### Anti-patterns

- Copy-pasting `direction X Y Z` from another graphics tool's scene
  description without checking which convention that tool uses.
- Using a "negative-Z so it shines toward -Z" mental model from
  rasterizers (e.g. Unity / Unreal directional lights) — RISE is the
  opposite.
- "Direction toward asset" — also wrong; it's "direction FROM asset".

---

## 2. Spot light `focus_point` defines the cone axis (different convention)

```text
spot_light
{
    name        torch
    power       50
    color       1 1 1
    position    Px Py Pz
    focus_point Fx Fy Fz   # any point along the cone-axis BEYOND the source
    inner_angle 0.2
    outer_angle 0.4
}
```

For spots, RISE wants a **focus point**, not a direction.  Internally
it computes `vDirection = focus_point - position` (the shine direction
for the cone axis).  This is symmetric to how SpotLight tests `dot(toLight, -vDirection)` at hit time.  See
[src/Library/Lights/SpotLight.cpp:37](../src/Library/Lights/SpotLight.cpp:37).

The glTF importer for spots passes `position + shine_direction` as the
focus point — that's correct (no extra flip needed).

---

## 3. Light power semantics

`power` on each light type acts as a multiplier on `color`.  Roughly:

| Light type | Effective radiance at hit |
|---|---|
| `directional_light` | `color · power` (since rays are parallel; no distance falloff) |
| `omni_light` (point) | `color · power / r²` |
| `spot_light` | `color · power / r²` within the cone (cosine fall-off across inner→outer angles) |
| `ambient_light` | `color · power` (constant, all surfaces) |

`power = π` (often written `3.14`) is a common starting point for a
directional sun-like key on a Lambertian-white scene because the
Lambertian BRDF's `1/π` factor cancels with it: a fully-lit Lambertian
white surface returns `color = 1`.

---

## 4. Color spaces in painters

Most painters take a `colorspace` parameter:

| `colorspace` value | Meaning | When to use |
|---|---|---|
| `sRGB` | sRGB-encoded display value; gamma-decoded on load | Hand-authored colors copied from a colour picker / RGB hex code (e.g. `#FF7F00`) |
| `Rec709RGB_Linear` | Already-linear Rec.709 RGB | Numerical weights / multipliers / measured spectra |
| `ROMMRGB_Linear` | RISE's internal working space | Bypass colour conversion entirely (e.g. tangent-space normal maps where the "RGB" is a vector, not a colour) |
| `ProPhotoRGB` | ROMM with the ProPhoto display-encoding curve | Rare; only when you've explicitly authored ProPhoto-encoded data |

`uniformcolor_painter` defaults to sRGB.  Painters that load image data
(`png_painter`, `jpg_painter`, `exr_painter`, `hdr_painter`) default to
sRGB for PNG/JPG and linear for EXR/HDR.

### Anti-patterns

- Using `colorspace sRGB` for normal maps — gamma-decoding warps the
  encoded vector.  See `gltf_normal_mapped.RISEscene` for the right
  recipe.
- Using `Rec709RGB_Linear` for hand-picked perceptual colors — the
  painted result will look ~2.2× brighter than a colour picker
  suggested.
- Applying gamma twice: pass `1.0 1.0 1.0` to a `uniformcolor_painter`
  with `colorspace sRGB`, then pass the painter through another
  sRGB-decoding stage.  Match the colour space to the data's encoding.

---

## 5. `standard_object` transform precedence (Phase 3+)

```text
standard_object
{
    name        my_mesh
    geometry    geom
    material    mat
    position    Px Py Pz
    orientation Rx Ry Rz       # Euler XYZ degrees (legacy)
    quaternion  Qx Qy Qz Qw    # glTF/OpenGL convention (xyzw); takes precedence over orientation
    matrix      m00 m01 ... m33  # 16 doubles, column-major; takes precedence over both above
    scale       Sx Sy Sz
}
```

**Order of precedence** (highest first):
1. `matrix` — full 4×4 supersedes everything; emits warning if combined with
   `quaternion`, `orientation`, or `position`.
2. `quaternion` — replaces Euler rotation; still composes with `position`
   and `scale`.  Emits warning if combined with `orientation`.
3. `orientation` — Euler XYZ degrees (RX·RY·RZ, lossy at gimbal-lock).

The glTF importer always uses the `matrix` path for losslessness.  Most
hand-authored scenes use the Euler form for simplicity.

`scale` is per-axis (`Vector3`, not scalar).

---

## 6. Coordinate system

RISE uses a right-handed coordinate system.  Common authored conventions:

- `up = (0, 1, 0)` (Y-up) for cameras and lights — matches glTF, OpenGL.
- Asset Z-axis points "out of the screen" toward the viewer.
- A camera at `(0, 0, +5)` looks at `(0, 0, 0)` along the `-Z` axis.

When you import an asset whose source convention is Z-up (3DS Max, Blender
defaults), you typically need a `90°` X rotation on the importing
`standard_object`.  The glTF importer assumes Y-up per the glTF spec
and does not insert this rotation; check `OrientationTest.glb` if in
doubt.

---

## 7. Texture V-axis flip

glTF textures use OpenGL V-up (V=0 at the bottom of the image).  RISE
samplers use DirectX V-down (V=0 at the top).  The
`gltfmesh_geometry` chunk has `flip_v TRUE` by default to compensate
at mesh-load time — this is correct for almost every glTF asset.
Native PLY / 3DS / RAW2 files don't flip.

If a texture maps "upside down" on a glTF asset, check that `flip_v` is
TRUE; if it maps wrong on a non-glTF asset, the source asset itself was
authored against the opposite convention.

---

## 8. Render-rasterizer pairing

A scene needs exactly one rasterizer.  The shader-op pipeline behaves
differently across rasterizers; some features (notably
`alpha_test_shaderop`) only work under integrators that go through
`IShader::Shade()`:

| Rasterizer | Honours shader-ops? | glTF MASK alpha? |
|---|---|---|
| `pixelpel_rasterizer` (PT) | yes | yes |
| `bdpt_pel_rasterizer` (BDPT) | no — bypasses for path construction | no, surface treated as opaque |
| `vcm_*_rasterizer` (VCM) | no | no |
| `mlt_*_rasterizer` (MLT) | no | no |
| Photon tracers | no | no |

If a scene relies on alpha cutout (foliage, decals) or any
shader-op-driven effect, render with PT.

---

## 9. Sanity-check workflow

When a new scene renders unexpectedly:

1. **Render a Lambertian-white sphere with the same lights and camera.**
   If the sphere is dark, the lighting is wrong — investigate
   `direction` first (§1), then `power` (§3), then color space (§4).
2. **Check `RISE_Log.txt`** for parser warnings — many config errors
   are diagnosed there but never surface in the rendered image (e.g.
   "Material X not found, falling back to default").
3. **Render with `samples 4` first** to iterate quickly; bump to 64+
   only after the scene is qualitatively correct.
4. **Bisect by removing chunks** if a multi-feature scene is broken —
   start from a known-good scene and add one chunk at a time.

---

## See also

- [scenes/README.md](../scenes/README.md): where to put scenes
  (`Tests/` vs `FeatureBased/`).
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md):
  how the chunk parser works, how to add a new chunk.
- [docs/skills/effective-rise-scene-authoring.md](skills/effective-rise-scene-authoring.md):
  procedure for authoring scenes that render right the first time.
- [docs/GLTF_IMPORT.md](GLTF_IMPORT.md): glTF-specific conventions and
  the convention-conversion sites in the importer.
