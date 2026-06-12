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
- `scenes/Tests/Geometry/sweep_instances.RISEscene` — the procedural
  chunks (`sweep_geometry`, `path_instances_geometry`) on four reuse
  cases: tube on a 3D S-curve, tapered horn, beads on an arc, twisted
  ribbon.  Working omni-light powers for a ~30-unit camera distance.
- `scenes/Tests/SDF/` — `sdf_geometry` inline `part` authoring
  (shadows / volume / caustic / luminaire-in-fog variants).
- `scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene` — the
  full procedural showcase: every geometry and heat-tint map in it is
  a native chunk (guilloché disks, profile sweep, path instances,
  melded SDFs, oxide-dose painters).  The reference for the
  authored-data pattern (§"Procedural chunks" below).

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
   point for a **directional** sun on a Lambertian-white scene.  But
   `omni_light` falls off as 1/r² — `power 3.14` at a 30-unit
   distance delivers ~0.003 irradiance and renders BLACK.  Rule of
   thumb for omnis: `power ≈ π · r²` for full Lambertian brightness
   at distance `r` (≈ 2800 for r ≈ 30).  This trap recurs — the
   sweep_instances test scene hit it on first authoring (2026-06-11)
   even with the convention doc open.
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

## Procedural Chunks — generate geometry and maps in the scene, not in sidecar files

Since 2026-06 the scene language carries native procedural chunks.
Reach for these BEFORE writing a Python baker or checking in a mesh /
texture asset — they evaluate at parse time, keep the scene
self-contained, and live-rebind in the GUI like any named geometry.

### The catalog

- **`sweep_geometry`** — an arbitrary CLOSED 2D profile polygon
  (repeatable `profile_point <x> <h>` lines) swept along an arbitrary
  3D Catmull-Rom path (repeatable `point <x> <y> <z>` lines) with
  rotation-minimizing frames.  Tubes, rails, mouldings, bands, cables,
  straps.  Knobs: `n_len` (path samples), `end_scale_x` / `end_scale_y`
  (linear per-axis taper, e.g. a 20→16 mm strap is
  `end_scale_x 0.7997`), `cap_start` / `cap_end` (ear-clipped caps —
  non-convex profiles like grooved straps are fine), `frame_hint <x y z>`
  (initial binormal; omit = world axis most perpendicular to the start
  tangent — a planar YZ path therefore gets world X as the width axis).
  Conventions: profile **CCW = outward normals**; profile `x` maps to
  the frame binormal, `h` to the normal; UV = (profile arc fraction,
  path fraction).  Caveats: the path sampler is OPEN (reflective end
  padding) — a closed loop's two seam segments deviate from the ideal
  curve; sharp path corners get rounded by Catmull-Rom (author more
  control points to tighten).
- **`path_instances_geometry`** — a named TEMPLATE geometry stamped
  along a 3D Catmull-Rom path at arc-length `pitch`.  Fence posts,
  rivets, beads, stitching.  The template goes through the universal
  `TessellateToMesh` contract, so ANY first-class geometry works —
  including an `sdf_geometry` (the watch's stitch thread is an SDF
  capsule).  Axis convention: template **+Y → path tangent** (rotated
  by `slant` degrees about the frame normal — sign mirrors, giving
  saddle-stitch pairs), **+Z → frame normal**, **+X → binormal**.
  `phase` = arc distance before the first instance (omit = pitch/2,
  centred); `scale` uniform; `detail` = template tessellation.
- **`sdf_geometry`** — sphere-traced implicit primitives composed with
  smooth-min / boolean ops, authored as repeatable inline `part` lines:
  `<prim> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <a b c>  <round>`.
  Capsule semantics: `a` = radius, `b` = core half-length, axis =
  local Y (so a 1.35-long, 0.14-radius thread is `0.14 0.535 0`).
  First part must be `union`/`smin`.  Melded organic shapes (lugs
  flowing into a bezel, crowns, hands).
- **`guilloche_disk_geometry`** — engine-turned rose-engine relief
  baked over a disk (six pattern families: uniform / lightning /
  radial / iris / swirl / varwidth).  Any disk-shaped part: watch
  dials, pen caps, jewelry, lighters.  UV is the linear Cartesian map
  `u = (x+R)/2R`, so any 2D map applies independent of pattern.
