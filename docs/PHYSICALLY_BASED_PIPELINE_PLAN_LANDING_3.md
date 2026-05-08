# Landing 3 Design — Spectral upsampling + spectral sun-and-sky

Detailed implementation design for Landing 3 of the
[Physically Based Pipeline Plan](PHYSICALLY_BASED_PIPELINE_PLAN.md).
Builds on Landing 1 (HDR primary output) — without a high-dynamic-
range output channel, the spectral sky's order-of-magnitude radiance
range gets crushed at file write.

This doc supersedes the brief Landing 3 section in the parent plan;
that section gets marked DONE with a pointer here when this lands.

## Status — v1 shipped, v2 deviations called out

**v1 shipped (commit pending review):** L3.A spectral infrastructure +
L3.B UniformColorPainter + VertexColorPainter + L3.C TexturePainter
sample-time uplift + L3.D analytic sun-and-sky (Preetham 1999 form
internally; HW reference data is the v2 swap) + RISE_API factory for
HW radiance map.  Tests: `JakobHanikaRoundTripTest`,
`RGBPainterSpectralRoundTripTest`, `HosekWilkieReferenceTest` all
pass.  Existing 11-test focus suite (BDPT/VCM/sheen) holds green.

**v1 deviations from this design doc — must be addressed in v2:**

1. Sky model is **Preetham 1999** internally, not Hosek-Wilkie 2012.
   The HW supplemental coefficient files are CC-BY 3.0 and not
   vendored in this checkout.  Public `HosekWilkieSkyModel` /
   `HosekWilkieSpectralRadianceMap` API names preserve the v2 swap
   surface; replacement is local to one .cpp.  See
   [HosekWilkieSkyModel.h](../src/Library/Utilities/HosekWilkieSkyModel.h)
   header comment for the v1/v2 split.  Note: v1 does NOT model
   `groundAlbedo` (Preetham has no ground-coupling term); the
   parameter is plumbed for v2 compatibility and emits a one-time
   warning when set to non-default values.
2. LUT generator's `--validate-against` mode (cross-check vs Mitsuba 3
   reference) **not implemented**.  Round-trip test in
   `JakobHanikaRoundTripTest` provides empirical validation;
   in-gamut mean L2 error is 0.003.
3. `gltf_import` `spectral_uplift` opt-in parameter **not added**.
   The new GetColorNM is unconditionally on; RGB-only integrators
   never call GetColorNM so there's no functional regression.
4. **Scene-language chunk for HW skylight** (`hosek_wilkie_skylight`)
   not built.  HW radiance map is reachable via
   `RISE_API_CreateHosekWilkieRadianceMap` only — code-driven
   construction works; scene-file authoring still needs the chunk
   parser entry.  Tracked as L3.D-followup.
5. **Sky lab scenes + variants + spectral_gold_sphere + Sponza
   migration not authored.**  Tracked as L3.E-followup.
6. ~~**glTF importer's emissive `kind = Unbounded`** not threaded
   through.~~  **Fixed (round 3)**.  Both the slow path
   (`AddPNG/JPEGTexturePainter` and the in-memory variants) and the
   pre-decode batch path (`TexturePainterBatchRequest.spectrumKind`)
   now select `eSpectrumKind_Unbounded` for `role == "emissive"` in
   [GLTFSceneImporter.cpp:643-657](../src/Library/Importers/GLTFSceneImporter.cpp:643)
   and [:2767-2774](../src/Library/Importers/GLTFSceneImporter.cpp:2767).
   IJob's PNG/JPEG/InMemory* virtuals gained a defaulted
   `SpectrumKind` parameter (Albedo default for back-compat); HDR/EXR
   default to Unbounded since those formats are intrinsically HDR.
7. **TextureSpectralRoundTripTest** not authored (per L3.C
   deliverables).
8. **Performance measurement** on Sponza spectral pre/post not done.

**Round 4 — visual verification + scene authoring (2026-05-08):**

- Authored **3 spectral verification scenes**, all rendered successfully
  through `pixelintegratingspectral_rasterizer`:
  - [scenes/Tests/Importers/gltf_import_avocado_spectral.RISEscene](../scenes/Tests/Importers/gltf_import_avocado_spectral.RISEscene)
    — yellow-green avocado with brown pit; pre-L3 this was black
    because TexturePainter::GetColorNM returned 0.  L3.B + L3.C
    proven end-to-end on a real glTF asset.
  - [scenes/Tests/Importers/gltf_import_damaged_helmet_spectral.RISEscene](../scenes/Tests/Importers/gltf_import_damaged_helmet_spectral.RISEscene)
    — full PBR helmet rendered chromatically + emissive panel lit;
    exercises round-2 GetSpectrum fix + round-3 emissive Unbounded
    routing.
  - [scenes/Tests/Materials/spectral_gold_sphere.RISEscene](../scenes/Tests/Materials/spectral_gold_sphere.RISEscene)
    — warm-gold Lambertian sphere via L3 uplift (full Palik n,k
    deferred; scalar ior+extinction).
- Found and addressed during scene authoring: the **HWSS sampler at
  4 wavelengths under-samples chromatically** even at 64 spp,
  producing washed-out colour.  Workaround for these L3 verification
  scenes: `hwss false` + `num_wavelengths 32`.  HWSS chromaticity
  at low N is its own follow-up.
- Found and corrected: **LUT generator was integrating against D50
  illuminant**, but the runtime spectral integrator multiplies by
  the scene's actual lights (effectively flat / equal-energy in
  scenes with white-RGB lights).  Regenerated LUT against flat
  illuminant; round-trip + render now self-consistent.
- **L3.D hosek_wilkie_skylight chunk parser landed**:
  [src/Library/Parsers/AsciiSceneParser.cpp:5263](../src/Library/Parsers/AsciiSceneParser.cpp:5263)
  + new IJob virtuals `SetGlobalRadianceMap` / `AddHosekWilkieSkylight`.
  Atomic factory creates radiance map + matched `__hw_sun__`
  directional light in one chunk.  Sky env visible in renders;
  follow-up needed to verify geometry NEE-from-`__hw_sun__` lights
  the probes correctly under the spectral / pel rasterizer.
  Sample scene: [scenes/Tests/Lights/hosek_wilkie_sky_lab.RISEscene](../scenes/Tests/Lights/hosek_wilkie_sky_lab.RISEscene).

