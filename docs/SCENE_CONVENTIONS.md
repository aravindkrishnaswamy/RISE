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

Source of truth: [src/Library/Lights/DirectionalLight.cpp:48](../src/Library/Lights/DirectionalLight.cpp):

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

## 2. Spot light `target` defines the cone axis (different convention)

```text
spot_light
{
    name     torch
    power    50
    color    1 1 1
    position Px Py Pz
    target   Fx Fy Fz   # any point along the cone-axis BEYOND the source
    inner    20         # inner cone HALF-angle, in DEGREES
    outer    45         # outer cone HALF-angle, in DEGREES
}
```

For spots, RISE wants a **target point**, not a direction (the
`spot_light` chunk parameters are `target`/`inner`/`outer` — see the
descriptor in
[src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)).
Internally it computes the cone-axis shine direction as
`target - position`; `inner`/`outer` are cone **half-angles in
degrees** (converted to radians at parse time; defaults 45/90).  This
is symmetric to how SpotLight tests `dot(toLight, -vDirection)` at hit
time.  See
[src/Library/Lights/SpotLight.cpp:37](../src/Library/Lights/SpotLight.cpp).

The glTF importer for spots passes `position + shine_direction` as the
target point — that's correct (no extra flip needed).

---

## 3. Light power semantics

`power` on each light type acts as a multiplier on `color`.  Roughly:

| Light type | Effective radiance at hit |
|---|---|
| `directional_light` | `color · power` (since rays are parallel; no distance falloff) |
| `omni_light` (point) | `color · power / r²` |
| `spot_light` | `color · power / r²` within the cone (cosine fall-off across inner→outer angles) |
| `ambient_light` | `color · power` (constant, all surfaces) — **do not use; see §3.5** |

`power = π` (often written `3.14`) is a common starting point for a
directional sun-like key on a Lambertian-white scene because the
Lambertian BRDF's `1/π` factor cancels with it: a fully-lit Lambertian
white surface returns `color = 1`.

An emissive material has no `power`; its `scale` multiplies the
`exitance` painter's colour instead (§3.5).  `rect_light` has no `power`
either — it takes `exitance`, which is that same per-unit-area quantity,
and the parser rejects a `power` line on it.

---

## 3.5. Which light kind to use — area lights are the norm

House convention (project owner, 2026-08-12).  This is a **convention**
for hand-authored scenes: the parser accepts every light chunk listed
below, and nothing in the scene loader rejects one.  The **agent
surface** is the one place it is enforced in code — see the end of this
section.

**Most scenes should be lit by AREA LIGHTS: an object wearing a
luminaire material, in most cases a rectangle.**

### The rectangle case: `rect_light` (one chunk)

Because a rectangular panel is the common case, it has a chunk of its
own (2026-08-12):

```text
rect_light
{
	name		window_light
	center		0 4 0
	size		2 1
	facing		0 -1 0
	color		1.0 0.95 0.85
	exitance	6000
}
```

- `center` is the panel's world-space centre; `size` is `width height`
  in scene units; `facing` is **the direction the panel emits toward**
  (a ceiling panel lighting the floor is `0 -1 0`).  It need not be unit
  length, but a zero vector fails the load.
- The panel emits toward `facing` **only** — its back face is not hit at
  all.  `facing` becomes the quad's geometric normal exactly.
- `exitance` is emitted radiance **per unit area** (it is the `scale` of
  the luminaire material below), so the same number on a panel twice the
  size delivers twice the light.  It must be > 0.  **There is no `power`
  parameter** — `power` on the zero-area lights is a different quantity,
  and writing one here fails the load rather than being ignored.
- `color` defaults to `1 1 1`, linear Rec.709.

`rect_light` is **parse-time sugar**: the parser expands it into exactly
the four chunks below, so the renderer treats it as an ordinary emitting
object.  Its `name` names the OBJECT; the helpers it also creates are
`<name>__pnt` (painter), `<name>__mat` (luminaire material) and
`<name>__geo` (quad).  A collision on any of the four fails the load
with the ordinary duplicate-name error.  The scene file keeps the
compact text — save serializes the CST document, so a `rect_light` saves
back as a `rect_light`.

### Any other shape: the four-chunk chain

There is no `area_light` chunk, and `rect_light` is a *rectangle*.  An
emitter of any other shape — a sphere lamp, a mesh fixture, a curved
panel — is written as the chain `rect_light` expands into, as used by
[scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene](../scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene):