- **`guilloche_oxide_painter` + `scalar_painter { function2d … }`** —
  the heat-tint pipeline.  The painter is a named IFunction2D giving a
  normalized thermal-oxide DOSE in [0,1] (Arrhenius radial profile,
  per-metal `metal ti|nb|ta|steel` kinetics presets, signed
  `torch_amount` along the pattern's mask); the scene then maps dose →
  nanometres with `scalar_painter { function2d <name> scale <span_nm>
  bias <centre_nm> }` feeding a thin-film material's `film_thickness`.
  Tuning the colour sweep = editing two numbers, no rebake.  An
  ABSOLUTE-temperature temper mode (`output thickness_nm` / `spall_mask`
  + `temp_center_c` / `temp_rim_c`) instead drives real per-metal oxide
  nm from a temperature ramp, with a matte oxide-scale blend past the
  flaking temperature (the comparison-render path).
- **`function2d_painter`** — the greyscale COLOUR wrapper of any named
  IFunction2D (`out = bias + scale·f(u,v)` on all channels).  The colour
  dual of `scalar_painter { function2d }`: use it when a procedural 2D
  field needs to feed a COLOUR slot or a `blend_painter` mask (e.g. the
  temper spall mask pushing `rd`/`rs` toward matte scale).  `scalar_painter
  { function2d }` for physical-scalar slots (roughness, thickness, IOR);
  `function2d_painter` for colour slots and masks.
- **`expression_function2d`** — a procedural 2D field whose value is a
  MATH EXPRESSION authored in the scene file (the in-scene-scripted
  analogue of `perlin2d` / the C++ guilloché field).  Variables `u, v`;
  declare `param <name> <number>` constants and `def <name> <expr>`
  named sub-expressions (let-bindings, in order), then the final `expr`.
  Functions: sin cos atan2 exp log sqrt abs floor frac sign mod min max
  pow hypot step clamp smoothstep mix select; operators `+ - * / % ^` and
  comparisons; consts pi tau e.  Usable as a displacement (with
  `displaced_geometry`), a greyscale colour, or a scalar.  This is how to
  author a NEW procedural pattern WITHOUT touching C++.
- **General displacement of any field onto any base.**  `displaced_geometry`
  takes an arbitrary `base_geometry` + any painter as `displacement`.
  For an OPEN field on a non-wrapping (Cartesian) UV (a guilloché
  `expression_function2d`), set `uv_seam_fold FALSE` (default TRUE
  tent-folds the UV for closed wrap-seam surfaces and would MIRROR an
  open field).  `cartesian_disk_geometry` is the flat Cartesian-UV disk
  base for the dial case.  A guilloché dial = `cartesian_disk_geometry` +
  a guilloché `expression_function2d` + `displaced_geometry uv_seam_fold
  FALSE`.

### The authored-data pattern

When a shape needs scene-specific knowledge (a particular
cross-section, derived offset paths, blessed parameter sets), that
knowledge belongs in the SCENE as authored data feeding general
chunks — never in a new chunk type.  The watch strap is the canonical
example: its FKM cross-section is 28 `profile_point` lines evaluated
once from the reference formulas; the stitch rows are two derived
3D paths.  Requirements when you do this:

1. Document the derivation in the chunk's comment (the formula or the
   reference script + invocation), so the data can be re-derived when
   the design changes.
2. Keep the generating script in the scene folder as the reference
   implementation if the derivation is nontrivial.
3. If you're tempted to add a new chunk instead: the feature-design
   bar is "would this chunk look reasonable in the engine's feature
   list next to `sphere_geometry`?"  Chunks are named by TECHNIQUE
   (sweep, instancer, disk), never by the scene part that motivated
   them.  `swept_band_geometry` (a strap generator in chunk costume)
   was excised for exactly this; the general
   `sweep_geometry` + `path_instances_geometry` replaced it.

### Parse-time cost awareness

Procedural chunks bake at PARSE time, every scene load.  A 560² disk
bake is ~0.5 s; the watch's six-dial live-rebind library is ~3 s and
~1.9 GB retained (each variant keeps its mesh + BVH for GUI
rebinding).  Author `mesh_n` / `n_len` / `detail` to what the shot
needs; don't reflexively max them.  For scenes with many alternates,
know that every declared geometry chunk is built whether or not an
object binds it.

### Scene-language hard rules (the parser will reject)

- Braces on their own lines; single-line chunks fail to parse.
- `nan` / `inf` / non-numeric tokens in any numeric parameter
  hard-fail at parse (text-domain validation — the build's
  `-ffast-math` makes value-domain NaN guards unreliable, so the
  parser is the wall).  Inline trailing `# comments` after values are
  fine.
- Unknown parameter names and unknown enum strings hard-fail with the
  chunk + line in the error.  Read the error before re-trying.

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
- **Writing a Python baker (or checking in a .raw2 / texture asset)
  for something the procedural chunks can express.**  Meshes from
  profiles/paths → `sweep_geometry`; repeated elements →
  `path_instances_geometry`; melded solids → `sdf_geometry`; engraved
  disks + heat tints → the guilloché pair.  Bakers mean stale assets,
  regeneration steps, and repo bloat.
- **Adding a scene-specific chunk type instead of authoring data for a
  general one.**  See "The authored-data pattern" above — this is a
  hard project rule, not a style preference.
- **Lighting a test scene with `omni_light power 3.14` at tens of
  units.**  That's the directional-light starting value; omnis need
  `power ≈ π·r²`.

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