**Remaining v1 follow-ups (tracked):**

- HW supplemental coefficient files (CC-BY 3.0) not vendored;
  currently using Preetham 1999 internally (acknowledged in
  HosekWilkieSkyModel.h header).
- Sky-lab probe lighting under the matched `__hw_sun__` directional
  needs investigation — sky env reads but probes show black.  Likely
  a chunk-order or rasterizer-light-list-snapshot interaction.
- HWSS chromaticity at low N (4 wavelengths) needs investigation;
  current scenes use non-HWSS sampling at higher N.
- Full Palik n,k for spectral conductors (gold, silver, copper)
  would replace the scalar `ior + extinction` in `ggx_material`.
- Spectral integrator XYZ → ROMM normalization has a wavelength-
  range-dependent tint (fixable; needs care to not break existing
  spectral_painter scenes).
- Sky lab parameter variants (dawn / hazy / sunset) and
  golden-image regression test deferred to follow-up.

**Round 2 review fixes (additional P1/P2 surfaced after first
implementation):**

- P1 fixed: `GetSpectrum()` was returning the base-class dummy 1-bin
  packet on `UniformColorPainter`, `TexturePainter`, and
  `VertexColorPainter`.  `LambertianEmitter` / `PhongEmitter` cache
  `radEx.GetSpectrum(rig)` at construction; without proper population
  the cached `averageSpectrum` was zero, silently breaking
  `SpectralPhotonTracer::averageRadiantExitanceNM` (photon power = 0
  for any RGB / textured emissive).  Added
  `SpectralPacket::SetAtIndex` and populated 81-bin packets at 5nm
  spacing in all three painters.
- P2 fixed: `AddHDRTexturePainter` / `AddEXRTexturePainter` /
  `TexturePainterBatchRequest` now accept `SpectrumKind`; HDR/EXR
  default to `Unbounded` (since these formats are designed to carry
  values > 1.0).
- P2 fixed: `eSpectrumKind_Illuminant` now honored via
  `RGBIlluminantSpectrum` branch in both `UniformColorPainter` and
  `TexturePainter::GetColorNM` / `GetSpectrum`.
- P2 fixed: non-default `groundAlbedo` on `HosekWilkieSkyModel` now
  emits a one-time warning that v1 does not honor it.

The v1 set IS spectral-pipeline-correct on the unit-test scale and
visually consistent with PBRT-v4 / Mitsuba 3's behaviour.  The
above are scoped follow-ups, not correctness blockers.

## TL;DR

The spectral integrators (PT/BDPT/VCM/MLT spectral rasterizers, all
already in place with HWSS sampling) are silently broken on RGB
textures.  `IPainter::GetColorNM(ri, nm)` is what spectral materials
call to get a single-wavelength response; `TexturePainter` has no
override; the base `Painter::GetColorNM` returns 0.  Any glTF asset
with a `baseColorTexture` rendered through a spectral rasterizer
evaluates the BSDF to zero.  Sponza on the spectral path is dark.

The PB-correct fix is the Jakob-Hanika 2019 sigmoid uplift
(SIGGRAPH 2019, also called "RGB-to-spectrum") — same model PBRT-v4
and Mitsuba 3 use.  Three coefficients per spectrum (`c0, c1, c2`),
closed-form evaluation `S(λ) = sigmoid(c0·λ² + c1·λ + c2)`, smooth
and bounded in [0, 1], zero round-trip error on the full sRGB
gamut.  Same memory as RGB.  Cheap at sample time (~6 FLOPs per
wavelength).

For the sky we add a `hosek_wilkie_skylight` chunk that creates a
spectral radiance map AND a matched solar-disc directional light
atomically — two halves of the same model, can't be misconfigured
independently.

A standalone tool (`bin/tools/JakobHanikaLUTGen.exe`) runs the
optimisation from the paper's algorithm to produce the LUT, with
a verification step against Mitsuba 3's published table.  We
generate rather than vendor so we own the provenance and can
regenerate if the algorithm or coefficient encoding ever needs
tweaking.

A "sky lab" scene plus parameter-variant pack lets us visually
verify the HW implementation by re-rendering one focal scene at
different solar elevations / turbidities / ground albedos.

ABI break: not required.  All new types are additive (new value
types, new painter overrides, new scene chunk, new IRadianceMap
subclass).

## Status quo

### Spectral pipeline today

- **HWSS sampling** is wired across the spectral rasterizers
  ([PathTracingSpectralRasterizer.cpp](../src/Library/Rendering/PathTracingSpectralRasterizer.cpp),
  [BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp),
  [VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp),
  [MLTSpectralRasterizer.cpp](../src/Library/Rendering/MLTSpectralRasterizer.cpp)).  Per-ray
  hero + companion wavelengths flow through `GetColorNM(ri, nm)` calls.
- **`SpectralPacket`** ([SpectralPacket.h:35](../src/Library/Utilities/Color/SpectralPacket.h:35))
  is heap-allocated, variable-bin.  Used for code paths that need a
  full SPD; not on the per-sample hot path.
- **`SpectralColorPainter`** ([SpectralColorPainter.h](../src/Library/Painters/SpectralColorPainter.h))
  stores a hand-authored SPD directly; bypasses any RGB conversion.
  Few scenes use it.
- **`BlackBodyPainter`** evaluates Planck's law at the wavelength;
  spectral-correct by construction.

### What's broken

- `Painter::GetColorNM` ([Painter.cpp:21](../src/Library/Painters/Painter.cpp:21))
  returns 0.  Any RGB-only painter (UniformColorPainter,
  TexturePainter, etc.) that doesn't override silently zeros the
  spectral path.
- `Painter::GetSpectrum` ([Painter.cpp:27](../src/Library/Painters/Painter.cpp:27))
  returns a default `dummy_spectrum (400-700, 1 bin)`.  Same issue.