```text
uniformcolor_painter
{
	name				pnt_window
	color				1.0 0.95 0.85
}
lambertian_luminaire_material
{
	name				window_mat
	exitance			pnt_window
	scale				6000.0
	material			none
}
clippedplane_geometry
{
	name				window_geo
	pta					-1.0 2.0 -1.0
	ptb					 1.0 2.0 -1.0
	ptc					 1.0 2.0  1.0
	ptd					-1.0 2.0  1.0
}
standard_object
{
	name				window_obj
	geometry			window_geo
	material			window_mat
}
```

- `material none` means the surface only emits (no underlying BRDF).
- `phong_luminaire_material` is the directional sibling (same
  `exitance` / `scale` / `material`, plus `N`).
- Any geometry works; a mesh wearing the material emits from every
  triangle.  `clippedplane_geometry` is a quad given by four corner
  points and is the usual choice for a window or a panel.
- **Sidedness is the corner winding plus `doublesided`.**  The quad's
  normal is `normalize(Cross(ptb - pta, ptd - pta))`, and a Lambertian
  luminaire emits only where `Dot(out, N) > 0`.  With `doublesided TRUE`
  (the default) a back-face hit flips the normal toward the ray, so the
  quad emits from BOTH faces; with `doublesided FALSE` the back face is
  not hit at all and only the winding's face emits.  `rect_light` sets
  `FALSE` and derives the winding from `facing`.
- **`scale` sets exitance — brightness per unit area** — so the same
  `scale` on a panel twice the size delivers twice the light.  The
  number is therefore scene-dependent, and the values in `scenes/` span
  four orders of magnitude: single digits and tens for interior fill
  panels, hundreds to tens of thousands for small bright emitters (the
  jewel-vault's 11 × 0.4 slot window, seen through a narrow gap, uses
  6000).
- An area light has real area, so it produces soft shadows and physical
  falloff, and it is visible in the frame wherever the camera can see
  it.  Point it away from the camera, or place it outside the view, if
  you do not want to see the emitter itself.

**`hosek_wilkie_skylight` is fine** — it is a physically based analytic
sun-and-sky model, not an anachronism.  It builds the scene's global
radiance map and (unless `create_sun false`) a matched
`directional_light` named `__hw_sun__`.

**`omni_light`, `spot_light` and `directional_light` are for special
cases only.**  They are zero-area idealizations: the light arrives from
a single point or from infinity, so their shadows have hard edges with
no penumbra at any distance, no material governs what they emit, and
nothing about them appears in the frame.  Reach for them when that is
what you actually want (a stand-in sun on a scene with no sky, a hard
key for a diagram, a cheap probe while iterating), not as the default
way to light a scene.

**`ambient_light`: never.**  It contributes the same `color · power` at
every shading point, scaled only by that surface's own reflectance; it
has no position and no direction (`name`, `power`, `color` are its only
parameters), and it casts no shadow ray
([RayCaster.h](../src/Library/Rendering/RayCaster.h) says so in the
shadow-routing contract: *"Ambient casts no shadow ray"*).  So it can
produce neither a shadow nor any falloff — it is a pre-GI flat-fill hack
([AmbientLight.h](../src/Library/Lights/AmbientLight.h)'s own header
comment says it exists for the ray tracer, which cannot do global
illumination).  In a path-traced scene, indirect light already does this
job physically; if a scene reads too dark, add or enlarge an emitting
surface, or add a sky, rather than a constant term.  The chunk still
parses — existing scenes that use it are not broken by this convention —
but do not author a new one.

**Where this is enforced in code:** the agent surface (`insert_chunk`,
`insert_chunks`, `light_scene`, the scaffold verbs that route through
them, and the `propose_patch` value-splice path) **refuses** to create
an `ambient_light` chunk, unconditionally — in every build phase and
with `--agent-build-protocol=off` — and its refusal states the physics
above and names `rect_light` first, then this four-chunk chain as the
general form.  Everything else in this section
is convention, not enforcement: the CLI, the GUI and the scene loader
will all happily author and load any light kind.

---

## 4. Color spaces in painters

Most painters take a `colorspace` parameter:

