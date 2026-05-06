# Physically Based Rendering Pipeline Plan

End-to-end plan for closing the gap between RISE's path tracer and a
defensibly physically based renderer, motivated by the
`scenes/FeatureBased/Geometry/sponza_new.RISEscene` render coming out
visibly less realistic than the asset's bundled reference images.

The lens is **physical correctness first**.  When a feature has both a
physical and a non-physical implementation, we prefer the physical one
even when it costs more to build.  When a glTF feature is itself
non-physical (occlusion-baked-into-texture is the canonical example),
we still implement it for asset compatibility, but we hide it behind
an opt-in flag and document the departure.

This document is a working plan; it should reflect both the audit that
motivated each landing and the decisions reached during design.  Mark
landings DONE inline as they ship; don't delete them.

## Sibling docs

- [docs/IMPROVEMENTS.md](IMPROVEMENTS.md) — general engine improvement
  ranking; this plan references item #11 (Jakob-Hanika spectral
  uplifting) as a dependency of Landing 3.
- [docs/GLTF_IMPORT.md](GLTF_IMPORT.md) — glTF import phasing and the
  delivered/deferred matrix; this plan extends it with the importer
  fidelity items in Landing 12.
- [docs/SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) — scene authoring
  conventions; the HDR output and physical-camera landings will add
  new conventions that need recording here.

---

## Audit: NewSponza_Main_glTF_003 vs the current pipeline

### Asset inventory

Counts from cgltf parse of the .gltf JSON:

- 28 materials, 115 meshes (405 primitives), 155 nodes, 1 scene
- 6 perspective cameras, 24 lights (1 directional + 23 point) — **all
  authored at intensity = 0** (artist-leaves-lighting-to-renderer
  convention)
- 0 animations, 0 skins, 0 morph targets
- All primitives are indexed `TRIANGLES` mode

Material slot usage:

| Slot | Material count |
|---|---|
| `pbrMetallicRoughness.baseColorTexture` | 25 / 28 |
| `pbrMetallicRoughness.metallicRoughnessTexture` | 24 / 28 |
| `normalTexture` | 24 / 28 |
| `occlusionTexture` | **0** |
| `emissiveTexture` | **0** (the scene file's comment claiming "emissive maps for every surface" is wrong; only the `_Lights` and `_Curtains` companion glTFs ship those) |
| `alphaMode = OPAQUE` | 27 |
| `alphaMode = BLEND` | 1 (curtain) |
| `alphaMode = MASK` | 0 |
| `doubleSided` | 2 |
| Any `KHR_materials_*` | **0** |
| `KHR_texture_transform` | **0** |

Texture distribution: 72 textures backed by 72 distinct PNG images
(no KTX2 / `KHR_texture_basisu`).  One sampler entry, configuration
`magFilter = LINEAR / minFilter = LINEAR_MIPMAP_LINEAR / wrap =
REPEAT`.

Vertex attributes per primitive: `POSITION`, `NORMAL`, `TANGENT`,
`TEXCOORD_0`, `TEXCOORD_1` everywhere.  No `COLOR_0`, no skinning
attributes.  No material binds `texCoord = 1`, so the imported
`TEXCOORD_1` is currently dead weight.

### Pipeline status

What the engine already does correctly for this asset:

- BSDF: GGX (anisotropic alpha_x / alpha_y), Smith joint G2,
  Schlick Fresnel, Kulla-Conty multiscattering compensation —
  faithful to glTF 2.0 §B.  See `src/Library/Materials/GGXBRDF.cpp`.
- Metallic interpolation between dielectric F0 = 0.04 and conductor
  F0 = baseColor; diffuse term Lambertian, energy-conserved by
  `(1 - max(F0))` modulation.
- Normal-map TBN with imported per-vertex `TANGENT` and
  spec-correct `B = cross(N, T) * tangent.w`.  Falls back to
  `dpdu/dpdv` from triangle UV derivatives when no `TANGENT` is
  imported.  See [src/Library/Modifiers/NormalMap.cpp](../src/Library/Modifiers/NormalMap.cpp).
- Spectral integrators (PT, BDPT, VCM) with Hero Wavelength Spectral
  Sampling (HWSS) — see [docs/IMPROVEMENTS.md](IMPROVEMENTS.md) #5.
- Light BVH for many-light sampling (#4).
- OpenPGL path guiding for both eye and light subpaths (#2).
- Owen-scrambled Sobol + Z-Sobol blue-noise screen ordering (#6).
- Environment-map importance sampling (existing, on `radiance_map`).
- Null-scattering volumes (#7).
- OIDN denoise with albedo + normal AOVs.

Importer features wired:

- baseColor / metallicRoughness / normal / emissive textures
  (channel-extract for MR; factor multiplication for baseColor +
  emissive; `KHR_materials_emissive_strength`).
- `KHR_materials_unlit`, scalar subset of `KHR_materials_transmission`
  + `volume` + `ior`.
- alphaMode `OPAQUE` / `MASK` / `BLEND` (BLEND via per-material
  `transparency_shaderop`); `doubleSided` forwarded to
  geometry.
- `KHR_lights_punctual` (directional / point / spot) with optional
  uniform `lights_intensity_override` for assets that ship
  intensity = 0.
- Multi-camera (every camera-bearing node imported as a switchable
  named camera).
- Sampler wrap modes (`REPEAT` / `CLAMP_TO_EDGE` / `MIRRORED_REPEAT`).
- `lowmem_textures` mode that defers sRGB→linear decode to per-sample
  access (NewSponza-class assets).

### Gaps that bite this scene

Ranked by visual contribution to the "doesn't look realistic" delta:

1. **No texture pre-filtering.**  Bilinear-only sampling of 2K/4K
   PBR textures viewed at column-distance produces aliasing
   speckles that don't fade with spp.  This is the single largest
   source of the "noise that isn't variance" look in the current
   render.
2. **Sky environment is a uniform RGB.**  No directional sun, no
   sky gradient → no chiaroscuro → reads as overcast smog rather
   than the bright atrium the asset depicts.
3. **No HDR output / display transform separation.**  Linear
   radiance is written straight into 8-bit sRGB PNG via the
   existing `file_rasterizeroutput`, which clips highlights and
   crushes shadows.  No EXR primary output today.
4. **Light unit blindness.**  `lights_intensity_override = 100`
   conflates lux (directional illuminance) and candela (point
   intensity) — physically off by ~5 orders of magnitude between
   them.  The override is workflow tape, not a unit-respecting
   replacement.
5. **No physical camera.**  `pinhole_camera` is geometry-only; the
   render lacks an exposure response, so the scene's brightness
   has to be tuned by tweaking `radiance_scale` on the env map.

### Gaps that don't bite this scene but bite the next one

- `occlusionTexture` (Sponza has none; almost every other PBR asset
  does)
- `KHR_texture_basisu` (KTX2) — de-facto distribution format
- `KHR_texture_transform` — sub-atlas UV manipulation
- `TEXCOORD_1` material routing — AO commonly lives on UV1
- ORM-packed textures (R = AO, G = roughness, B = metallic)
- `KHR_materials_specular`, `anisotropy`, `iridescence`
- Clearcoat / sheen / transmission **textures** (the scalar factors
  are wired; the texture slots are `#if 0` pending energy-conservation
  audit on layered composition)

### Decisions reached during design

- **PB-first when the choice is binary.**  HDR pipeline before any
  feature work; spectral upsampling before more material extensions;
  ray differentials over screen-space heuristic LOD.
- **glTF `occlusionTexture` is IN, but pragmatic.**  A path tracer
  computes real occlusion via shadow rays, so multiplying baked AO
  on top double-counts.  But authored glTF assets routinely bake
  micro-occlusion (column flutes, brick mortar) at frequencies the
  geometry can't recover.  Decision: implement, default to applying
  only to *indirect-diffuse below a bounce threshold* (low-frequency
  fill that doesn't double-count direct lighting), expose a flag to
  disable, document the compromise.
- **No Disney/Frostbite empirical diffuse.**  If we want to upgrade
  past Lambertian, use Oren-Nayar (physically derived from rough
  Lambertian-microfacet).  Don't introduce empirical curves into
  a PB renderer.
- **Layered materials get an energy-conservation audit BEFORE we
  expose more layer textures.**  Naive `(1-Fc)·base + Fc·layer`
  composition under-conserves at grazing angles; need
  Belcour-style directional-albedo compensation.

---

## Roadmap

Numbered in execution order, not priority — earlier landings unblock
later ones.  Each landing should be independently shippable and
verifiable.  Landings 1–6 are foundational; 7–14 are feature work
that builds on the foundation.

| # | Landing | Category | ABI break? | Depends on |
|---|---|---|---|---|
| 1 | HDR primary output (EXR + separated exposure / display transform) | Output | No | None |
| 2 | Ray differentials → mip LOD → stochastic mip selection | Texture | No | None |
| 3 | Spectral upsampling (Jakob-Hanika) + spectral Hosek-Wilkie sun-and-sky | Spectral | No | IMPROVEMENTS.md #11 |
| 4 | Per-light-type intensity override (drop unit-blind override) | Lights | Minor | None |
| 5 | Physical camera model (ISO / aperture / shutter / EV) | Camera | No (additive) | 1 |
| 6 | Energy-conservation audit on layered material composition | Materials | Maybe | None |
| 7 | `KHR_materials_specular` (real F0 + tint) | Materials | Yes (PBR mat params) | 6 |
| 8 | Anisotropy material parameter (BRDF already supports it) | Materials | Yes (PBR mat params) | 6 |
| 9 | `KHR_materials_iridescence` (thin-film) | Materials | Yes | 6, 3 |
| 10 | Wake clearcoat / sheen / transmission textures (`#if 0` slots) | Materials | Yes | 6 |
| 11 | `KHR_materials_volume` end-to-end (volumetric attenuation behind refractive surfaces) | Materials | Yes | 7, existing null-scattering volumes |
| 12 | Importer fidelity batch: KHR_texture_transform, KTX2 / Basisu, ORM packed, TEXCOORD_1 routing | Importer | Minor | None |
| 13 | `occlusionTexture` (pragmatic; indirect-only fill, opt-in) | Materials | Yes | 12 |
| 14 | Verify alphaMode = BLEND on Sponza curtain; fix if broken | Materials | No | None |

### Verification scaffolding (cross-cutting)

Before landing 1, set up:

- A control scene with a Lambertian sphere, a perfect-mirror sphere,
  and a checkerboard plane under a known directional light + uniform
  env.  Expected pixel values can be computed analytically; this is
  the regression oracle for landings 1, 4, 5.
- A "spectral gold sphere" scene (gold conductor under D65 sun + sky).
  RGB-only path produces a yellow-gray; spectrally correct path
  produces the recognizable warm gold hue of measured spectral F0.
  Regression for landing 3.
- A texture-aliasing scene with a high-frequency checker on a plane
  receding to the horizon.  Bilinear-only path produces moiré +
  fireflies; mip-LOD path produces clean band-limited fall-off.
  Regression for landing 2.
- The actual NewSponza render at fixed seed, fixed camera
  (`PhysCamera001` framing), fixed spp, written as EXR.  Track
  variance + RMSE-vs-reference per landing per the
  [variance-measurement skill](skills/variance-measurement.md).

---

## Landing 1 — HDR primary output

**Detailed design: [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md)**

### Goal

Render to linear-radiance EXR as the primary output.  PNG becomes a
derived view: `radiance × exposure → display_transform → sRGB encode`,
with exposure and display transform both as scene-level parameters
that don't touch the integrator.

### PB rationale

The rendered image is a linear radiance distribution.  Writing it as
8-bit sRGB-encoded PNG bakes both an exposure choice and a display
transform into the integrator output, which is unrecoverable.  The
integrator should produce the ground truth (EXR); display is a
separate concern.

### Scope

In:

- New `file_rasterizeroutput` types `EXR` and `HDR_RGBE`.
- `exposure_compensation` parameter on `file_rasterizeroutput` (EV
  stops; default 0).
- `display_transform` parameter: `none` / `reinhard` / `aces` /
  `agx` / `filmic`.
- When PNG is requested, apply `exposure → display_transform → sRGB
  encode`.  When EXR is requested, write linear radiance verbatim
  (no exposure, no display transform — those are derived later).
- Convention: a scene can declare both an EXR and a PNG output; the
  PNG is the EXR rasterizer's display-transformed view.

Out:

- Per-pixel exposure (no auto-exposure, no histogram tone-mapping
  yet).
- Tone-mapping shader-ops in the chain (the standalone post-process
  is enough for now; promote later if we need composition).

### Code touch points

- `src/Library/Rendering/FileRasterizerOutput.{h,cpp}` — new output
  types, exposure / display transform parameters.
- `src/Library/Parsers/` — extend `file_rasterizeroutput` chunk
  descriptor.
- New `src/Library/PostProcess/DisplayTransform.{h,cpp}` housing the
  ACES / AgX / Reinhard / filmic curves as pure functions.
- Tests under `tests/` — `HDRRoundTripTest` (EXR write → read → diff
  bounded by float epsilon).

### Verification

- Control scene renders to both EXR and PNG; EXR pixels match
  analytic radiance to float epsilon, PNG pixels match the analytic
  radiance through the chosen display transform.
- Sponza render produces an EXR that, when post-processed by an
  external tool (oiiotool / Nuke), matches the PNG output our
  pipeline produces — proving the display transform isn't the
  exclusive property of the integrator.

---

## Landing 2 — Ray differentials and mip LOD

**Detailed design: [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md)**

### Goal

Compute per-ray surface footprints from ray differentials (Igehy
1999), translate to a mip LOD, and select one mip level per texture
sample stochastically (Olano-Baker-style) so we stay
"one texel sample per ray" without baking a multi-tap blur into the
integrand.

### PB rationale

Texture aliasing is a Nyquist undersampling problem: a pixel covers
many texels but bilinear sampling only sees one.  Path tracing's
high-spp pixel-area sampling does eventually band-limit the texture
lookup correctly, but at infeasible spp for 4K PBR textures.  Mip
LOD with ray-differential-driven footprint computation is the
standard PB approximation (PBRT, Mitsuba 3, Embree all do this).

Stochastic single-sample mip selection (vs deterministic trilinear's
two-sample blend) preserves the path tracer's "single sample per ray"
structure and is variance-equivalent in the limit.

### Scope

In:

- Ray differentials carried on `RayIntersectionGeometric` (pixel
  footprint as ∂P/∂x, ∂P/∂y).
- `RasterImage` mip pyramid (box-filter; lazy build on first sample).
- `TexturePainter` LOD selection from footprint + texture-space
  Jacobian.
- Stochastic mip selection: probability proportional to LOD
  fractional part.
- Default-on; per-painter `mipmap = FALSE` opt-out for normal maps
  if we observe artefacts (vector-quantity prefiltering is its own
  audit).

Out:

- EWA / true anisotropic filtering (later landing if needed).
- LOD bias parameter (let the next person ask for it).
- Normal-map slope-space prefiltering (LEAN / LEADR — separate
  research project).

### Code touch points

- `src/Library/Rendering/RayDifferentials.{h,cpp}` — new.
- `src/Library/Intersection/RayIntersectionGeometric.h` — add
  footprint members.
- All triangle-mesh / sphere / quad intersect routines — populate
  footprint on hit.
- `src/Library/Painters/TexturePainter.cpp` — LOD select + sample.
- `src/Library/RasterImage/RasterImage.{h,cpp}` — mip pyramid build
  + access.
- New regression scene: `scenes/Tests/Texture/mipmap_aliasing.RISEscene`.

### Verification

- Aliasing checker scene: bilinear-only render shows moiré +
  fireflies; mip-LOD render shows smooth band-limited horizon.
- Sponza render: the salt-and-pepper sparkle on stone columns
  disappears.
- HDRVarianceTest on an animated camera flythrough: variance must
  not increase (stochastic mip is variance-equivalent to trilinear,
  not worse).

---

## Landing 3 — Spectral upsampling + spectral sun-and-sky

### Goal

Upsample RGB textures to spectra via Jakob-Hanika sigmoid model
(IMPROVEMENTS.md #11) so the spectral integrators consume real
spectra, not RGB triples broadcast to wavelength bins.  Add a
spectral Hosek-Wilkie sun-and-sky environment so the env light
itself is wavelength-correct.

### PB rationale

Today's spectral path samples 3 wavelengths from a wide set, evaluates
texture as RGB, and broadcasts.  This makes spectrally meaningful
phenomena impossible: gold's color from spectral conductor Fresnel,
chromatic shadow softening from the sun's spectrum, dispersion in
glass.  Jakob-Hanika upsampling closes the gap with a 4-coefficient
sigmoid per texel that reproduces the RGB on integration but lets
spectral queries return a proper distribution.

Hosek-Wilkie ships a spectral variant fitted to atmospheric
scattering; the RGB variant is itself a per-channel integration of
the spectral one.  Using the spectral form is just "skip the
integration step we'd have to undo at sample time."

### Scope

In:

- IMPROVEMENTS.md #11 (Jakob-Hanika), per its existing scope.
- Spectral Hosek-Wilkie env painter
  (`hosek_wilkie_spectral_painter`).  Parameters: solar elevation,
  azimuth, turbidity, ground albedo.
- Solar disc as a directional light (geometric sun + sky, both from
  the same model so they're consistent).
- Per-light spectral SPD support (where missing).  Existing
  `directional_light` already accepts spectral painter; verify
  end-to-end.

Out:

- Polarization (whole separate axis).
- Time-of-day animation helper (just a parameter today).
- Aerial-perspective volumetric atmosphere (later, after
  null-scattering volumes are wired into env paths).

### Dependencies

- IMPROVEMENTS.md #11 must land first (or as part of this landing).
- Landing 1 (HDR pipeline) — required because the spectral sky has
  high dynamic range.

### Code touch points

- `src/Library/Painters/JakobHanikaUpsampler.{h,cpp}` — new.
- `src/Library/Painters/HosekWilkieSpectralPainter.{h,cpp}` — new.
- Importer wiring: when a baseColor / emissive texture is registered
  in spectral mode, build the upsampling coefficients per pixel and
  store alongside the RGB texture.
- New regression scenes: spectral gold sphere, spectral glass prism
  (dispersion).

### Verification

- Gold sphere control: RGB path produces yellow-gray; spectral path
  produces measured gold tint (compare to PBRT-v4's gold reference).
- Sky control scene: noon clear-sky pixels match published Hosek
  reference radiance to within 5% per wavelength bin.
- Sponza: warm sun + cool sky should produce *visible* color
  contrast between sunlit and shaded regions, not just luminance
  contrast.

---

## Landing 4 — Per-light-type intensity override

### Goal

Drop the unit-blind `lights_intensity_override` and replace with
type-respecting overrides that match glTF's unit semantics.  Defaults
preserve current "do nothing" behavior.

### PB rationale

KHR_lights_punctual specifies:

- Directional: `intensity` in lux (illuminance, lm/m²).
- Point / spot: `intensity` in candela (lm/sr).

A uniform 100 across both is physically meaningless — 100 lux is dim
overcast, 100 cd is a bright lamp.  The override should respect the
declared units.

### Scope

In:

- `directional_intensity_override` (lux, default 0 = no-op)
- `point_intensity_override` (candela, default 0 = no-op)
- `spot_intensity_override` (candela, default 0 = no-op)
- Deprecate `lights_intensity_override` with a warning that it
  applies to all types uniformly (kept for back-compat one release).
- Document in `SCENE_CONVENTIONS.md` that glTF light intensities
  are physical units that should not be tuned by trial-and-error
  when the asset ships real values.

Out:

- IES profile import (separate landing).
- Photometric unit conversion helpers in the parser (lumens →
  candela for point lights at given solid angle — too much DSL).

### Code touch points

- `src/Library/Importers/GLTFSceneImporter.{h,cpp}` — split the
  override field, update the chunk parser.
- `src/Library/Parsers/AsciiSceneParser.cpp` — update `gltf_import`
  descriptor.
- `scenes/FeatureBased/Geometry/sponza_new.RISEscene` — migrate
  to the new fields with reasonable per-type values
  (sun ~10 klux, lamps ~50 cd) once the override is split.

### Verification

- Render a single directional light at 10 klux on a Lambertian
  plane: pixel value matches `albedo · 10000 / π · cos θ` in the
  EXR output (requires Landing 1).
- Same for a point light at 100 cd at distance 1 m: pixel matches
  `albedo · 100 / π · cos θ` (1 / r² implicit in distance).

---

## Landing 5 — Physical camera

### Goal

Add a `physical_camera` chunk with ISO, aperture (f-number), and
shutter speed parameters that compute exposure from physical inputs,
producing a ratio of scene radiance to sensor signal that's
unit-correct.

### PB rationale

A pinhole geometry doesn't have an exposure response.  In a PB
pipeline, scene radiance is in cd/m²; sensor signal is a function
of incident illuminance × shutter time × ISO sensitivity / aperture
area.  Exposing this directly lets scenes be authored in real units
("the sun is 1.6e9 cd/m², the lamp is 50 cd") without trial-and-error
brightness tweaks.

### Scope

In:

- `physical_camera` chunk that wraps `pinhole_camera` (or thin-lens)
  with ISO / f-number / shutter parameters.
- EV computation: `EV = log2(N² / t) - log2(ISO/100)` per the
  standard photographic formula.
- Sensor exposure factor folded into the rasterizer's accumulation
  (or applied at output time — TBD during design).

Out:

- Lens distortion / chromatic aberration (separate landing).
- Sensor noise model (separate landing).
- Bokeh aperture shape (already supported on the existing
  thin-lens; wire through).

### Dependencies

- Landing 1 (HDR pipeline) — required so exposure can be a
  meaningful physical multiplier rather than a workaround for PNG
  clipping.

### Code touch points

- `src/Library/Cameras/PhysicalCamera.{h,cpp}` — new.
- `src/Library/Parsers/` — `physical_camera` chunk descriptor.
- Documentation: `SCENE_CONVENTIONS.md` gets a "Physical units"
  section.

### Verification

- Render the same Sponza-like control scene with three EV settings
  (-2, 0, +2); pixel values should differ by exactly 4× factors in
  the EXR.
- A scene authored entirely in physical units (sun = 1.6e9 cd/m²,
  diffuse plane albedo 0.5, camera ISO 100 / f8 / 1/250 s) should
  render to a sensible mid-gray on the plane.

---

## Landing 6 — Layered material energy-conservation audit

### Goal

Audit the existing layered-material composition path (clearcoat,
sheen, transmission scalar factors are already wired) for energy
conservation.  Implement Belcour-style directional albedo
compensation if the audit shows under-conservation.  This is a
**precondition** for landings 7-11 (more layer textures and
extensions).

### PB rationale

Naive `(1-F_layer) · base + F_layer · layer` composition over-darkens
at grazing angles because the integrated `F_layer` over the
hemisphere is not equal to the per-direction `F_layer` value.  Belcour
2018 ("Efficient Rendering of Layered Materials") provides the
directional-albedo compensation that closes this.

Sheen has its own energy-loss problem (Charlie distribution loses
energy at grazing) requiring a precomputed sheen albedo LUT to
compensate the base BRDF below.

### Scope

In:

- White-furnace test on the existing layered stack (clearcoat over
  PBR base, sheen on top of base).  A perfectly white material
  under a uniform white env should integrate to white; deviation
  from white = energy loss (or gain).
- Implementation of compensation if the audit fails.
- New test under `tests/`: `WhiteFurnaceLayeredTest` that exercises
  every supported layer combination.

Out:

- Spectral compensation (the LUTs are scalar; spectral compensation
  needs per-wavelength LUTs — defer until we see a real artifact).

### Code touch points

- `src/Library/Materials/PBRMetallicRoughnessMaterial.cpp` (or
  wherever layer composition lives — locate during the audit).
- New `src/Library/Materials/LayerCompensation.{h,cpp}` if
  compensation is needed.
- Build-time LUT generation for sheen albedo (one-time, baked into
  source).

### Verification

- White-furnace test passes for every (base, clearcoat, sheen,
  transmission) combination to within 1% of unity.
- Existing scenes that use clearcoat / sheen / transmission render
  identically (no regression) when the compensation is a no-op for
  the test parameters.

---

## Landing 7 — `KHR_materials_specular`

### Goal

Implement `KHR_materials_specular` (specularFactor + specularTexture
+ specularColorFactor + specularColorTexture) so dielectric materials
can have F0 set independently of the 0.04 default and tinted
non-white.

### PB rationale

The default dielectric F0 = 0.04 is appropriate for plastic / common
polymers; many real materials (paint, varnish, water) have measured
F0 outside this default.  glTF's specular extension exposes the
control directly.  Without it, a PBR asset author has no PB way to
match a measured material.

### Scope

In:

- New material parameters on `pbr_metallic_roughness_material`:
  `specular_factor` (scalar) and `specular_color` (RGB).
- Importer wiring for `KHR_materials_specular`.
- glTF spec: F0 = `specularColor · 0.08 · specularFactor` for
  dielectrics; metals are unchanged.

Out:

- specularGlossiness fallback (separate; rarely seen in modern
  assets; punt).

### Code touch points

- `src/Library/Materials/PBRMetallicRoughnessMaterial.{h,cpp}` — new
  parameters; ABI break on its constructor.
- `src/Library/Job.cpp` — `AddPBRMetallicRoughnessMaterial` gets
  new parameters.
- `src/Library/RISE_API.h` — extend exported signature.
- `src/Library/Importers/GLTFSceneImporter.cpp` — read the
  extension.

### Verification

- Existing PBR test scenes (no specular extension declared) render
  identically (F0 = 0.04 by default).
- New test scene with `specular_factor = 0` (kills the dielectric
  highlight) renders as pure Lambertian.
- Test with `specular_color = (1, 0.5, 0.5)` produces the expected
  red-tinted highlights.

---

## Landing 8 — Anisotropy material parameter

### Goal

Expose anisotropy on `pbr_metallic_roughness_material`.  The
GGXBRDF already supports `alpha_x ≠ alpha_y`; the material just
collapses to isotropic.  Add anisotropy strength + tangent rotation
parameters that derive `alpha_x / alpha_y` from a single roughness +
anisotropy pair.

### PB rationale

Brushed metal, hair, fabric all have anisotropic specular response
that an isotropic GGX cannot reproduce.  The BRDF is already
anisotropy-aware; the material is the missing wiring.

### Scope

In:

- `anisotropy_factor` (scalar in [-1, 1]) and
  `anisotropy_rotation` (radians) parameters on the material.
- glTF formula: `α_x = α · sqrt(1 - 0.9·anisotropy)`,
  `α_y = α · sqrt(1 + 0.9·anisotropy)` (per `KHR_materials_anisotropy`).
- Importer wiring for `KHR_materials_anisotropy` (factor + texture).
- Anisotropy frame derived from the geometry tangent (already
  imported via `TANGENT`).

Out:

- Anisotropy texture (the full extension — wire only the factor in
  this landing; texture lands with #12).

### Code / verification per the same template as Landing 7.

---

## Landing 9 — `KHR_materials_iridescence`

### Goal

Implement thin-film iridescence on top of the dielectric Fresnel,
producing the soap-bubble / oil-slick / soap-film color shifts that
arise from wavelength-dependent thin-film interference.

### PB rationale

Iridescence is a real interference phenomenon, modeled by adding
a phase shift to the Fresnel based on optical path length through
the thin film.  Belcour-Barla 2017 "A Practical Extension of
Microfacet Theory for the Modeling of Varying Iridescence" is the
standard reference and the basis for the glTF extension.

### Dependencies

- Landing 3 (spectral) — iridescence is fundamentally spectral; an
  RGB approximation exists but a spectral renderer should use the
  spectral form.

### Scope and code touch points

Per Landings 7-8 template plus a new
`src/Library/Materials/IridescentFresnel.{h,cpp}` housing the
Belcour-Barla formulation.

---

## Landing 10 — Wake clearcoat / sheen / transmission textures

### Goal

The texture slots for `clearcoatTexture`, `clearcoatRoughnessTexture`,
`clearcoatNormalTexture`, `sheenColorTexture`,
`sheenRoughnessTexture`, and `transmissionTexture` are wired in
the importer behind `#if 0` blocks pending material-side support.
Wake them.

### Dependencies

- Landing 6 (energy-conservation audit) — must pass before we
  expose more layer textures.

### Scope

For each texture slot: importer-side `enqueue` in
`PreDecodeTextures`, painter creation in `CreateMaterial`, material
parameter wiring.  Mostly mechanical given the existing scalar paths.

---

## Landing 11 — `KHR_materials_volume` end-to-end

### Goal

Beer-Lambert attenuation through refractive surfaces with a thickness
texture and attenuation distance / color, so glass and translucent
materials show real internal absorption.

### Dependencies

- Landing 7 (specular) — refractive surfaces use the specular F0
  to determine reflection/refraction split.
- Existing null-scattering volume framework (#7 in
  IMPROVEMENTS.md) — wire as the in-volume integrator when
  thickness and attenuation imply non-trivial transport.

### Scope

In: `thickness_factor` + `thickness_texture` + `attenuation_distance`
+ `attenuation_color` parameters on the material; importer wiring;
material wiring through to the existing volume integrator.

---

## Landing 12 — Importer fidelity batch

### Goal

Close the importer feature gap for next-generation PBR assets that
NewSponza doesn't exercise but that almost every other modern
PBR asset does.

### Scope

In:

- `KHR_texture_transform` — UV scale / offset / rotate per texture
  binding.  Implemented as a per-binding matrix that the painter
  applies before the underlying texture lookup.
- `KHR_texture_basisu` — KTX2 reader + Basis Universal transcode
  to a high-precision in-memory format.  Decode to full-precision
  linear RGB at load (don't keep block-quantized data; the
  quantization corrupts normals especially).
- ORM-packed texture detection — when the same image is referenced
  by both `metallicRoughnessTexture` and `occlusionTexture` slots,
  treat it as ORM packed and channel-extract once instead of
  decoding twice.
- `TEXCOORD_1` material routing — when a texture binding declares
  `texCoord = 1`, sample from the second UV set (currently we
  unpack the attribute but don't bind it).

Out:

- KTX2 with non-trivial supercompression (Zstd) — start with raw
  Basis Universal; add Zstd as a follow-up if needed.

### Code touch points

- `src/Library/Importers/GLTFSceneImporter.cpp` — texture
  enqueue + binding logic.
- `src/Library/Painters/UVTransformPainter.{h,cpp}` — new.
- `src/Library/Painters/KTX2Loader.{h,cpp}` — new (likely pulls in
  external Basis Universal source as a submodule).
- `src/Library/Geometry/TriangleMesh*.{h,cpp}` — verify TEXCOORD_1
  reaches the shading point through the existing v3 interface.

---

## Landing 13 — `occlusionTexture` (pragmatic)

### Goal

Import glTF occlusion textures and apply them as a low-frequency
modulator on **indirect diffuse only**, with a bounce threshold
below which the modulation is suppressed (so direct lighting and
the first indirect bounce — the bounces a path tracer handles best
— are unaffected).  Opt-in via a per-material flag; opt-out
documented as "more PB but loses authored micro-occlusion."

### PB rationale (and concession)

A path tracer computes real occlusion via shadow rays.  Multiplying
baked AO on top double-counts.  But assets routinely bake
micro-occlusion at frequencies the geometry can't recover (column
flutes, brick mortar joints), and refusing to consume the texture
loses information the artist deliberately encoded.  The pragmatic
compromise: apply only to indirect diffuse beyond the first bounce,
where the path tracer's occlusion term is less reliable anyway
(low-spp variance dominates), document the departure, default opt-in
for asset-faithful rendering, expose opt-out for strict-PB workflows.

### Scope

In:

- `AddOcclusionMapModifier` API + `occlusion_map_modifier` chunk.
- `respect_baked_occlusion` parameter on `gltf_import`
  (default TRUE).
- Bounce threshold: hard-coded to 1 (apply only when bounce ≥ 2)
  for now; expose as a parameter if we need per-scene control.
- TEXCOORD_1 routing (depends on Landing 12).

### Verification

- A control scene with a Lambertian sphere on a plane, no AO
  texture, renders identically with the modifier active vs absent
  (no-op when texture is uniform white).
- A scene with an AO texture darkening crevices renders darker in
  the crevices on indirect bounces only — direct lighting hitting
  the crevice is unchanged.

---

## Landing 14 — Verify alphaMode = BLEND on Sponza

### Goal

Render the Sponza curtain (the one BLEND material in the asset) and
confirm the existing transparency_shaderop path produces a sensible
result.  Fix any defects surfaced.

### Scope

In:

- A focused render of the curtain with backdrop visible through it.
- Compare against the asset's reference renders.
- If broken, fix in `src/Library/ShaderOps/Transparency*` or in the
  importer's shader-chain wiring.

Out:

- Order-independent transparency (we're a path tracer; transparency
  is just sampled, not raster-blended).

---

## Out of scope for this plan (deliberately)

- Hair / fiber BSDF — IMPROVEMENTS.md #10; orthogonal to PB
  pipeline correctness.
- Subsurface scattering improvements beyond what BSSRDF already
  delivers.
- Photon mapping / VCM tuning — separate, tracked elsewhere.
- Real-time / interactive editor support — see
  `docs/INTERACTIVE_EDITOR_PLAN.md`.

---

## Open questions still to resolve

These were flagged during design but not decided:

1. **Tone mapping placement (Landing 1).**  Parameter on
   `file_rasterizeroutput` (additive, simple) vs new `tone_map`
   shader-op in a chain (composable, more work).  Lean toward
   the simple parameter form for v1; promote to shader-op if
   compositing demands it.
2. **Sky model choice (Landing 3).**  Spectral Hosek-Wilkie
   (analytic, sun-position driven, ~500 lines) vs HDRI loader for
   `equirectangular_painter` (defers content choice to user).
   These aren't mutually exclusive — Hosek-Wilkie is the analytic
   sun-and-sky for synthetic scenes; an HDRI loader is for
   captured environments.  Probably ship both, in that order.
3. **Layer-composition compensation form (Landing 6).**  Belcour
   2018 directional-albedo LUT vs more recent (Zeltner et al.
   2022) "Practical Multiple-Scattering Sheen Using Linearly
   Transformed Cosines" specifically for sheen.  Decide during
   the audit based on which artifact pattern dominates.