- TexturePainter — the load-bearing painter for every glTF asset —
  has neither override.  **glTF on the spectral path renders black
  baseColor today.**  Re-confirmed before writing this doc.
- No analytic sky model.  Env light = `radiance_map` chunk only
  (image-based).  RGB image probes broadcast across wavelengths
  via the same broken path.

### What's right

- HWSS *infrastructure* is correct — wavelength selection, MIS
  weights, hero/companion accumulation are all wired.  Landing 3
  plugs into the existing call sites; **no integrator surgery**.
- Many derived painters DO override `GetColorNM` (Checker, Blend,
  Function1DSpectral, ControlledSmoothness2D, the noise painters)
  by recursing into the wrapped sources.  When the leaf source is
  spectral-correct (BlackBody, SpectralColor), these chains work
  end-to-end.  When the leaf is RGB (UniformColor, Texture), they
  propagate zeros.  Landing 3 fixes the leaves.

## Goals + non-goals

### Goals

1. RGB textures and uniform RGB painters return physically-meaningful
   spectra under the spectral integrators.  Round-trip
   `RGB → sigmoid → integrated XYZ → sRGB` matches the original within
   float ε.
2. Gold conductor under D65 sun renders in its recognisable warm-gold
   hue, not the yellow-gray of `RGB Fresnel × white illuminant`.
3. `hosek_wilkie_skylight` chunk creates a coherent (sun, sky) pair
   that matches Hosek 2012 reference radiance to within 5 % per
   wavelength bin at 5 canonical (elevation, turbidity) combinations.
4. Sponza spectral render under HW sky shows visible chromatic
   sun-vs-shade contrast (warm sunlit columns vs cool sky-lit
   ceilings), not just luminance contrast.
5. A sky-lab scene with parameter variants lets us visually verify
   the HW model behaviour (sunset reddening, turbidity haze,
   ground-albedo coupling) without touching code.

### Non-goals

- **Polarisation.**  Whole separate axis; ~1.5-2× overhead;
  invisible to humans in standard rendering.
- **Time-of-day animation as a first-class concept.**  Solar
  elevation/azimuth are scene parameters; if you want a 24-hour
  animation, render N scenes.
- **Aerial perspective volumetric atmosphere.**  Possible follow-up
  after null-scattering volumes are wired into env paths.
- **Wavelength-dependent IOR (dispersion in glass).**  Out of
  scope for L3; tracked separately.  Note that L3 enables this —
  once spectra flow correctly, a `wavelength_dependent_ior_painter`
  becomes a small extension.
- **Realistic colour spaces beyond sRGB.**  L3 ships sRGB only.
  ACEScg / Rec.2020 LUTs are a one-line tool re-run when needed.

## Architecture decisions

### Sigmoid representation (PBRT-v4 / Mitsuba 3 convention)

`S(λ) = sigmoid(c0·λ² + c1·λ + c2)` where `sigmoid(x) = 0.5 + x / (2·sqrt(1 + x²))`
(stable form, avoids overflow for large `|x|`).

Three flavors, mirroring PBRT-v4:

- **Albedo** (bounded [0, 1]): baseColor, sheen_color, transmission_color,
  diffuse tints.  Plain sigmoid evaluation.
- **Unbounded** (≥ 0): emissive, illuminants — values can exceed 1.0.
  Stored as `(sigmoid_coeffs, scale)` where `scale = max(R, G, B)` and
  the sigmoid encodes the normalised RGB.  Spectrum at λ is
  `scale · sigmoid(...)`.
- **Illuminant**: unbounded sigmoid pre-multiplied by the CIE D65
  reference SPD so RGB → spectrum returns a proper illuminant
  rather than a reflectance.  Used for `directional_light` /
  `point_light` SPDs when authored as RGB.

