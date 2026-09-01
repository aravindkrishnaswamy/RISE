# Physically Based Rendering Pipeline Plan

**Status:** **PARTIALLY EXECUTED ROADMAP.** HDR output, ray differentials/mip
LOD, Rec.709 spectral uplift plus analytic spectral sky, physical exposure,
light units, several glTF material extensions, volume, and importer-fidelity
landings have shipped. The table below is the item-level source of truth for
remaining work; the three landing documents are executed design records.

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

| # | Landing | Category | ABI break? | Depends on | Status |
|---|---|---|---|---|---|
| 1 | HDR primary output (EXR + separated exposure / display transform) | Output | No | None | **DONE** |
| 2 | Ray differentials → mip LOD → stochastic mip selection | Texture | No | None | **DONE** (a78593b) |
| 3 | Spectral upsampling (Jakob-Hanika) + analytic spectral sun-and-sky | Spectral | No | IMPROVEMENTS.md #11 | **DONE — v1**. Rec.709 JH uplift and the `hosek_wilkie_skylight` API/chunk shipped; the sky model retains the documented Preetham-v1 internals rather than the planned Hosek-Wilkie coefficient fit. |
| 4 | Per-light-type intensity override (drop unit-blind override) | Lights | Minor | None | **DONE** (Group A bundle) |
| 5 | Physical camera model (ISO + fstop + shutter → EV stack into LDR outputs) | Camera | No (additive) | 1 | **DONE — minimal variant** (Group A bundle).  No new chunk; ISO is opt-in on existing `pinhole_camera` / `thinlens_camera`.  Realistic-camera (lens-element ray tracing) is a separate future landing. |
| 6 | Layered material energy-conservation audit | Materials | No | None | **PARTIAL — audit + sheen fix shipped**.  [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp) covers 19 configs.  Sheen grazing divergence (Findings B / configs #2, #6) FIXED via Imageworks production-friendly Charlie Λ (Estevez & Kulla 2017).  Composite recursion-budget loss (Finding A / configs #3, #7) remains its own future landing.  Detailed disposition in §"Landing 6" below. |
| 7 | `KHR_materials_specular` (specular_factor + specular_color → F0) | Materials | No (additive defaults) | None | **DONE** — `pbr_metallic_roughness_material` accepts `specular_factor` and `specular_color`; importer reads `KHR_materials_specular`'s scalar fields.  Defaults preserve every existing scene bit-identically.  Texture-sampled specular is L12. |
| 8 | Anisotropy (anisotropy_factor + anisotropy_rotation → α_x / α_y) | Materials | No (additive defaults) | None | **DONE**.  `pbr_metallic_roughness_material` accepts `anisotropy_factor` (αt = mix(α, 1, anisotropy²)) AND `anisotropy_rotation` (tangent-frame rotation around w; round-2 fix per adversarial review).  GGX BRDF / SPF now apply the rotation per shading point.  `anisotropy_texture` (per-pixel rotation + strength) is L12.  Defaults preserve every existing scene. |
| 9 | `KHR_materials_iridescence` (thin-film) | Materials | Yes | 6, 3 | TODO |
| 10 | Wake clearcoat / sheen / transmission textures (`#if 0` slots) | Materials | Yes | 6 | **PARTIAL** — sheen + transmission texture slots are wired in scene-language; clearcoat texture routing depends on Finding A composite redesign. |
| 11 | `KHR_materials_volume` end-to-end (volumetric attenuation behind refractive surfaces) | Materials | Yes | 7, existing null-scattering volumes | **DONE** — importer creates `HomogeneousMedium` from `attenuation_color` + `attenuation_distance` and binds it as `SetObjectInteriorMedium` on transmissive primitives.  Thin-walled (`thickness_factor == 0`) materials skip the medium per spec.  `thickness_texture` is ignored with a warning (RISE has no per-pixel medium parameters). |
| 12 | Importer fidelity batch: KHR_texture_transform, KTX2 / Basisu, ORM packed, TEXCOORD_1 routing | Importer | Minor | None | **PARTIAL** — KHR_texture_transform / ORM-packed / TEXCOORD_1 DONE; KTX2 detection-with-fallback DONE, full Basis Universal decoder is its own future landing. |
| 13 | `occlusionTexture` (pragmatic) | Materials | No (additive defaults) | None | **DONE — Phase-1 minimal**.  Importer wires `occlusionTexture` into a baseColor modulator chain (R-channel × occlusionStrength × strength painter).  Applied uniformly to all bounces (small over-darkening of direct lighting; recovers high-frequency baked AO geometry can't reach).  Opt-out via `respect_baked_occlusion FALSE` on `gltf_import`.  Bounce-gated "indirect-only" version is future work. |
| 14 | Verify alphaMode = BLEND | Materials | No | None | **DONE**.  Khronos's `AlphaBlendModeTest.glb` renders with three distinct behaviours per row (OPAQUE / MASK / BLEND), confirming the existing transparency_shaderop wiring works.  Sponza's `dirt_decal` BLEND material is too subtle to inspect in the canonical hero shot, so the test asset is the load-bearing verification. |

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

**Detailed design: [PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_3.md)**

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

## Landing 4 — Per-light-type intensity override [DONE — Group A bundle]

**Shipped.**  Three new `gltf_import` parameters — `directional_intensity_override`
(lux), `point_intensity_override` (candela), `spot_intensity_override`
(candela) — each replace zero authored intensities for their respective
light types only.  Per-type values win when both they and the legacy
`lights_intensity_override` are set on a given light type.  The
deprecated legacy field stays one release for back-compat with a parser
warning pointing at the typed forms.  No code change needed in scenes
that don't set any override; Sponza renders identically pre/post.

Migration of `scenes/FeatureBased/Geometry/sponza_new.RISEscene` to the
typed fields is a follow-up — kept on the legacy `lights_intensity_override 100`
for now to avoid a visible Sponza render change in the same commit that
introduces the API.

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

## Landing 5 — Physical camera [DONE — minimal variant, Group A bundle]

**Shipped — minimal variant, no new chunk.**  After comparing notes
with PBRT v4's own architecture (which treats ISO / shutter / EV as
display-side constants and reserves `RealisticCamera` for actual
lens-element ray tracing), L5 was scoped down to:

- `iso` parameter on existing `pinhole_camera` and `thinlens_camera`
  chunks.  Default 0 = physical exposure DISABLED — every pre-L5
  scene renders bit-identically.
- `fstop` parameter added to `pinhole_camera` (thinlens already had it
  for DOF; reused for EV when iso > 0).  `exposure` (shutter time,
  already present for motion blur) is the third leg.
- Camera computes `evCompensation = -log2(1.2) - log2(N² × 100 / (ISO × T))`
  per the UE5 / Filament saturation-based formula (ISO 12232).
- New virtual `ICamera::GetExposureCompensationEV()` (default returns 0).
- New virtual `IRasterizerOutput::SetCameraExposureCompensationEV(ev)`
  (default no-op).  Pixel-based rasterizer propagates the camera EV
  to every output once at frame start (in BOTH `RasterizeScene` and
  `RenderFrameOfAnimation` entry points).
- `FileRasterizerOutput` sums camera EV with its static
  `exposure_compensation` parameter to produce the total EV applied to
  LDR outputs (PNG / JPEG / PPM).  HDR archival outputs (EXR / RGBE)
  zero the camera EV in the setter to preserve "linear radiance ground
  truth" from L1.

Verified end-to-end via `scenes/Tests/Camera/physical_exposure.RISEscene`:
toggling iso/fstop/exposure dims the rendered sphere by exactly the
predicted factor on the PNG; EXR is bit-identical across settings.
Sponza + Cornell + shapes all render pixel-identical to pre-L5 (the
non-physical default path).

The realistic-camera (lens-element ray tracing à la pbrt-v4
`RealisticCamera`) is reserved as a separate future landing; the
keyword `realistic_camera` is already kept reserved by the parser
for it.

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

## Landing 6 — Layered material energy-conservation audit [PARTIAL — audit + sheen fix shipped]

**Audit scaffolding shipped.**  [tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp)
drives every L7-L11-relevant layered configuration through a Monte-
Carlo furnace test and reports the directional albedo `ρ(θ_i) =
E[Σ_j kray_j]` at θ ∈ {0°, 30°, 60°, 80°}.  Per-config posture
(`kPosturePass` / `kPostureBounded` / `kPostureKnownFailure`) keeps
the test useful for regression catching while documenting limitations
whose fix is its own future landing.  Test exits 0 today; any drift
will surface as a regression.

**Sheen grazing fix shipped (Finding B).**  [src/Library/Materials/CharlieSheen.h](../src/Library/Materials/CharlieSheen.h)
hosts the shared D / Λ / V helpers used by both
[src/Library/Materials/SheenBRDF.cpp](../src/Library/Materials/SheenBRDF.cpp)
and [src/Library/Materials/SheenSPF.cpp](../src/Library/Materials/SheenSPF.cpp).
Implements the Imageworks production-friendly Charlie shadowing-
masking (Estevez & Kulla 2017, the same closed form the Khronos
KHR_materials_sheen reference + glTF Sample Renderer use).  The
α-coefficient blend uses weight `w = (1 − α)²` on the α=0 endpoint
to match Khronos's `lambdaSheenNumericHelper`; an earlier draft used
linear blending which passed energy bounding but deviated ~3-8 %
from the reference renderer at mid-roughness.  Replaces the
Ashikhmin / Neubelt closed form whose `n·l + n·v − n·l·n·v`
denominator diverged as `cos θ → 0`.  Results: ρ(80°) on the sheen-
alone config drops from **8.7** to **0.55** (configurations #2 and
#6, both PASS at 5% bounded posture).

**Composite recursion redesign NOT yet shipped (Finding A).**
Configurations #3 and #7 still fail with ρ(0°) ≈ 0.04 — `CompositeSPF`'s
random-walk recursion budget clips below-layer diffuse paths under
delta-mediated specular tops.  Remains the dominant blocker for
clearcoat-over-paint scenes; its own landing.

> **ADDENDUM 2026-09-01 — Findings A and D were both misdiagnosed; row #3 is
> partly fixed and row #7 is a different bug entirely.**  The rows and the
> Findings prose below are left as the historical record; this paragraph is the
> correction.
>
> **Row #3 (dielectric / Lambertian) was NOT a recursion-budget failure.**  It
> was an IOR-stack threading bug in `CompositeSPF`'s random walk: the walk
> recursed with the `ior_stack` it was *handed* instead of each `ScatteredRay`'s
> *own* stack.  The camera ray's first refraction pushes the dielectric onto the
> scattered ray's stack, the walk discarded that, and the return trip therefore
> re-entered `ProcessTopLayer` with the **outside** stack and an **upward** ray.
> `IORStack::containsCurrent()` reads false, `DielectricSPF` takes its
> "entering from outside" branch, and both of its lobes are then culled — the
> transmission lobe by the hemisphere gate (an upward direction cannot be a
> transmission when entering from above) and the Fresnel lobe by the
> geometric-normal gate (reflecting an upward ray about `−N` points down).  The
> interface emitted **nothing**, so every gap-crossing path died inside the walk
> and the row sat at exactly the bare surface-Fresnel reflectance.  Measured
> directly: an upward ray handed to a bare `DielectricSPF(ior 1.5)` scattered
> nothing on 20000/20000 draws with the outside stack, and scattered a mean
> exiting `kray` of 0.949 with the pushed stack.  The same defect made
> `composite_material`'s `extinction` and `thickness` **exactly inert** — both
> apply only to gap-crossing legs — which is why an asymmetric per-channel
> extinction rendered exactly R=G=B.  Fixed by threading TWO IOR stacks through
> all four `Process*` variants (RGB and NM): `CompositeSPF::EvalStack` selects
> outside-vs-gap by the incoming ray's direction, and `GapStackBelowTop` updates
> the gap stack only on the top→bottom leg.  (A first-cut fix threaded each
> scattered ray's own stack unconditionally; review caught that this made a
> stack-sensitive *bottom* layer — both layers key the stack on the same
> `IObject*` — read `containsCurrent()==true` and take its from-inside branch,
> the same double-cull one layer down.)  Row #3 moves
> **{0.040, 0.042, 0.089, 0.388} → {0.3339, 0.3044, 0.3128, 0.5324}**, and the
> row is now gated at `kPostureMatchesPrediction` (eps 0.03) rather than waved
> through as a known failure.  The residual deficit *is* budget truncation —
> the ~56 % of the returning diffuse population that TIRs back down hits
> `kMaxReflectRecur = 2` at `steps = 2` and is dropped — so the original prose
> describes what is left, not what was wrong.  Guarded by
> [tests/CompositeExtinctionTest.cpp](../tests/CompositeExtinctionTest.cpp),
> which red-proves 10 checks against the pre-fix code.
>
> **Row #7 (clearcoat / red GGX-PBR) is not "the same bug in another regime".**
> It did not move at all across the fix ({0.0390, 0.0391, 0.0652, 0.1970} before
> and after), and it is not a budget effect either.  `GGXSPF` only ever emits
> **upward** lobes — reflection and diffuse; there is no transmission lobe in
> the file — so a GGX top layer never hands the walk a downward ray and
> **the substrate is never reached at all**.  Measured: GGX(rd=0, rs=0.04) over
> a white Lambertian reports ρ(0°) = 0.03900 against the *bare* coat's 0.03897,
> with zero downward rays emitted.  Rows #4 and #5 have the identical structure
> and only look healthy because their top layer's `rs = 1` masks the missing
> substrate at ρ = 1.  Closing #7 needs a transmission path for reflection-only
> top layers, not a walk fix; `coated_material` (#14/#15) is the shipped answer.
>
> **Separately, still open:** `extinction` is typed `const IPainter&`, so the
> spectral walk reads it through `GetColorNM` → the Jakob-Hanika **albedo**
> uplift, which is bounded to [0, 1].  An extinction of 50 arrives at the NM
> walk as ≈1.0: measured spectral attenuation 0.958 where the RGB walk gives
> 0.119.  Every extinction above ~1 is silently clamped in every spectral
> rasterizer.  This is the routing hazard
> [WETNESS_COAT_DESIGN.md §3.2](WETNESS_COAT_DESIGN.md) already flagged, now
> with a number on it; the fix is retyping the slot to `IScalarPainter`
> (ctor + `RISE_API` + `Job::AddCompositeMaterial` + the `composite_material`
> chunk descriptor + a scene migration) and was left out of the walk fix.

### Audit results (FURNACE_SAMPLES = 100,000 per (config, angle))

| # | Configuration | ρ(0°) | ρ(30°) | ρ(60°) | ρ(80°) | Disposition |
|---|---|---|---|---|---|---|
| 0 | Lambertian alone (sanity) | 1.0000 | 1.0000 | 1.0000 | 1.0000 | PASS @ 1% — methodology confirmed |
| 1 | GGX-PBR (schlick_f0, α=0.16) | 1.0025 | 1.0010 | 1.0032 | 1.0077 | PASS @ 5% — **⚠ RETRACTED 2026-09-01: the earlier {1.016, 1.015, 1.020, 1.032} and its "Kulla-Conty over-corrects ~1-3 %" reading were both ARTEFACTS OF THE HARNESS, not of GGX.** `DirectionalAlbedo` divided by the number of samples that produced a usable ray rather than by the sample COUNT, so every configuration with a nonzero rejection rate was re-normalised upward by 1/(survival rate) — this row rejects ~2 %, which is essentially the whole "over-correction". Corrected to `sum / FURNACE_SAMPLES`; the rejection rate is now printed per config so a value that moves can be told apart from a sampler that started rejecting. Kulla-Conty's residual gain at this roughness is ≤ 0.8 %, not 1-3 %. Finding C is retracted with it. |
| 2 | Sheen alone (Imageworks-Charlie Λ) | 0.088 | 0.128 | 0.281 | 0.545 | PASS @ 5% bounded — energy-bounded by the Estevez & Kulla 2017 production-friendly Λ; no grazing blow-up. |
| 3 | Composite: dielectric / Lambertian | **0.040** | **0.042** | **0.089** | 0.388 | KNOWN-FAIL — `CompositeSPF` random walk exits with the dielectric's surface-Fresnel only.  Below-layer diffuse paths get clipped by the recursion budget (`kMaxRecur = 4`, `kMaxDiffuseRecur = 2`).  This is the catastrophic loss the importer's Phase-5 warning flagged.  Needs the random-walk's recursion accounting redesigned, OR replacement with a proper analytic layered-BSDF (Belcour 2018 / Heitz 2017 LTC-Layered). |
| 4 | Composite: GGX / Lambertian | 1.0020 | 1.0005 | 1.0009 | 1.0100 | PASS @ 6% — non-delta GGX top doesn't trigger the recursion-loss path. Values corrected 2026-09-01 for the row-1 normalisation fix (this config rejects ~4 %, so the old numbers were inflated by about that much); the "systematic walk bias" the previous disposition inferred was mostly the same artefact. |
| 5 | Composite: GGX / GGX-PBR (clearcoat over PBR), white inputs | 1.0004 | 1.0012 | 1.0011 | 1.0094 | PASS @ 6% — passes with white baseColor; the importer's "near-black" warning was suspected stale based on this alone, but #7 below shows it's regime-dependent. Values corrected 2026-09-01 (row-1 normalisation fix; ~4 % rejection). |
| 6 | Composite: Sheen / GGX-PBR | 0.087 | 0.128 | 0.280 | 0.547 | PASS @ 5% bounded — Imageworks Λ keeps the layered case bounded too; sheen-over-PBR no longer blows up at grazing. |
| 7 | Composite: clearcoat / red GGX-PBR (Finding D) | **0.0390** | **0.0391** | **0.0652** | **0.1970** | KNOWN-FAIL — same loss profile as #3.  Confirms the importer warning at [GLTFSceneImporter.cpp:851](../src/Library/Importers/GLTFSceneImporter.cpp) is real for diffuse-dominant materials.  Tied to Finding A — same recursion-budget bug, different regime. |
| 8 | `polished_material`, tau=1.0 (full coverage) | 1.0000 | 1.0000 | 1.0000 | 1.0000 | PASS @ `kPostureMatchesPrediction`, eps=0.002 — 2026-08-31, [WETNESS_COAT_DESIGN.md §6.2](WETNESS_COAT_DESIGN.md#6-2-the-emitted-target-polished_material-and-why-not-ggx) Phase-1 exit-gate configuration. `c=1` zeroes the `Rd·Rs·(1−c)` deficit identically; measured matches the predicted `ρ=1.0` exactly. Doubles as the polished-fixture methodology check (analogue of #0). |
| 9 | `polished_material`, tau=0.5 (worst-case dip) | 0.9900 | 0.9894 | 0.9704 | 0.8265 | PASS @ `kPostureMatchesPrediction`, eps=0.002 — 2026-08-31, same doc §6.2. Predicted `ρ = 1 − 0.5·Rs(θ)` = {0.9900, 0.9894, 0.9704, 0.8265} (Rs at ior=1.33: {0.0201, 0.0211, 0.0591, 0.3469}), stored per-angle and enforced (not just an energy band) — the `Rd·Rs·(1−c)` analytic bound is confirmed, not just asserted, and the separate geometric-horizon coat-lobe drop §6.2 also describes adds no measurable extra loss on this flat, unperturbed fixture. |
| 10 | `polished_material`, tau=0.9 (recipe's pooled value) | 0.9980 | 0.9979 | 0.9941 | 0.9653 | PASS @ `kPostureMatchesPrediction`, eps=0.002 — 2026-08-31, same doc §6.2, tau matches the rain-wet-cobbles recipe's own pooled joints value ([WETNESS_COAT_DESIGN.md §6.5](WETNESS_COAT_DESIGN.md#6-5-worked-example-rain-wet-cobblestones)). Predicted `ρ = 1 − 0.1·Rs(θ)` = {0.9980, 0.9979, 0.9941, 0.9653}; enforced to within eps. This is the number §13's Phase-1 exit gate and §12 debt 5 ask for: at the recipe's actual coverage the coverage-mask deficit is ≤0.2% up to 60° and ~3.5% at 80° grazing. |

| 11 | `coated_material`: varnish (ior 1.5) / white Lambertian | 0.9998 | 0.9998 | 1.0008 | 0.9895 | PASS @ 2% `kPosturePass` — 2026-09-01, [WETNESS_COAT_DESIGN.md §7](WETNESS_COAT_DESIGN.md) Phase-2 exit gate. **The direct mirror of known-failing #3**: same white Lambertian substrate, same 1.5 coat IOR, full coverage. Nothing recurses — the coat's transmission is folded into the substrate lobe's throughput analytically — so there is no recursion budget to exhaust. HIGH-SUBSTRATE-ALBEDO (R=1), the case §7.4 names as the one that fails without the interreflection compensation. |
| 12 | `coated_material`: water (ior 1.33) / white Lambertian | 0.9997 | 1.0001 | 1.0004 | 0.9896 | PASS @ 2% `kPosturePass` — the wetness case at the highest substrate albedo there is. Red-proof twin is #16. |
| 13 | `coated_material`: water, coat_weight = 0.5 / white Lambertian | 1.0001 | 0.9999 | 0.9999 | 0.9944 | PASS @ 2% `kPosturePass` — §7.3's coverage is a sub-pixel AREA mixture, and both the coated and the bare branch conserve energy at R=1, so ρ stays 1 at every coverage. The discriminating contrast is #9, where `polished_material`'s `tau`-as-coverage stand-in loses 17 % at 80°. |
| 14 | `coated_material`: clearcoat (ior 1.5, α=0.16) / white GGX-PBR | 0.9997 | 1.0010 | 1.0104 | 0.8776 | PASS @ `kPostureMatchesPrediction`, eps=0.005 — config #7's SHAPE at white inputs. Cross-checked ANALYTICALLY against reference row #17 via `ρ = F + (1−F)·A_base·(1−r_i)/(1−r_i·R_hemi)`: agreement +0.0009 / +0.0016 at 0°/30°, −0.0125 at 60°, **−0.2187 at 80°**. The grazing divergence is root-caused, not noise — see Finding E. |
| 15 | `coated_material`: clearcoat / red GGX-PBR | 0.6777 | 0.6789 | 0.7002 | 0.6589 | PASS @ `kPostureMatchesPrediction`, eps=0.005 — **the apples-to-apples improvement claim against #7**: composite reports {0.0390, 0.0391, 0.0652, 0.1970} on the SAME substrate; coated reports 17× the energy at normal incidence. ρ cannot be 1 (the substrate absorbs; row #18 measures its ceiling). Analytic Saunderson agreement vs #18: +0.0007 / +0.0010 at 0°/30°, −0.0139 / −0.2137 at 60°/80° — the same profile as #14 on a completely different substrate. |
| 16 | `coated_material`: water / white Lambertian, **§7.4 recycling DISABLED** | 0.5375 | 0.5377 | 0.5562 | 0.6804 | PASS @ `kPostureMatchesPrediction`, eps=0.015 — **the red proof.** Identical to #12 except that §7.4's required interreflection compensation is switched off (a constructor flag, unreachable from the scene language), leaving plain Weidlich-Wilkie single bounce. Predicted `ρ = F + (1−F)(1−r_i)` with `r_i(1.33) = 0.471949` computed independently of the code under test: {0.5375, 0.5380, 0.5560, 0.6918}. **A 46 % energy loss at normal incidence — §7.4's "roughly 45 %", measured rather than asserted.** The configuration must keep FAILING conservation in exactly this shape, so a regression that quietly reintroduced or mis-weighted the term moves off the curve. |
| 17 | White GGX-PBR base alone (reference for #14) | 0.9988 | 0.9994 | 1.0251 | 1.1573 | KNOWN-FAIL — a bare-substrate REFERENCE row, present so #14 can be checked analytically rather than pinned to measurement. Goes **over unity at grazing (1.157)**: GGX's own low-F0 behaviour (Schlick grazing Fresnel rising toward 1 on top of a diffuse weight of `1 − maxF0` = 0.96, plus the Kulla-Conty tail). First measured here; recorded rather than gated, and NOT introduced by — or fixable from — `coated_material`. The coated row above it does not inherit the gain. See [IMPROVEMENTS.md](IMPROVEMENTS.md) §"GGX low-F0 grazing gain". |
| 18 | Red GGX-PBR base alone (reference for #15) | 0.8069 | 0.8074 | 0.8344 | 0.9631 | PASS @ 6% bounded — bare-substrate reference row for #15's analytic check. Supplies `A_base(θ)`, the substrate's ACTUAL directional albedo, which is a different quantity from the `R_hemi` the recycling denominator uses (0.8537143 here, closed-form). Conflating the two is the easy mistake; both appear in #15's derivation. |

### Findings, in priority order

**Finding A — Catastrophic dielectric/Lambertian loss (#3).**  Most
critical: this is the canonical "thin coat over diffuse paint"
pattern the importer was supposed to use.  The current
`CompositeSPF` random walk loses ~96% of energy at normal incidence
because dielectric refractions emerge as delta rays whose recursion
budget collapses before the diffuse below-layer scattering bubbles
out.  Two paths forward, both significant landings:
  - **A1 — Patch the random walk's recursion accounting** so
    delta-mediated paths to the bottom layer don't fall under the
    same budget as diffuse-recursion.  Smaller intervention; keeps
    the existing model.  Risk: doesn't close all loss, may surface
    different artefacts.
  - **A2 — Replace with an analytic layered BSDF** (Belcour 2018
    "Efficient Rendering of Layered Materials" or Heitz et al. 2017
    LTC-Layered).  Larger refactor; produces cleaner BRDF
    semantics; integrates with the L7-L11 layer extensions (sheen,
    clearcoat, iridescence) more naturally.

**Finding B — Sheen at grazing diverges (#2, #6).**  ✅ FIXED.
Replaced the Ashikhmin / Neubelt closed-form `V` with the Imageworks
production-friendly Charlie Λ (Estevez & Kulla 2017 — same form the
Khronos KHR_materials_sheen reference renderer uses).  The Λ
polynomial is energy-bounded by construction at all `cos θ ∈ [0, 1]`,
so no per-sample clamps and no LUT bake step needed.  Both #2 and #6
now PASS at the 5% bounded posture.  See [src/Library/Materials/SheenBRDF.cpp](../src/Library/Materials/SheenBRDF.cpp)
and [src/Library/Materials/SheenSPF.cpp](../src/Library/Materials/SheenSPF.cpp);
duplicated helpers across the value() and Scatter() sites are
documented as "keep in sync".

**Finding C — GGX baseline gains 1-3% via Kulla-Conty over-
correction (#1, #4, #5).**  ⚠ **RETRACTED 2026-09-01 — the finding was
a measurement artefact, not a property of GGX.**  `DirectionalAlbedo`
normalised by the count of samples that produced a usable ray instead
of by the sample count, so any configuration with a nonzero rejection
rate was scaled up by 1/(survival rate).  Configs #1, #4 and #5 reject
~2-4 %, which is essentially the entire "1-3 % over-correction" the
finding was built on.  With the denominator corrected the same configs
read 1.0025 / 1.0020 / 1.0004 at normal incidence and at most 1.0100 at
grazing.  Kulla-Conty's real residual gain at this roughness is
≤ 0.8 %.  No action needed, and specifically: **do not switch to
Turquin 2019 or Hoffman 2023 on the strength of this finding** — there
is no longer a 1-3 % gain to chase.  The harness now prints each
config's rejection rate so this class of artefact is visible rather
than inferred.

**Finding D — Importer's "near-black-clearcoat-over-PBR" warning
IS real, but only in the coloured / diffuse-dominant regime.**
The white-input GGX/GGX-PBR test (#5) passes at 6 % tolerance,
so the warning at first appeared stale.  Re-running with red
baseColor + metallic = 0 (#7 in the audit) reproduces the
catastrophic loss the warning predicts: ρ ≈ 0.04 at normal
incidence, growing to 0.20 at grazing — the same loss profile as
#3 (dielectric / Lambertian).  Same root cause: at small α
(≈ 0.16) and low F0 (≈ 0.04), the GGX top layer behaves like a
near-delta dielectric, and the diffuse-dominant base's exit
paths get clipped by the same `CompositeSPF` recursion budget
that clobbers #3.  Disposition: tied to Finding A — both are the
same bug, surfacing in different regimes.  The importer warning
at [GLTFSceneImporter.cpp:851](../src/Library/Importers/GLTFSceneImporter.cpp)
SHOULD remain in place until Finding A's recursion-budget fix
lands.

**Finding E — `polished_material`'s `tau`-as-coverage-mask deficit
(#8, #9, #10) is exactly the analytic `Rd·Rs·(1−c)` bound, with no
measurable extra loss.**  2026-08-31, added for
[WETNESS_COAT_DESIGN.md §6.2/§13](WETNESS_COAT_DESIGN.md)'s Phase-1
exit gate ("a furnace configuration putting a number on §6.2's
coverage dip").  `PolishedSPF`'s coat lobe carries `kray = tau·Rs`
but the substrate lobe stays `kray = Rd·(1−Rs)` rather than
`Rd·(1−tau·Rs)`, so intermediate coverage (`tau ∈ (0,1)`) loses
`Rd·Rs·(1−tau)` relative to a true coverage mixture.  Measured at
`tau = 0.5` (#9, the worst-case region) and `tau = 0.9` (#10, the
rain-wet-cobbles recipe's own pooled value) against the closed-form
prediction `ρ_pred(θ) = 1 − Rs(θ)·(1−tau)` for `Rd = 1`: **both match
to within 0.0001 at every incident angle**, and `tau = 1.0` (#8)
reproduces `ρ = 1.0000` exactly, confirming the fixture's
methodology.  **Configs #8-10 use a dedicated `kPostureMatchesPrediction`
posture** (added 2026-08-31, review round P1) rather than the generic
`kPostureBounded` used elsewhere in this table: it stores the
closed-form `ρ_pred(θ)` above per config/angle and fails when
`|measured − predicted| > 0.002`, so the gate actually checks the
predicted curve rather than just an energy band — `kPostureBounded`'s
`[0, 1+tol]` would silently pass both a regression that turns `tau`
into a no-op (`ρ` pinned near 1.0 regardless of coverage) and one that
lets the deficit run unbounded or invert into a gain.  Red-proved by
perturbing config #9's stored prediction by 0.01 at θ=80°: the test
correctly turned that row FAIL (exit 1) with the mismatch reported in
the note, then passed again once reverted.  §6.2 separately describes
a second, grazing-
concentrated loss from `PolishedSPF`'s geometric-horizon coat-lobe
rejection ([PolishedSPF.cpp:206-207](../src/Library/Materials/PolishedSPF.cpp))
— that mechanism needs a shading/geometric-normal MISMATCH (a tilting
modifier bending the shading normal away from the true surface), and
this furnace fixture is a single flat, unperturbed normal, so the
gate degenerates to the ordinary shading-hemisphere test and
contributes no additional measurable loss here.  **The `Rd·Rs·(1−c)`
term is therefore confirmed as the dominant, and at this fixture's
geometry the *only* measurable, component of §6.2's coverage dip** —
at the recipe's actual pooled coverage (`tau = 0.9`) the loss is
≤ 0.2 % up to 60° and ≈ 3.5 % at 80° grazing, small enough that Phase
1 ships as designed; `coated_material`'s `coat_weight` (Phase 2,
§7.4) remains the principled fix for the general case, including
scenes whose geometry DOES produce a shading/geometric mismatch.

### Disposition for L7-L11

The audit's findings unblock most of L7-L11 with caveats:

- **L7 (KHR_materials_specular):** unblocked.  Adds F0 to the
  single-layer GGX-PBR which is in the PASS bucket.
- **L8 (anisotropy):** unblocked.  Same reasoning.
- **L9 (KHR_materials_iridescence):** unblocked.  Iridescence is
  a Fresnel modifier, not a layer; energy comes from the Belcour-
  Barla formula directly.
- **L10 (clearcoat / sheen / transmission textures):** PARTIALLY
  blocked.  Clearcoat textures need the layered-PBR composite to
  be wired (importer's `#if 0`), which depends on Finding A's
  resolution.  Sheen is now energy-bounded (Finding B fixed) so
  sheen textures render correctly at all angles without further
  work.  Transmission textures are independent (single-layer
  dielectric).
- **L11 (KHR_materials_volume):** unblocked.  Beer-Lambert through
  a single-layer dielectric; doesn't traverse the random walk.

The audit is **not** a hard gate on L7-L9 — those can ship
independently, with the audit catching any new layer-related
regression they might introduce.  L10's clearcoat and sheen
sub-features should follow Finding A (Belcour or recursion-fix)
and Finding B (sheen LUT) respectively; transmission can ship
without either.

### Goal (original — superseded by audit findings above)

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


**Finding E — `coated_material` closes Finding A's regime, and its own
limit is measured and named (#11-#18, 2026-09-01).**  The
`coated_material` triad ([WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md)
§7) lands the configurations that mirror known-failing #3 and #7 in
`kPosturePass` — #11/#12/#13 hold ρ = 1 to within 2 % at the highest
substrate albedo there is, where #3 loses 96 %.  It does so WITHOUT
touching `CompositeSPF`: the coat's transmission is folded into the
substrate lobe's throughput analytically, so Finding A's recursion
budget is simply not in the path.  Finding A still stands for
`composite_material` itself, which remains the general layer-stacking
primitive; what changed is that the specific "transparent film over an
opaque substrate" case now has a material that gets it right.

The model's own boundary is measured rather than assumed.  Rows #14 and
#15 diverge from their analytic prediction by −0.219 and −0.214 at 80°
while agreeing to ~0.001 at 0° and 30° — **near-identical profiles on a
white substrate and on a coloured absorbing one**, which is what makes
the cause attributable rather than guessed.  The mechanism: the layer's
exit factor is DIRECTIONAL (`T(θ_o) = 1 − F(θ_o)`) while its recycling
coefficient is the DIFFUSE average `r_i`.  Those agree for a Lambertian
substrate — that agreement is the identity putting #11-#13 exactly on
1.0 — but a microfacet substrate returns light near its own mirror
direction, which at 80° incidence is also grazing, where `T(θ_o)` is
0.61 while `r_i` still says 0.60.  The model under-recycles precisely
where a specular substrate returns its light.  This is the limitation
§7.4 names IN ADVANCE as the Belcour trigger, and it is the same
Belcour 2018 / LTC-Layered option Finding A's A2 lists — so if A2 is
ever taken, it closes both.  Declined for v1 on scope, not on
correctness.

## Landing 7 — `KHR_materials_specular` [DONE]

**Shipped — scalar-factor + RGB-color, textures deferred to L12.**
`pbr_metallic_roughness_material` accepts two new parameters:

  - `specular_factor` — scalar in [0, 1] (or scalar painter) that
    scales the dielectric F0.  Default 1.0 = standard 0.04 dielectric
    F0; lower values reduce the specular highlight to match matte
    plastic / paint.  Metals are unaffected (their F0 is the
    `baseColor`, untouched by this parameter).
  - `specular_color` — RGB painter that tints the dielectric Fresnel.
    Default "none" = white (untinted).  Final dielectric F0 =
    0.04 × specular_color × specular_factor.

The importer (`GLTFSceneImporter`) reads `KHR_materials_specular`'s
`specularFactor` + `specularColorFactor[3]` when present and threads
them into the new parameters.  `specularTexture` + `specularColor-
Texture` are flagged with a one-line warning per material and
ignored — the texture wiring lands with the L12 importer-fidelity
batch.

Every pre-L7 scene renders bit-identically: the C++ side detects
both parameters at their default sentinel values (`"1.0"` / `"none"`)
and skips the painter chain, falling back to the original 0.04
constant.  Verified via `scenes/Tests/Materials/pbr_metallic_roughness.RISEscene`
and a new regression scene `scenes/Tests/Materials/pbr_specular_anisotropy.RISEscene`.

**Round-2 fix — RISE_API surface coverage.**  Original L7 / L8
extended `IJob::AddPBRMetallicRoughnessMaterial` but didn't add a
matching `RISE_API_CreatePBRMetallicRoughnessMaterial` entry point;
external embedders couldn't reach the new params without going
through scene parsing or internal C++ types.  Round-2 closes the gap:
the new RISE_API function constructs the same painter graph as the
Job-level version but without the painter-manager registry — every
internal helper is held alive via refcounts on the chain.  Verified
end-to-end by `tests/PBRMaterialAPITest.cpp`, which constructs PBR-
MR via the C API in three configurations (defaults, KHR_specular,
KHR_anisotropy + rotation) and asserts each produces a working
material with sensible directional albedo.

### Goal (original)

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

## Landing 8 — Anisotropy material parameter [DONE]

**Shipped — both `anisotropy_factor` and `anisotropy_rotation`.**
`pbr_metallic_roughness_material` accepts:

  - `anisotropy_factor` — scalar in [0, 1] (or scalar painter)
    controlling specular-lobe stretch along the surface tangent.
    Default 0 = isotropic (αx = αy = roughness²; bit-identical to
    pre-L8 PBR-MR).  Larger values produce αt = mix(α, 1, anisotropy²)
    along tangent and αb = α along bitangent — useful for brushed
    metal, hair, fabric.
  - `anisotropy_rotation` — scalar painter or string giving the
    tangent-frame rotation in radians.  Default 0 = aligned with
    the geometry's `TANGENT` attribute (or the dpdu fallback).
    **Round-2 fix**: rotation is now actually applied — the GGX
    BRDF / SPF rotate the (u, v) basis around w by the painter-
    evaluated angle before sampling.  The original L8 ship had
    rotation read from the parser / importer but discarded inside
    `Job::AddPBRMetallicRoughnessMaterial` with `(void)` — a
    visible correctness bug for any glTF asset that authors
    `anisotropyRotation` non-zero.  Adversarial review caught it;
    fix plumbs the rotation through `GGXMaterial` →
    `GGX{BRDF,SPF}` via a new optional `tangent_rotation` IPainter
    parameter (default nullptr / no rotation).  Sample at each
    shading point so a texture or procedural can drive it; scalar
    is a special case.

The importer reads `KHR_materials_anisotropy.anisotropy_strength` +
`anisotropy_rotation` and threads both into the new parameters.
`anisotropy_texture` (per-pixel variation) is flagged with a one-
line warning per material and ignored — L12 wires that alongside
the texture support.

The glTF spec uses `αt = mix(α, 1, anisotropy²)`, `αb = α` (the
"clamp top of stretching range to 1" form), which is what we
implement.  An alternative `α_x = α/aspect, α_y = α·aspect` form
(Disney) would be a separate landing if needed; both are common.

Verified via `scenes/Tests/Materials/pbr_specular_anisotropy.RISEscene`
which paints six spheres differing only in L7 + L8 parameters
(sphere 5 = anisotropy_factor=0.85; sphere 6 = same factor +
rotation=π/2 — visibly perpendicular highlight stretch confirms
the rotation applies).

### Goal (original)

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

## Landing 11 — `KHR_materials_volume` end-to-end [DONE]

### Status

**Shipped**: importer reads `attenuation_color` + `attenuation_distance`,
builds a `HomogeneousMedium` with σ_a = -log(attenuation_color) /
attenuation_distance per channel (Beer-Lambert), and binds it to each
volume-bearing object via `SetObjectInteriorMedium`.  Existing null-
scattering volume integrator does the absorption integral along
ray-traced path lengths through the medium.

`thickness_factor`: gated correctly per glTF spec.
- `thickness_factor > 0`: thick-walled (solid object).  RISE's path-
  traced ray length × σ_a does the absorption integral natively;
  current behaviour is correct.
- `thickness_factor == 0`: thin-walled (2D sheet).  RISE doesn't have
  a true thin-walled mode (would need σ_a applied as a per-surface
  attenuation factor on the transmission BSDF rather than along ray
  length).  Importer skips medium attachment + warns; recommended
  fix is to bake the colour into baseColor / τ.

`thickness_texture`: ignored with a warning.  Per-pixel thickness
modulation only matters for the thin-walled path (which RISE doesn't
implement); the thick-walled path uses geometric path length and
gets the right absorption integral without the per-pixel scale.

Verified end-to-end via [scenes/Tests/Importers/gltf_import_attenuation.RISEscene](../scenes/Tests/Importers/gltf_import_attenuation.RISEscene)
— Khronos's AttenuationTest array (18 materials testing varied
attenuation_distance + thickness_factor) renders with visible per-
material absorption variation.

### Dependencies

- Landing 7 (specular) — refractive surfaces use the specular F0
  to determine reflection/refraction split.
- Existing null-scattering volume framework (#7 in
  IMPROVEMENTS.md) — wire as the in-volume integrator when
  thickness and attenuation imply non-trivial transport.

---

## Landing 12 — Importer fidelity batch

### Goal

Close the importer feature gap for next-generation PBR assets that
NewSponza doesn't exercise but that almost every other modern
PBR asset does.

### Sub-landing status

| # | Item | Status |
| - | ---- | ------ |
| 12.A | `KHR_texture_transform` | **DONE** — `UVTransformPainter` wraps each binding when `view.has_transform == true`; identity transforms collapse to passthrough.  Math verified by `tests/UVTransformPainterTest.cpp` (7 / 7 pass: identity, T, S, R 90°, combined TRS, GetAlpha forwarding). |
| 12.B | ORM-packed detection | **DONE** — `IsORMPacked` detects when occlusion and metallicRoughness slots reference the same `cgltf_texture *`; `EffectiveRole` routes the AO painter lookup through the MR painter name when packed.  PreDecodeTextures' `seen` set then dedupes, eliminating one PNG decode + one painter manager entry per ORM-packed material.  Color-space and wrap mode are identical for both bindings (both linear, shared sampler), so the share is safe.  Untouched on non-ORM assets (DamagedHelmet, OrientationTest etc.). |
| 12.C | `anisotropy_texture` + `specular_texture` | **DONE — partial** — `specular_factor` × `specularTexture.A`, `specular_color` × `specularColorTexture.RGB`, and `anisotropy_factor` × `anisotropyTexture.B` are now per-pixel via the existing painter graph (ChannelPainter → BlendPainter chain into `AddPBRMetallicRoughnessMaterial`'s painter-string args).  No GGX BRDF/SPF change needed — the pre-existing `resolveOrSynth` in `Job::AddPBRMetallicRoughnessMaterial` already accepts a painter name in place of a scalar.  Per-pixel anisotropy DIRECTION (encoded in `anisotropyTexture.RG`) requires `atan2(2G−1, 2R−1)` per sample, which doesn't compose from BlendPainter / ChannelPainter primitives — deferred to a follow-up landing that adds an `ATan2Painter` (or changes `pTangentRotation`'s contract to a (cos, sin) pair).  Importer logs once when an asset uses per-pixel rotation so the visual mismatch is visible. |
| 12.D | `TEXCOORD_1` routing | **DONE** — `ri.ptCoord1` + `bHasTexCoord1` added to RayIntersectionGeometric.  Triangle-mesh intersection interpolates `pTexCoords1` parallel to `pCoords` (recovers index via pointer arithmetic, no PointerTriangle bloat).  New `TexCoord1Painter` wraps any source painter and swaps in `ri.ptCoord1` before sampling.  Importer's `WrapWithUVTransform` now also wraps with TexCoord1Painter when the binding declares `texCoord = 1` (textureInfo.texCoord OR KHR_texture_transform.texCoord override).  Verified end-to-end via [scenes/Tests/Importers/gltf_import_multi_uv.RISEscene](../scenes/Tests/Importers/gltf_import_multi_uv.RISEscene) — Khronos's MultiUVTest renders the gltf logo (emissive on UV1) overlaid on the +pattern (baseColor on UV0). |
| 12.E | `KHR_texture_basisu` (KTX2) | **PARTIAL — graceful detection + fallback** — importer detects `tex->has_basisu` and either (a) falls back to the texture's PNG/JPEG `source` with a warning, or (b) returns empty when the asset is KTX2-only (binding renders as the material's uniform fallback, no crash).  Full KTX2 / Basis Universal decoding requires the basisu submodule + transcoder integration; deferred to its own landing. |

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
  enqueue + binding logic; per-call-site `WrapWithUVTransform`
  helper applied at all 6 `CreateTexturePainter` sites
  (basecolor, MR, occlusion, emissive, normal, sheen color).
- `src/Library/Painters/UVTransformPainter.h` — header-only
  passthrough wrapper; `GetColor` / `GetColorNM` / `GetAlpha`
  apply T·R·S to ri.ptCoord and forward to source.
- `src/Library/RISE_API.{h,cpp}`, `src/Library/Job.{h,cpp}` —
  `Add/CreateUVTransformPainter` plumbing.
- `src/Library/Painters/KTX2Loader.{h,cpp}` — new (likely pulls in
  external Basis Universal source as a submodule).
- `src/Library/Geometry/TriangleMesh*.{h,cpp}` — verify TEXCOORD_1
  reaches the shading point through the existing v3 interface.

---

## Landing 13 — `occlusionTexture` (pragmatic) [DONE — Phase-1 minimal]

**Shipped — applies AO as a baseColor multiplier across all
bounces.**  glTF's `occlusionTexture` is now imported and threaded
into the per-material baseColor painter graph as:

  `final_baseColor = baseColor × lerp(white, ao_R, occlusionStrength)`

The R channel is extracted via `ChannelPainter`; `occlusionStrength`
defaults to `1.0` per the glTF spec (the texture-sampler's `.scale`
field).  When the strength is 0, the modulator collapses to white
(no AO effect).  When the texture is absent, the chain is skipped
entirely — every pre-L13 scene renders bit-identically.

**`respect_baked_occlusion` knob** on `gltf_import` (default `TRUE`)
opts out for strict-PB workflows where you want only the path
tracer's computed occlusion.

**Phase-1 caveat — bounce-uniform application.**  The plan's
original design proposed applying AO only on bounces ≥ 2 (so direct
lighting and the first indirect bounce are unaffected, matching the
"path tracer handles those best" intuition).  Implementing that
requires plumbing bounce count from the integrator into the painter
evaluation, which `RayIntersectionGeometric` doesn't currently
carry.  Phase-1 ships the simpler "AO multiplies all bounces"
variant; the small over-darkening of direct lighting is documented
and accepted in exchange for not touching the integrator/painter
boundary.  A bounce-gated v2 is a future refinement (likely
alongside the broader integrator-context cleanup planned for
shader-op evaluation).

Verified end-to-end via [scenes/Tests/Importers/gltf_import_damaged_helmet.RISEscene](../scenes/Tests/Importers/gltf_import_damaged_helmet.RISEscene)
— Khronos's DamagedHelmet is the canonical occlusion-bearing test
asset.  Comparison render with `respect_baked_occlusion FALSE`
shows uniform brightness; the default `TRUE` render shows visibly
darker recesses (visor area, panel seams, riveted edges) consistent
with the artist-baked AO.

### Goal (original)

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

## Landing 14 — Verify alphaMode = BLEND on Sponza [DONE]

**Verified — BLEND machinery works.**  Khronos's `AlphaBlendModeTest.glb`
([scenes/Tests/Importers/gltf_import_alpha_blend.RISEscene](../scenes/Tests/Importers/gltf_import_alpha_blend.RISEscene))
renders three distinct row behaviours: top row (OPAQUE) shows fully-
visible squares; middle row (MASK at cutoff 0.5) shows hard-edge
cutouts; bottom row (BLEND) shows smooth transparency.  The existing
`transparency_shaderop` wiring (post-Phase-4) was already correct;
no fix needed.

Sponza-specific note: the asset's one BLEND material is `dirt_decal`
(applied as a semi-transparent darkening overlay on floor / walls),
not a curtain.  The decal is too subtle to inspect from the canonical
hero shot at the current lighting / spp settings — the
AlphaBlendModeTest asset is the load-bearing verification.

### Goal (original)

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