| `colorspace` value | Meaning | When to use |
|---|---|---|
| `sRGB` | sRGB-encoded display value; gamma-decoded on load | Hand-authored colors copied from a colour picker / RGB hex code (e.g. `#FF7F00`) |
| `Rec709RGB_Linear` | Already-linear Rec.709 RGB | Numerical weights / multipliers / measured spectra |
| `ROMMRGB_Linear` | Linear ROMM (ProPhoto primaries); applies a REAL Rec.709 → ROMM conversion on load (the working space has been Rec.709 since the 2026-05 Stage-B migration) | Only when you've explicitly authored ROMM-encoded data.  NEVER for normal maps — the conversion warps the vectors; the verbatim-store idiom is `Rec709RGB_Linear` |
| `ProPhotoRGB` | ROMM with the ProPhoto display-encoding curve | Rare; only when you've explicitly authored ProPhoto-encoded data |

`uniformcolor_painter` defaults to `Rec709RGB_Linear` (its `color` is treated
as already-linear Rec.709 — i.e. a numerical weight, no gamma decode).  Pass an
explicit `colorspace` (e.g. `sRGB`) when the value was picked perceptually and
should be gamma-decoded.  Painters that load image data (`png_painter`,
`jpg_painter`, `exr_painter`, `hdr_painter`) default to sRGB for PNG/JPG and
linear for EXR/HDR.

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

## 8.5. The `film` chunk — pixel-grid output settings

Output settings (image width, height, pixel aspect ratio) live on a
scene-level `film` chunk, not on the camera.  The camera handles
imaging optics (FOV, aperture, focal length); the `film` describes
the discretization of the continuous image to pixels.  Background:
[docs/ARCHITECTURE.md](ARCHITECTURE.md) "Camera / Film / Output
Separation".

```
film {
    width 1920
    height 1080
    pixelAR 1.0
}
```

- `width` (UInt, default 960) — image width in pixels.
- `height` (UInt, default 540) — image height in pixels.
- `pixelAR` (Double, default 1.0) — pixel aspect ratio. 1.0 = square
  pixels (the common case). Use ≠ 1.0 for anamorphic / NTSC-style
  non-square pixel grids.

If no `film` chunk is present, the default is **qHD = 960 × 540 with
square pixels** — sized for fast iteration on test scenes. Override
at the command line with `RISE-CLI --width N --height N --pixel-ar X`.

**Camera chunks no longer accept `width`/`height`/`pixelAR`.** Scene
format v6 (Phase B2 of the Camera/Film/Output split, 2026-05) moved
those into the dedicated `film` chunk above.  Cameras read dims from
the active Film at construction.  v5 scenes that authored
`width`/`height` inside camera chunks must be migrated:

```sh
python tools/migrate_scenes_v5_to_v7.py [path-or-dir]
```

The script is idempotent — running it on a v6 scene is a no-op.
Loading a v5 scene through the parser without migration produces a
clear error pointing at this command.

**Multiple `film` chunks: last-declared wins.** Each `film` chunk
calls `Scene::ResizeFilm` which mutates the active Film in place AND
resyncs every previously-added camera's projection matrix.  So you
can author multiple `film` chunks (rare; useful for scene-time
overrides) and the final state is always: Film = the most recent
chunk, every camera's projection matches Film.

**Multi-camera with different per-camera dims is not preserved.**
Authoring two cameras at different resolutions and switching between
them with `SetActiveCamera` won't restore the per-camera dims —
every camera resyncs to whatever Film is. If you genuinely want
multiple cameras at different resolutions, you need separate render
invocations (e.g., `RISE-CLI --width A scene.RISEscene` then
`RISE-CLI --width B scene.RISEscene`).

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

## 10. HDR output pipeline

Added by Landing 1 of the
[PB pipeline plan](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md).
The recommended pattern for any new scene is to declare **two**
`file_rasterizeroutput` chunks: an HDR primary and a tone-mapped
display preview.

**Declaration order matters:** a `file_rasterizeroutput` chunk
attaches to the rasterizer that is active when it is parsed, so the
rasterizer chunk must appear **before** it (same convention as
`camera_defaults` before cameras).  A scene that declares
`file_rasterizeroutput` first — or omits the rasterizer chunk
entirely — fails to load with a "no rasterizer is set" diagnostic.

