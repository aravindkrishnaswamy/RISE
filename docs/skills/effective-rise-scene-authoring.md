---
name: effective-rise-scene-authoring
description: |
  Procedure for authoring RISE scenes that render correctly the first time
  (or for diagnosing one that doesn't).  Use when: writing a new
  `.RISEscene` from scratch, adapting an existing scene to a new asset,
  porting a scene description from another tool (Blender / glTF / Maya),
  or chasing a "this scene renders unexpectedly dark / wrong-coloured / 
  oriented backwards" report.  Walks through the convention checklist
  ([docs/SCENE_CONVENTIONS.md](../SCENE_CONVENTIONS.md)), the "Lambertian
  control sphere" diagnostic, and the bisection workflow for broken
  multi-chunk scenes.
---

# Effective RISE Scene Authoring

## When To Use

- Writing a new `.RISEscene` test or showcase from scratch.
- Adapting / cloning an existing scene to a new geometry, light setup,
  or material palette.
- Porting a scene description from a foreign tool (Blender, Maya,
  glTF asset, Unity / Unreal lighting setup).
- Diagnosing a scene that renders unexpectedly: too dark, wrong
  orientation, washed-out colours, missing geometry, etc.
- Reviewing a scene-file PR.

## When NOT To Use

- The scene is rendering "correctly but slowly" — that's a
  performance / SPP issue.  Use
  [performance-work-with-baselines](performance-work-with-baselines.md)
  or [variance-measurement](variance-measurement.md).
- The scene shows fireflies — those are a sampler / integrator concern.
  Use [sms-firefly-diagnosis](sms-firefly-diagnosis.md) for SMS-specific
  fireflies, or the BDPT/VCM MIS skill for those.
- The scene works but you want to add a new shader feature — that's a
  new-chunk-parser task.  Read
  [src/Library/Parsers/README.md](../../src/Library/Parsers/README.md)
  first.

## Procedure

### 1. Start from a known-good scene, not from scratch

The single biggest predictor of "this scene renders correctly the
first time" is whether you started from a working reference.  Find one
in `scenes/Tests/` whose **rasterizer + camera + lighting style** matches
what you want (e.g. `pixelpel_rasterizer` + `pinhole_camera` + a
directional key + ambient is the most common combo) and adapt it.
Don't start with a blank file unless you're authoring something
genuinely novel.

Good starting points:

- `scenes/Tests/Cameras/realistic.RISEscene` — pinhole + key + Lambertian
  textured cube.  Light direction `(0, 0, 1)`.
- `scenes/Tests/Materials/pbr_metallic_roughness.RISEscene` — PBR
  spheres with `pbr_metallic_roughness_material`.
- `scenes/Tests/Importers/gltf_import_avocado.RISEscene` — glTF bulk
  import + key/ambient lighting.

### 2. Walk the convention checklist before rendering

Open [docs/SCENE_CONVENTIONS.md](../SCENE_CONVENTIONS.md) and audit
each section against your scene:

1. **§1 Directional light direction.**  `direction` points FROM
   surface TO light source.  For a camera at `+Z` looking at the
   origin, your `direction` vector should have a **positive Z**
   component to light camera-facing surfaces.  If you've copy-pasted
   from a glTF / Unity / Unreal tutorial, **the sign is probably wrong** —
   they use the "shine direction" convention (opposite).
2. **§3 Light power.**  `power = π` (`3.14`) is the right starting
   point for a directional sun on a Lambertian-white scene.  Bump up
   for darker materials, but don't go below 1.0 unless you're
   deliberately dimming.
3. **§4 Colour spaces.**  `uniformcolor_painter` defaults to sRGB.  If
   you typed a perceptual hex code, use `colorspace sRGB`.  If you
   typed a numerical weight (`0.5` to mean "half"), use
   `Rec709RGB_Linear`.  Normal maps need `ROMMRGB_Linear`.
4. **§5 standard_object transforms.**  Don't combine `matrix`,
   `quaternion`, and `orientation` — pick one.  glTF imports always
   use `matrix`.  Hand-authored scenes usually use Euler `orientation`.
5. **§6 Coordinate system.**  Y-up, right-handed.  Camera at `+Z`
   looking at `-Z`.

### 3. Render a Lambertian-white control sphere first

Before rendering your full asset, swap it out for a `sphere_geometry`
+ `lambertian_material` with white reflectance.  If the sphere is
fully bright, your lighting is correct and the asset itself is
behaving as authored.  If the sphere is dim, **fix the lighting
before touching the asset's material**.

Boilerplate:

```text
uniformcolor_painter
{
    name        ctrl_white
    color       0.8 0.8 0.8
    colorspace  sRGB
}

lambertian_material
{
    name        ctrl_diffuse
    reflectance ctrl_white
}

sphere_geometry { name ctrl_sph radius 0.6 }

standard_object
{
    name     ctrl
    geometry ctrl_sph
    material ctrl_diffuse
    position 0 0 0
}
```

A dark control sphere with `power 3.14` directional + `power 0.25`
ambient indicates a wrong-direction directional (most common — see
§1).  A control sphere that's bright but tinted indicates a colour
space bug (§4).

### 4. Render at low SPP first

Set `samples 4` on the rasterizer.  Render.  If the qualitative shape
is wrong (orientation, lights, materials), more samples won't fix it.
Iterate at low SPP, then crank to 64+ once the scene is qualitatively
right.

### 5. Read `RISE_Log.txt` after every render

Many scene-config errors are caught at parse / setup time and surface
only as log warnings, never in the rendered image.  Examples:

- "Material X not found" — probably a typo or a chunk-order bug.
- "Painter `Y` already registered" — duplicate name.
- "Geometry `Z` not found, object `obj` skipped" — the object
  silently disappears from the render.
- "Unknown fresnel_mode" — falls back to conductor.

Always grep `RISE_Log.txt` after a "wrong" render before opening the
integrator code.

### 6. Bisect a broken multi-chunk scene

When a scene with many chunks renders incorrectly and the cause isn't
obvious, bisect by reduction.  Comment out half the geometry / lights
/ materials / objects with `#`, render again, and observe.  Continue
halving until you've isolated the problem chunk.  This is more
reliable than trying to reason about chunk interactions a priori.

For glTF assets imported via `gltf_import`, comment out the entire
`gltf_import { … }` block and replace it temporarily with a plain
`sphere_geometry` + `lambertian_material` + `standard_object` to
confirm the lighting works in isolation before trusting the importer.

### 7. SMS-specific: pick `sms_seeding` + `sms_target_bounces` from regime

If the scene has SMS enabled (`sms_enabled TRUE`), three SMS knobs
need scene-class-aware values, not defaults.  See
[CLAUDE.md] §"SMS seeding mode" + [docs/SMS_PHOTON_REGIME.md] for the
full empirical writeup; quick decision tree:

1. **`sms_seeding`**:
   - `"snell"` (default) for displaced refractive caustics —
     deterministic Snell-trace from shading toward light.  Best on
     interior lights through dielectric shells (Veach egg style).
   - `"uniform"` for smooth analytic primitives with reflection /
     glint caustics (smooth glass spheres, smooth mirrors).  Mitsuba-
     faithful uniform-area sampling on the caster.
   - The two are regime-complementary, not one-better — picking the
     wrong one can be a 2-3× quality gap.

2. **`sms_target_bounces`**: the natural specular-vertex count for
   the scene's caustic.  Default 0 = no target.  Recommended values:
   - `2` for glass shells, interior-light scenes, k=2 dielectric
     caustics (Veach egg, sms_k2_glassblock, sms_k2_glasssphere).
   - `1` for k=1 single-mirror caustics (diacaustic-class scenes,
     sms_k1_botonly).
   - Required (not optional) when `sms_seeding "uniform"` — without
     it, uniform mode produces variable-length chains and a 36-53 %
     phys-fail rate vs. snell's 3-17 %.  See commit `a3790c4` and
     [docs/SMS_SEEDING_OVERTRACE_FIX.md] §"Uniform-mode follow-up".

3. **`sms_photon_count`**: default `0` = off.  Enable (`100000` is a
   reasonable starting value) ONLY when the no-photon ok rate is
   below ~10 % — i.e. snell-trace is systematically missing
   caustic basins.  Diagnostic decision tree in
   [docs/SMS_PHOTON_REGIME.md]:
   - **Diacaustic-class** (multi-basin curved mirror): photons help
     both modes, uniform sees the bigger gain.  `sms_photon_count
     100000` + `sms_seeding "uniform"` is the canonical combo.
   - **`sms_slab_close_sms`-class** (snell-trace blocked or
     occluded): photons are essential — without them ratio is 0.02;
     with them ratio is 3.6+.
   - **Displaced-egg-class** (single-basin interior-light): photons
     don't help, just cost ~6× more wall-time.  Don't enable.

### 8. Save the working scene to `scenes/Tests/` or `scenes/FeatureBased/`

Once the scene renders correctly, add a header comment that documents:

- What it tests / showcases.
- Asset attribution if importing from external sources (URL, license).
- Known-good camera / light parameters and why those values were
  chosen.
- The `pattern` line should write to `rendered/<scene_name>` to keep
  outputs consistent.

Place the file per [scenes/README.md](../../scenes/README.md): focused
regression scenes go in `Tests/`, multi-feature showcases in
`FeatureBased/`.

## Anti-Patterns

- **Copy-pasting a `direction` line from a glTF tutorial / Unity guide
  / Unreal blueprint without re-checking the convention.**  The most
  common cause of dim renders.  See [SCENE_CONVENTIONS.md §1](../SCENE_CONVENTIONS.md#1-directional-light-direction-is-from-surface-to-light).
- **Cranking `power` to 100 to "fix" a dim scene without understanding
  why it's dim.**  Almost always masks a wrong direction or wrong
  colour space; the scene becomes correct-on-the-surface but the
  pixel values blow out HDR range.
- **Using `samples 64` for the first render** of a new scene.  Iterate
  at `samples 4` until qualitatively correct.
- **Authoring a metallic surface (e.g., `metallic = 1` PBR) without an
  environment map and expecting it to look bright.**  Metals reflect
  only what's in the specular cone; a bare metal under a single
  directional looks almost black off the mirror reflection.  Add an
  environment radiance map (`environment_radiance_map` / `radiance_map`
  on the object) or accept the limitation.
- **Mixing `quaternion` and `orientation` on the same `standard_object`.**
  The parser warns and picks one; the un-picked one is silently lost.
- **Not reading `RISE_Log.txt`.**  Half of "wrong render" reports are
  diagnosed in the log and never reach the pixel buffer.

## Concrete Example

Phase 3 of the glTF import work added four test scenes
(`gltf_import_avocado`, `gltf_import_damaged_helmet`,
`gltf_import_metalrough_spheres`, and `pbr_metallic_roughness`).
The scenes were authored with `direction -0.4 -0.4 -0.7` based on a
mental model of "negative Z = light shines toward -Z, where the asset
is".  Rendered scenes came out very dark.

**Misdiagnosis trail (skipped this skill):**
1. Suspected metallic-PBR-without-IBL (partially true for
   MetalRoughSpheres' metals — but the dielectrics and Lambertian-like
   surfaces were also dark, which the IBL hypothesis doesn't explain).
2. Suspected the Phase 3 Schlick-from-F0 BRDF rewrite — re-read the
   math, found it correct.

**Correct diagnosis (using this skill):**
1. **Step 3 — render a Lambertian-white control sphere with the same
   lights.**  It came out dark gray → lighting is wrong, not material.
2. **Step 2 + §1 — checked `direction` convention.**  Read
   `DirectionalLight.cpp:48`: `Dot(direction, normal) > 0` ⇒ `direction`
   is FROM surface TO light.  Our `(-0.4, -0.4, -0.7)` placed the
   light *behind* the camera; camera-facing surfaces had
   `dot < 0` → unlit.
3. **Fix:** invert all four scenes' direction signs.  Also discovered
   the same convention bug inside the glTF importer's
   `CreateLightForNode` — the cgltf-derived shine direction was being
   passed through as-is and needed a sign flip too.

The control-sphere diagnostic in step 1 took two minutes; the
misdiagnosis trail before that was ~30 minutes of re-reading BRDF math.
The point of this skill is to do step 1 first, every time.

## Stop Rule

The skill's work is done when:

- Your control-sphere render is bright in the expected places.
- The full asset render qualitatively matches your authoring intent at
  `samples 4`.
- `RISE_Log.txt` has no error/warning lines from this scene.
- You've added a header comment that future you (or a reviewer)
  can read to understand the scene's intent.

If any of those four are missing, you're not done.