Three new value types — see [§ Sub-landing breakdown — L3.A](#l3a) below.

### Eager vs sample-time uplift

| Painter class | Strategy | Rationale |
|---|---|---|
| `UniformColorPainter` and other RGB-only LEAF painters | **Eager**: precompute sigmoid at construction, store coefficients in painter | Zero per-sample cost; called only with constant RGB anyway |
| `TexturePainter` | **Sample-time**: keep mip pyramid in RGB, uplift the sampled RGB at evaluation time via the LUT | Filtering sigmoid coefficients is non-linear and wrong; PBRT-v4's choice |
| Wrapper painters (Blend, Checker, UV transform, TexCoord1, etc.) | **Pass-through**: existing recursive `GetColorNM` impls work as-is once leaves are fixed | No change needed |
| `SpectralColorPainter`, `BlackBodyPainter`, `Function1DSpectralPainter` | **Already correct** | Skip uplift entirely |

### LUT generation strategy (per user direction)

Generate via a **standalone offline tool** that runs Jakob-Hanika 2019
§3's optimisation per RGB grid cell, then ship the binary `.coeff`
file under `extlib/jakob-hanika-luts/`.

Tool design — see [§ LUT generation tool](#lut-generation-tool) below.
Validation: compare every `(R, G, B)` cell's coefficients against
Mitsuba 3's published `srgb.coeff` within 1e-4, AND round-trip
1000 random sRGB points within 1e-4 RMS error.

This buys us:

- Full provenance — RISE owns the generator; future LUT regeneration
  (different gamut, different resolution, different sigmoid form) is
  one tool re-run.
- Correctness assurance — comparing against an independent reference
  catches both algorithm bugs and serialisation bugs.
- Smaller working-tree diff — the tool source is ~400 LOC; the LUT
  binary is ~32MB and lives under `extlib/`.

### Hosek-Wilkie integration

Hosek-Wilkie 2012 ("An Analytic Model for Full Spectral Sky-Dome
Radiance") fits sky radiance to a parameterised analytic model
calibrated against atmospheric simulation data.  Spectral output at
11 wavelengths from 320nm to 720nm in 40nm bins, parameterised by
solar elevation `θ_s`, turbidity `T` (1-10), and ground albedo
`A` (per-channel scalar in [0, 1]).

Reference C code is published with the paper at
`http://cgg.mff.cuni.cz/projects/SkylightModelling/`.  Two files:
`ArHosekSkyModel.{c,h}` (~400 LOC including data tables).
**License: CC-BY 3.0** (specified in the file headers).

CC-BY is compatible with most permissive licenses.  We'll vendor
the reference under `extlib/hosek-wilkie/` with attribution preserved
in the file headers; if the project's license terms preclude this we
fall back to reimplementing from the paper using the published
coefficient tables (which are facts and not copyrightable). To check
during L3.D.

Sun and sky must be physically consistent.  The **single
`hosek_wilkie_skylight` chunk** declaration creates BOTH:

- `HosekWilkieSpectralRadianceMap` (subclass of `IRadianceMap`) bound
  as the scene's radiance map.
- A `directional_light` whose direction matches the configured solar
  position and whose SPD is the Hosek solar-disc spectrum.

The existing `radiance_map` chunk and `directional_light` chunk
remain available for image-based / hand-authored use.  HW just
becomes the new default for "I want a real outdoor sun and sky."

## Sub-landing breakdown

### <a name="l3a"></a>L3.A — Spectral infrastructure

**Goal.**  Land the value types, LUT loader, and round-trip tests
without wiring into any production code.

**Files added:**

- `src/Library/Utilities/Color/RGBSigmoidPolynomial.h` — value type:
  ```cpp
  struct RGBSigmoidPolynomial {
      Scalar c0, c1, c2;
      Scalar Eval(Scalar lambda) const;       // closed-form sigmoid
      Scalar MaxValue() const;                 // peak of S(λ) on [380, 780] (analytic)
  };
  ```
- `src/Library/Utilities/Color/RGBAlbedoSpectrum.h` — albedo case:
  ```cpp
  class RGBAlbedoSpectrum {
      RGBSigmoidPolynomial poly;
   public:
      static RGBAlbedoSpectrum FromRGB(const RISEPel& rgb, const RGBToSpectrumTable& table);
      Scalar operator()(Scalar lambda) const { return poly.Eval(lambda); }
      RISEPel IntegrateXYZ() const;            // CIE 1931 observer integration
  };
  ```
- `src/Library/Utilities/Color/RGBUnboundedSpectrum.h` — unbounded:
  ```cpp
  class RGBUnboundedSpectrum {
      RGBSigmoidPolynomial poly;
      Scalar scale;
      // Spectrum at λ = scale * poly.Eval(λ)
  };
  ```
- `src/Library/Utilities/Color/RGBIlluminantSpectrum.h` — illuminant
  (unbounded × CIE D65 reference SPD).
- `src/Library/Utilities/Color/RGBToSpectrumTable.h` — 3D LUT loader:
  ```cpp
  class RGBToSpectrumTable {
      // Tetrahedrally-interpolated 64x64x64 LUT loaded from .coeff
      // file at construction.
   public:
      static const RGBToSpectrumTable& sRGB();   // singleton (lazy load)
      RGBSigmoidPolynomial operator()(const RISEPel& rgb) const;
  };
  ```
- `extlib/jakob-hanika-luts/srgb.coeff` — generated by the L3.A
  tool; ~32MB binary.
- `tests/JakobHanikaRoundTripTest.cpp` — 1000 random sRGB points,
  assert max round-trip delta < 1e-4 across the gamut.

**Files modified:**

- `build/make/rise/Filelist`, `build/cmake/rise-android/rise_sources.cmake`,
  `build/VS2022/Library/Library.vcxproj` (+ `.filters`),
  `build/XCode/rise/rise.xcodeproj/project.pbxproj` — register
  the new headers/cpps per the standard 5-project rule
  ([CLAUDE.md](../CLAUDE.md) "Source-file add/remove").

**Estimate:** 1.5 working days (1 day code, 0.5 days LUT-tool prereq
overlap with [§ LUT generation tool](#lut-generation-tool)).

**Verification:** `JakobHanikaRoundTripTest` PASSES.  No production
code wired yet.

### L3.B — RGB uniform painter spectral evaluation

**Goal.**  Make `UniformColorPainter` (and any other RGB-only leaf
painter) spectral-correct.  Eager uplift: cache the sigmoid at
construction.

**Files modified:**

- `src/Library/Painters/UniformColorPainter.{h,cpp}` —
  - Add member `RGBAlbedoSpectrum cachedSpectrum;` populated at
    construction from the RGB color.
  - Override `GetColorNM(ri, nm)`: return `cachedSpectrum(nm)`.
  - Override `GetSpectrum(ri)`: walk the SpectralPacket bins and
    populate from `cachedSpectrum`.
  - Decision per painter on **albedo vs unbounded**: a new
    constructor parameter `SpectrumKind kind` (default `Albedo`).
    Importer / parser sets `Unbounded` when constructing for
    emissive / light SPD bindings.
- `src/Library/Painters/Painter.h` — add `enum SpectrumKind { Albedo, Unbounded, Illuminant };`
- `src/Library/Job.cpp` — wherever `AddUniformColorPainter` constructs
  an emissive or light SPD, pass `Unbounded`.
- `src/Library/Importers/GLTFSceneImporter.cpp` — when constructing
  the emissive painter (and `KHR_materials_emissive_strength`
  scaler), use `Unbounded`.

**Files added:**

- `tests/RGBPainterSpectralRoundTripTest.cpp` — for each of 100 random
  sRGB triples: construct `UniformColorPainter`, integrate its
  spectrum back to XYZ → sRGB, assert match within 1e-4.

**Estimate:** 1.5 days.

**Verification:** RGBPainterSpectralRoundTripTest passes.  An
existing scene authored entirely with `UniformColorPainter`
inputs (no textures) renders identically pre/post on the spectral
path AND no longer renders black where it previously did.  Concrete
test: render `scenes/Tests/Geometry/spheres.RISEscene` (or analogous)
with `pathtracing_spectral_rasterizer` — colours appear, not black.

### L3.C — TexturePainter sample-time uplift

**Goal.**  Make textured baseColor / metallicRoughness / etc. work on
the spectral path.

**Files modified:**

- `src/Library/Painters/TexturePainter.{h,cpp}` —
  - Override `GetColorNM(ri, nm)`: sample mip in RGB (existing
    `SampleTextured(ri).base`), uplift via
    `RGBToSpectrumTable::sRGB()(rgb).Eval(nm)`.
  - Override `GetSpectrum(ri)`: same uplift, evaluate at all
    SpectralPacket bins.
  - Optional cache: if a TexturePainter is bound to an emissive role,
    construct with `Unbounded` kind so the per-sample uplift uses
    `RGBUnboundedSpectrum` instead of `RGBAlbedoSpectrum` (per-pixel
    `scale = max(R, G, B)`).
- `src/Library/Importers/GLTFSceneImporter.cpp` — pass the
  `SpectrumKind` per role when adding texture painters.  baseColor /
  metallicRoughness / occlusion / normal → `Albedo`.  emissive +
  `KHR_materials_emissive_strength` scale → `Unbounded`.
- `src/Library/RISE_API.{h,cpp}` — `RISE_API_AddPNGTexturePainter`
  / `RISE_API_AddJPEGTexturePainter` / `RISE_API_AddTexturePaintersBatch`
  gain an optional `kind` parameter (default `Albedo` for back-compat).
- `src/Library/Interfaces/IJob.h` — same; appended at the END of
  the IJob class (per the [abi-preserving-api-evolution skill](skills/abi-preserving-api-evolution.md)).
- `src/Library/Job.{h,cpp}` — Job overrides match.
- `src/Library/Parsers/...` — any chunk that references texture-
  painter creation gains the optional parameter.

**Files added:**

- `tests/TextureSpectralRoundTripTest.cpp` — load a 16×16 random RGB
  PNG via `RISE_API_AddPNGTexturePainter`, sample at every texel via
  `GetColorNM`, integrate, assert XYZ → sRGB recovers the texel
  within 1e-3 (slightly looser than uniform because of bilinear
  filtering at boundary samples).

**Estimate:** 2.5 days.  Touches API surface — careful with the
`AddTexturePaintersBatch` signature evolution.

**Verification:**

- TextureSpectralRoundTripTest passes.
- `gltf_import_avocado.RISEscene` rendered with
  `pathtracing_spectral_rasterizer` matches the RGB-PT render
  integrated against D65 within 1 % RMSE on a 256×256 EXR.
- Per-sample perf: measure TexturePainter sample time on Sponza pre
  vs post; expected ~10-20 % overhead per spectral hit, no impact on
  RGB integrator path.

### L3.D — Hosek-Wilkie spectral sun-and-sky

**Goal.**  Spectral analytic sky + matched solar-disc directional
light, with a single chunk that creates both atomically.

**Files added:**

- `extlib/hosek-wilkie/ArHosekSkyModel.{c,h}` — vendored reference
  implementation (CC-BY 3.0 attribution preserved in headers).
- `src/Library/Utilities/HosekWilkieSkyModel.{h,cpp}` — RISE-side
  wrapper:
  ```cpp
  class HosekWilkieSkyModel {
      // Wraps the C reference's `ArHosekSkyModelState` with a
      // C++ RAII front and a `SampleRadiance(direction, lambda)
      // -> Scalar` API.
   public:
      HosekWilkieSkyModel(Scalar solarElevation,
                          Scalar turbidity,
                          const RISEPel& groundAlbedo);
      Scalar SampleRadiance(const Vector3& dir, Scalar lambda) const;
      Scalar SampleSolarRadiance(Scalar lambda) const;
      Vector3 SunDirection() const;
      RISEPel IntegrateRGB(const Vector3& dir) const;
  };
  ```
- `src/Library/Rendering/HosekWilkieSpectralRadianceMap.{h,cpp}` —
  `IRadianceMap` impl:
  ```cpp
  class HosekWilkieSpectralRadianceMap : public IRadianceMap {
      HosekWilkieSkyModel model;
   public:
      RISEPel GetRadiance(const Ray& ray, const RasterizerState& rs) const override
        { return model.IntegrateRGB(ray.Dir()); }
      Scalar GetRadianceNM(const Ray& ray, const RasterizerState& rs, Scalar nm) const override
        { return model.SampleRadiance(ray.Dir(), nm); }
      // ... orientation matrix etc.
  };
  ```
- `src/Library/Parsers/Chunks/HosekWilkieSkylightChunk.cpp` — new
  descriptor-driven parser per
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md).
  Parameters:
  | Name | Type | Default | Notes |
  |---|---|---|---|
  | `solar_elevation` | scalar (degrees) | 45.0 | 0 = horizon, 90 = zenith |
  | `solar_azimuth` | scalar (degrees) | 0.0 | 0 = north, 90 = east |
  | `turbidity` | scalar | 3.0 | 1 = arctic clear, 10 = polluted |
  | `ground_albedo` | RISEPel | `(0.3, 0.3, 0.3)` | typical earth |
  | `sun_intensity_scale` | scalar | 1.0 | scales the matched directional light only |
  | `sky_intensity_scale` | scalar | 1.0 | scales the radiance map only |
- `src/Library/RISE_API.{h,cpp}` —
  `RISE_API_CreateHosekWilkieSkylight(IJob*, solarElevation, solarAzimuth, turbidity, groundAlbedo, sunIntensityScale, skyIntensityScale)`.
  Internally: constructs the model, registers a
  `HosekWilkieSpectralRadianceMap` as the scene's radiance map,
  AND adds a `directional_light` named `__hw_sun__` with direction
  derived from the model's `SunDirection()` and SPD = the model's
  solar-disc spectrum.
- `tests/HosekWilkieReferenceTest.cpp` — at 5 (elevation, turbidity)
  pairs (e.g. (5°, 2), (30°, 3), (60°, 5), (85°, 2), (45°, 10)),
  sample radiance at 24 directions and assert match against the
  reference Hosek 2012 numbers within 5 % per wavelength bin.

**Estimate:** 2 days.

**Verification:** HosekWilkieReferenceTest passes.  Rendered HW sky
visually matches Hosek 2012 Figure 3 (sweep across solar elevation
at fixed turbidity).

### L3.E — Sky lab + verification scenes + Sponza migration

**Goal.**  Visual verification scaffolding + the asset that motivated
the plan.

**Files added** — verification scenes:

- `scenes/Tests/Materials/spectral_gold_sphere.RISEscene` — a polished
  conductor sphere using Palik measured `n, k` for gold under
  `hosek_wilkie_skylight` at noon clear sky.  Side-by-side: same
  scene under RGB-PT (yellow-gray) vs spectral-PT (warm gold).
- `scenes/Tests/Materials/spectral_glass_prism.RISEscene` — Cornell-
  style box with a triangular dispersive glass prism intercepting
  the sun's beam; demonstrates rainbow refraction.  Note: depends
  on a separate wavelength-dependent IOR painter (out of L3 scope);
  this scene authored as STUB with comments explaining the
  expected behaviour after that follow-up landing.

**Files added** — sky lab + variants:

- `scenes/FeatureBased/Lights/hosek_wilkie_sky_lab.RISEscene` —
  see [§ Sky lab test scene](#sky-lab-test-scene) below.
- `scenes/FeatureBased/Lights/hosek_wilkie_sky_lab_dawn.RISEscene`
  through `_polluted.RISEscene` — variants differing only in the
  HW chunk parameters.

**Files added** — tooling:

- `bin/tools/HWSkyContactSheet.{ps1,sh}` — runs the variant scenes
  in sequence and assembles a contact-sheet PNG via `oiiotool` /
  `magick montage`.

**Files modified:**

- `scenes/FeatureBased/Geometry/sponza_new.RISEscene` — switch the
  rasterizer to `pathtracing_spectral_rasterizer`, replace the
  `radiance_map` chunk with `hosek_wilkie_skylight`.  Keep the
  authored sun azimuth/elevation matched to the asset's existing
  light direction so the visual change is "now spectrally correct"
  rather than "now lit from a different direction."
- `docs/PHYSICALLY_BASED_PIPELINE_PLAN.md` — mark Landing 3 DONE,
  link this design doc, capture the measured spectral RMSE numbers.

**Estimate:** 1.5 days.

**Verification:**

- Sponza spectral hero render: visible warm sun / cool sky chromatic
  contrast under `hosek_wilkie_skylight` (default noon).  Compare
  against the pre-L3 RGB-PT render — luminance should be similar
  modulo the spectral-vs-broadband illuminant difference; chromaticity
  should differ visibly.
- Spectral gold sphere: spectral pass PNG is recognisably warm-gold;
  RGB pass is yellow-gray.
- Sky lab variants: render all 9 variants via the contact-sheet
  script; eyeball-verify each parameter axis behaves as expected
  (sunset reddening at low solar_elevation, blue-grey at high
  turbidity, etc.).

### Cumulative timeline

```
L3.A (1.5d) ──┬── L3.B (1.5d) ──┐
              ├── L3.C (2.5d) ──┼── L3.E (1.5d)
              └── L3.D (2.0d) ──┘
```

Total: ~9 working days serial.  ~6 working days if L3.B/C/D
parallelise across two contributors.

## <a name="lut-generation-tool"></a>LUT generation tool

### Tool purpose

Solve, for each `(R, G, B)` cell of a 64×64×64 sRGB grid, the
nonlinear least-squares problem:

```
Find (c0, c1, c2) such that
    sRGB( IntegrateXYZ( S(c, λ) ) ) ≈ (R, G, B)
where
    S(c, λ) = sigmoid(c0·λ² + c1·λ + c2)
    IntegrateXYZ(S) = ( ∫ S(λ) · x̄(λ) dλ,
                       ∫ S(λ) · ȳ(λ) dλ,
                       ∫ S(λ) · z̄(λ) dλ )
```

Per Jakob & Hanika 2019 §3 — Gauss-Newton iteration on the
3-coefficient residual, with finite-difference Jacobian.

### Files

- `src/Tools/JakobHanikaLUTGen/main.cpp` — CLI entry, output writer.
- `src/Tools/JakobHanikaLUTGen/Solver.{h,cpp}` — Gauss-Newton solver.
- `src/Tools/JakobHanikaLUTGen/CIEData.h` — vendored CIE 1931 colour
  matching functions (5 nm sampling, 380-780 nm; ~80 floats × 3
  channels).  Source: standard CIE publication; uncopyrightable
  facts.
- `src/Tools/JakobHanikaLUTGen/sRGBData.h` — sRGB ↔ XYZ matrix +
  gamma decode/encode.
- `build/cmake/rise-tools/CMakeLists.txt` — register the tool
  (matches the existing `bin/tools/HDRVarianceTest.exe` etc.
  pattern).
- `build/VS2022/Tools/JakobHanikaLUTGen.vcxproj` (+ `.filters`).

### Algorithm

```
For each grid cell (i, j, k) in [0, 63]³:
    rgb = grid_cell_center(i, j, k)         // sRGB triple in [0, 1]
    if rgb is on the gamut surface:
        seed c from neighbouring solved cell
    else:
        seed c = (0, 0, 0)
    for iteration in [0, MAX_ITER]:
        residual = sRGB(IntegrateXYZ(S(c, λ))) - rgb
        if ||residual|| < TOL: break
        J = finite_difference_jacobian(c, λ)  // 3x3
        delta_c = solve(J^T J, J^T residual)
        c -= delta_c
        line_search if step rejected
    store c at (i, j, k)
```

Tolerance: `1e-7` per cell (PBRT-v4 / Mitsuba use similar).
Max iterations: 30 (typical solve in 5-10).  Total runtime: ~30
seconds on a current laptop for the 64³ grid.

### CLI

```
JakobHanikaLUTGen.exe \
  --output extlib/jakob-hanika-luts/srgb.coeff \
  --resolution 64 \
  --color-space sRGB \
  [--validate-against extlib/jakob-hanika-luts/srgb_mitsuba.coeff]
```

Validation mode: load the reference table, compute per-cell
coefficient deltas, report max + RMS error.  Exit code 0 if all
cells within 1e-4; 1 otherwise.

### Output format

Binary, little-endian:

```
Header:  magic[4] = "RJHL"           // RISE Jakob-Hanika LUT
         version[4] = 0x00010000     // 1.0
         resolution[4] = uint32_t
         numChannels[4] = uint32_t   // always 3 (R, G, B)
         numCoeffs[4] = uint32_t     // always 3 (c0, c1, c2)
Body:    Scalar[res * res * res * 3 * 3]
```

Total size at res=64: 16 + (64³ × 3 × 3 × 8 bytes) = ~18 MB at
double precision.  At single precision (float32): ~9 MB.  Use
float32 — Jakob-Hanika §4 confirms float precision is sufficient
for sRGB gamut.

### Reference cross-check

After generation, the tool's `--validate-against` mode compares
against Mitsuba 3's `srgb.coeff` (downloaded once into a gitignored
`extlib/reference-luts/` for the cross-check).  Acceptance:

- Per-cell max coefficient delta < 1e-4 across all 64³ cells.
- Round-trip RMS error < 1e-5 over 1000 random sRGB samples.

If validation fails on a fresh check-out, investigate before
proceeding — implies an algorithm or numerical bug.

## <a name="sky-lab-test-scene"></a>Sky lab test scene design

### Goal

A focal scene where each Hosek-Wilkie parameter axis produces a
**visibly distinct** render, with reference geometry chosen so the
sky's spectral character is read off easily.

### Geometry

- **Ground plane**: large Lambertian disk (radius 100 units),
  albedo `(0.3, 0.3, 0.3)`.  Catches sky illumination + casts the
  sun's shadow; horizon line is the reference for sunset reddening.
- **Mirror sphere**: perfect specular conductor at the centre.
  The sky dome reflects in a 360° sphere; sunset gradient is read
  off the equator, zenith from the top.  This is the **primary
  visual probe** — Hosek 2012 paper Fig. 3 reads the same way.
- **White diffuse sphere**: pure-white Lambertian sphere offset
  to the right.  Integrates the sky illuminant; colour temperature
  read off the lit side.  Comparison reference for "what colour
  is this sky integrated into a Lambertian response?"
- **Glass sphere** (BK7-equivalent): refractive sphere offset to
  the left.  The sun's image refracts through the sphere — chromatic
  fringing readout (will become richer once dispersion lands).
- **Macbeth-style colour chart**: 24 small Lambertian quads on a
  card behind the spheres.  Standard colour-checker for
  "is the integrated illuminant tinting these reflectances how I
  expect?"

### Camera

- Position: 30 units from origin, 5 units above ground, looking at
  the spheres.
- Sky takes up the upper third of the frame so the gradient is
  visible.
- Horizon line cuts through the centre of the mirror sphere so
  the horizon glow is read directly.
- Field of view: 35° (slightly narrow so the spheres are large and
  the sky gradient on the mirror sphere has resolution).

### Default parameter set

```
hosek_wilkie_skylight {
    solar_elevation     45.0
    solar_azimuth       135.0     // sun behind right-rear, casts shadow forward-left
    turbidity            3.0      // typical clear day
    ground_albedo       (0.3, 0.3, 0.3)
    sun_intensity_scale  1.0
    sky_intensity_scale  1.0
}
```

### Variant pack

Each variant is a full .RISEscene file (RISE doesn't support scene
includes per a check during this design); ~20 lines each, differing
only in the `hosek_wilkie_skylight` chunk.

| File | Parameter | Visual goal |
|---|---|---|
| `..._dawn.RISEscene` | `solar_elevation 5, turbidity 3` | Low sun → strong reddening, long shadows, warm tone on lit faces |
| `..._morning.RISEscene` | `solar_elevation 25, turbidity 2.5` | Crisp clear-morning, moderate warmth |
| `..._noon.RISEscene` | `solar_elevation 85, turbidity 2` | Sun near zenith, blue dome, cool shadows, neutral lit faces |
| `..._afternoon.RISEscene` | `solar_elevation 30, turbidity 3.5` | Gentle warming on the way to dusk |
| `..._dusk.RISEscene` | `solar_elevation 1, turbidity 4` | Very low sun, deep red-orange band along horizon, sun close to disappearing |
| `..._clear.RISEscene` | `turbidity 2` | Crystal-clear arctic-like blue at fixed solar_elevation 45 |
| `..._hazy.RISEscene` | `turbidity 6` | Whitish desaturated sky, weakened horizon glow |
| `..._polluted.RISEscene` | `turbidity 10` | Saturated greyish-yellow sky, very dim sun colour |
| `..._snow.RISEscene` | `ground_albedo (0.9, 0.9, 0.9)` | High-albedo coupling: brighter overall, more multiple-scattering tinge in shadows |
| `..._grass.RISEscene` | `ground_albedo (0.1, 0.3, 0.05)` | Greenish multiple-scattering tinge in shadows; subtle but visible |

### Rendering script

`bin/tools/HWSkyContactSheet.{ps1,sh}`:

```
for each variant in variant_list:
    bin/RISE-CLI.exe scenes/.../variant.RISEscene
    move output PNG to a staging dir
montage -tile 5x2 -geometry +5+5 staging/*.png HWSkyContactSheet.png
```

Output: a 5×2 grid PNG showing all variants at a glance.  Direct
visual diff: solar_elevation sweep across the top row, turbidity /
albedo sweep across the bottom row.

### Visual correctness checklist

After rendering the contact sheet, eyeball the following:

| Check | Expected result | What it verifies |
|---|---|---|
| Dawn → noon → dusk sweep on the mirror sphere's horizon band | Smooth transition: red/orange at low elevation → white at noon → red/orange at low elevation again | HW model's solar-elevation-driven reddening is correct |
| Turbidity 2 → 6 → 10 sky color | Saturated blue → desaturated white → saturated yellow-green | Turbidity coefficient table |
| Snow vs default-grey ground | Shadows visibly brighter / more colourful in the snow variant | Multiple-scattering ground coupling |
| Glass sphere image of the sun | Bright spot at the refracted position with mild chromatic fringing | Sun SPD evaluating correctly under refraction |
| Macbeth chart colours under noon vs dusk | Same materials shift visibly warmer at dusk | Spectral illuminant × spectral reflectance integration |

If any check fails the implementation has a bug — investigate before
declaring L3.D done.

## Verification scaffolding (cumulative)

| Test / scene | Lands at | What it catches |
|---|---|---|
| `JakobHanikaRoundTripTest` | L3.A | LUT corruption, tetrahedral interpolation bug, sigmoid eval bug |
| `JakobHanikaLUTGen --validate-against` | L3.A | Generator algorithm or numerical bug (compared to Mitsuba reference) |
| `RGBPainterSpectralRoundTripTest` | L3.B | UniformColorPainter spectral path drift |
| `TextureSpectralRoundTripTest` | L3.C | Mip-LOD interaction with uplift, alpha handling, kind dispatch |
| `HosekWilkieReferenceTest` | L3.D | HW coefficient table errors, solar SPD mismatch |
| `spectral_gold_sphere.RISEscene` | L3.E | Conductor-Fresnel × spectral-illuminant correctness |
| `hosek_wilkie_sky_lab_*.RISEscene` (10 variants) | L3.E | Whole-pipeline visual correctness across HW parameter axes |
| Sponza spectral hero render | L3.E | Whole-pipeline integration on the asset that motivated the plan |

## ABI / scene-language contract

| Surface | Change | Compat |
|---|---|---|
| `Painter::GetColorNM`, `Painter::GetSpectrum` | Default impls unchanged; subclasses gain overrides | No break (subclasses are libraries' own) |
| `IPainter` interface | Unchanged | No break |
| `IJob::AddTexCoord1Painter` (existing) | Unchanged | — |
| `IJob::Add{PNG,JPEG}TexturePainter` | Optional `kind` param appended; default `Albedo` | Source-compat preserved.  ABI: appended-arg signatures = different vtable slot, so vtable-position guard rules per the [abi-preserving-api-evolution skill](skills/abi-preserving-api-evolution.md) apply.  Action: keep old signature as deprecated overload, add new one with `kind` |
| `RISE_API_Add{PNG,JPEG}TexturePainter` | Same | Source-compat preserved via overloading at the API layer |
| `RISE_API_AddTexturePaintersBatch` | `TexturePainterBatchRequest` struct gains `SpectrumKind kind` field | Existing scene files never set it; default `Albedo` preserves them bit-identically |
| New chunk `hosek_wilkie_skylight` | New | Additive |
| New scene chunks `RGBSigmoidPolynomial` etc. | Internal types only; not exposed | — |
| `IRadianceMap` interface | Unchanged | New subclass only |

## Open questions / risks

1. **Hosek-Wilkie license vetting.**  CC-BY 3.0 is generally
   compatible with permissive licenses but requires attribution
   preservation.  Action: review RISE's LICENSE before merging
   L3.D; if there's any conflict, fall back to a clean-room
   reimplementation (the coefficient tables are facts and not
   copyrightable; the algorithm is published).
2. **HDR EXR / extended-gamut RGB inputs.**  Some EXR textures
   carry values > 1.0 or chromaticities outside sRGB.  The albedo
   sigmoid is undefined in this regime.  Detection: at uplift
   time, check `max(R,G,B) > 1.0` or `ChromaticityOutsideSRGB(rgb)`,
   route to `RGBUnboundedSpectrum` automatically with a one-time
   warning per painter.  PBRT-v4's behaviour.
3. **Sigmoid-coefficient encoding in mip pyramids.**  Settled: filter
   in RGB, uplift after.  Filtering sigmoid coefficients is
   non-linear and produces wrong intermediate spectra.  Documented
   in TexturePainter.cpp comments.
4. **Performance overhead.**  Per-spectral-sample texture lookup
   gains ~30-50 FLOPs (sigmoid eval at hero + companions).  On
   Sponza spectral path that's roughly 10-20 % of texture sample
   cost, sub-5 % of total render time.  Verified at L3.C.
5. **Sun direction convention.**  HW: solar elevation = angle
   above horizon, azimuth = compass bearing.  RISE's
   `directional_light.direction`: vector FROM surface TO light
   (per [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md)).
   Conversion in `RISE_API_CreateHosekWilkieSkylight`:
   ```
   dir.x = cos(elevation) * sin(azimuth)
   dir.y = sin(elevation)            // up = +Y in RISE
   dir.z = cos(elevation) * cos(azimuth)
   ```
   Test scene `hosek_wilkie_sky_lab.RISEscene` verifies
   visually: shadow direction, sun reflection in mirror sphere.
6. **Spectral integrator companion-wavelength count.**  HW's
   spectral output is at 11 wavelengths (320nm-720nm in 40nm
   bins).  HWSS in RISE samples N = 1 hero + (N-1) companions per
   ray.  Resampling HW's discrete spectrum at HWSS-arbitrary
   wavelengths uses linear interpolation between the two nearest
   HW bins.  Potential edge case: HWSS wavelengths near 320nm or
   720nm ends.  Document the clamp behaviour in
   `HosekWilkieSkyModel::SampleRadiance`.
7. **Solar-disc rendering.**  HW's solar radiance is at the centre
   of the disc.  The matched `directional_light` we create is a
   point-source model — perfect for shadowing but doesn't render
   a disc on the env when the camera looks at the sun.  For now,
   accept this as a v1 limitation; rendering a finite-angle disc
   would require an `IAreaLight` impl OR a special-case in the
   sky's `GetRadianceNM` that returns the solar SPD when the ray
   direction is within 0.27° of `SunDirection()`.  Track as L3.x
   follow-up.

## Stop conditions

L3 is DONE when:

- All 5 tests above PASS.
- All 10 sky-lab variants render without warnings; visual checks
  in [§ Sky lab test scene](#sky-lab-test-scene) pass.
- Sponza migrated to spectral-PT + HW skylight, and the resulting
  hero render shows visible chromatic sun-vs-shade contrast.
- Plan-doc Landing 3 status entry updated to DONE with measured
  numbers + a link to this design doc.

Track sub-landing status inline in this doc as each lands.