```
# HDR primary — the integrator's verbatim radiometric output.
# No exposure, no display transform.  Use as the source of truth
# for variance / RMSE comparisons and external compositing.
# (`Rec709RGB_Linear` is the identity/verbatim output since the
# Stage-B migration; `ROMMRGB_Linear` here would apply a real
# Rec.709 → ROMM conversion at the write boundary — use it only
# when you specifically want a ROMM-encoded EXR.)
file_rasterizeroutput
{
    pattern             rendered/myscene
    type                EXR
    color_space         Rec709RGB_Linear
    exr_compression     piz
}

# PNG display preview — derived view of the EXR.  Exposure 0
# means the EXR's nominal radiance values pass through unscaled;
# the ACES tone curve provides proper highlight rolloff for an
# LDR display.
file_rasterizeroutput
{
    pattern             rendered/myscene
    type                PNG
    bpp                 8
    color_space         sRGB
    exposure            0.0
    display_transform   aces
}
```

Both outputs share the same render — they're alternative views of
the same `IRasterImage`.  Both files coexist:
`rendered/myscene.exr` and `rendered/myscene.png`.

### Display transform options

| Curve | When to pick |
|---|---|
| `none` | Default for **HDR** types (EXR / HDR / RGBEA) — they archive linear radiance verbatim.  Also use on LDR for byte-exact regression testing or when downstream tooling expects raw linear. |
| `reinhard` | Cheapest; asymptotic to 1; good baseline; can look flat. |
| `aces` | Default for **LDR** types (PNG / TGA / PPM / TIFF).  Krzysztof Narkowicz fit to ACES RRT+ODT for sRGB.  Industry-standard PBR look. |
| `agx` | Modern alternative.  v1 ships a scalar sigmoid placeholder; the proper primaries-aware AgX lands with the spectral pipeline (Landing 3). |
| `hable` | John Hable's Uncharted 2 filmic.  Older; for compatibility with assets tuned against it. |

The default depends on the format: HDR types default to `none` (so they remain
the radiometric ground truth), LDR types default to `aces`.  Setting
`display_transform` on an HDR format triggers a warning and is ignored.

### Exposure semantics

`exposure` is in EV stops.  `+1` doubles brightness, `-1` halves.
Computed as `radiance × 2^EV` before the tone curve.  Lets you
over- / under-expose a render without re-integrating; the EXR
primary captures the un-exposed radiance so any number of
preview PNGs can be derived without re-rendering.

### HDR formats ignore exposure / display_transform

`type = EXR / HDR / RGBEA` are radiometric ground truth — applying
a tone curve to them would corrupt the recorded radiance.  Setting
`exposure` or `display_transform` non-default on an HDR output
fires a warning and is ignored.

### Migration from pre-Landing-1 scenes

Existing `file_rasterizeroutput { type PNG; ... }` chunks still
parse, but the **default `display_transform` for LDR types is now
`aces`**, not the previous implicit `none`.  PNG (and other LDR)
renders will look subtly different — highlights roll off rather
than clip.  HDR types (EXR / HDR / RGBEA) default to `none` and
their behaviour is byte-identical to before.

Scenes that require the legacy clip-at-1.0 behaviour for an LDR
output (e.g., byte-exact regression testing) must opt in
explicitly:

```
file_rasterizeroutput
{
    pattern             ...
    type                PNG
    color_space         sRGB
    display_transform   none      # opt back in to legacy clipping
}
```

### External tools and chromaticities

Our EXR output writes the `chromaticities` attribute matching the
selected `color_space` (Rec.709 primaries for `Rec709RGB_Linear`,
ROMM RGB primaries when `color_space = ROMMRGB_Linear`).  Colour-managed viewers that
honour the tag (tev, mrViewer, Nuke + OCIO) display correct hues.
Viewers that ignore it (Photoshop's EXR support) show slightly
desaturated values — the data is recoverable via an explicit
primaries conversion in oiiotool / Resolve / etc.

### Build dependency

OpenEXR is now a build requirement.  Windows pulls it via vcpkg
(automatic when building the VS2022 solution); macOS auto-detects
Homebrew's `openexr` package; Linux requires
`libopenexr-dev` (Debian/Ubuntu) or `OpenEXR-devel` (Fedora/RHEL).
Override `OPENEXR_PREFIX` / `IMATH_PREFIX` in
`build/make/rise/Config.Linux` for non-standard install paths.

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
- [docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md):
  Landing 1 detailed design for the HDR output pipeline.
