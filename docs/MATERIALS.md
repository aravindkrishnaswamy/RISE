# RISE Materials & BSDFs

How surface scattering is modelled in RISE: what an `IMaterial` is, the
BRDF / SPF / Emitter triad pattern, the sampling protocol that lets PT,
BDPT, and VCM all share the same materials, the SSS pathways, and a
catalogue of every material chunk the parser exposes.

This is the orientation doc. Source of truth for behaviour is the code in
[../src/Library/Materials/](../src/Library/Materials/) and the interfaces
in [../src/Library/Interfaces/](../src/Library/Interfaces/); this doc
points at the right files and explains the conventions that aren't
obvious from any single one.

## 1. `IMaterial` is an aggregate, not a leaf

[IMaterial.h](../src/Library/Interfaces/IMaterial.h) is a thin wrapper
around three optional sub-interfaces:

| Sub-interface | Role | Returned by |
|---|---|---|
| [`IBSDF`](../src/Library/Interfaces/IBSDF.h) | Closed-form scattering function `f_r(ω_i, ω_o)` for **explicit evaluation** (NEE, BDPT vertex connection, MIS weights, OIDN albedo AOV). | `IMaterial::GetBSDF()` |
| [`ISPF`](../src/Library/Interfaces/ISPF.h) | Scattering Probability Function — the **importance sampler**. Produces `ScatteredRay`s with sampling PDFs, lobe types, and IOR-stack updates. | `IMaterial::GetSPF()` |
| [`IEmitter`](../src/Library/Interfaces/IEmitter.h) | Surface emission profile for luminaire materials. | `IMaterial::GetEmitter()` |

A typical material returns non-null for `BSDF` and `SPF`; luminaire materials
also return an `Emitter`; some specialised materials (perfect mirror,
perfect refractor) return `BSDF=NULL` because no continuum BRDF exists for
a delta lobe — the SPF carries the entire scattering behaviour and the
`SpecularInfo` reported through `GetSpecularInfo()` tells the integrator
to evaluate that lobe via specular cascade rather than via NEE / BSDF
sampling.

## 2. The triad pattern: `XxxMaterial` + `XxxBRDF` + `XxxSPF`

Every continuum-lobe material in [Materials/](../src/Library/Materials/)
follows the same three-file layout:

```
GGXMaterial.{h,cpp}    -- thin aggregator: holds painters, news up the BRDF & SPF
GGXBRDF.{h,cpp}        -- IBSDF implementation: closed-form value() / valueNM()
GGXSPF.{h,cpp}         -- ISPF implementation: Scatter() + Pdf() + EvaluateKrayNM()
```

Why both? They satisfy different integrator needs:

- **`Scatter`** is used when the integrator generates a continuation ray
  (path tracing, light-subpath construction, photon tracing). It emits
  a sample with its associated PDF and lobe-type tag.
- **`value`** is used when the integrator already knows both directions
  and needs the BRDF magnitude (NEE shadow rays, BDPT connection terms,
  MIS denominator computation). The same material must answer both
  consistently for MIS to balance.

[`SPFBSDFConsistencyTest.cpp`](../tests/SPFBSDFConsistencyTest.cpp) and
[`SPFPdfConsistencyTest.cpp`](../tests/SPFPdfConsistencyTest.cpp) lock in
the contract: a sampled direction's BRDF (via the BRDF) and its sampling
PDF (via the SPF) must agree with each other.

## 3. Spectral variants: every method has an `NM` form

Spectral rasterizers (`bdpt_spectral_rasterizer`, `vcm_spectral_rasterizer`,
`pixelintegratingspectral_rasterizer`, `pathtracing_spectral_rasterizer`,
`mlt_spectral_rasterizer`) call the wavelength-tagged variants
`valueNM(...)` / `ScatterNM(...)` / `PdfNM(...)` / `GetSpecularInfoNM(...)` /
`GetRandomWalkSSSParamsNM(...)`. RGB rasterizers call the un-suffixed
form. The default implementation of `*NM` typically delegates to the
RGB form for materials that don't disperse (everything except
`DielectricMaterial`'s wavelength-dependent IOR and `BioSpecSkin`'s
spectral RW SSS).

When adding a new BSDF, implement both forms. If the lobe is achromatic,
the `NM` form is one line that delegates.

## 4. Delta vs. continuum lobes — `SpecularInfo`

`IMaterial::GetSpecularInfo` reports whether the surface scatters via a
delta distribution (perfect mirror / refraction) and, if so, the IOR at
the interface. Three integrator subsystems read this:

1. **MIS weight machinery** (`MISWeight`, `EvaluateNEE`, `EvaluateMerges`):
   delta lobes contribute zero to NEE and zero to merge density, so the
   numerator and denominator must skip them. Getting this wrong produces
   the "delta-position-light vs delta-surface-scatter" trap captured in
   [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md).
2. **SMS solver** (`ManifoldSolver`): builds a chain of specular vertices
   between an eye vertex and a light, using `SpecularInfo` to decide
   constraint type at each vertex (reflection / refraction / TIR).
3. **PT firefly suppression**: a path that has passed through specular
   AND landed on non-specular shading must suppress emission to avoid
   double-counting the SMS / connect strategies.

The default implementation returns `SpecularInfo()` (non-specular). Only
`DielectricMaterial`, `PerfectReflectorMaterial`, `PerfectRefractorMaterial`,
and (selectively) `PolishedMaterial` override.

## 5. Subsurface scattering: three pathways, one material flag set

A single material can route SSS through three different transports
depending on which method it overrides:

| Pathway | Trigger | Where the work happens | Use when |
|---|---|---|---|
| **Diffusion-profile BSSRDF** | `GetDiffusionProfile()` returns non-null | Integrator does importance-sampled disk-projection probes; `ISubSurfaceDiffusionProfile::Rd(r)` weights each entry. | Skin, marble, jade — slabs where the diffusion approximation is accurate. |
| **Random-walk SSS** | `GetRandomWalkSSSParams()` returns non-null (RGB) and/or `GetRandomWalkSSSParamsNM` returns true (spectral) | Integrator traces a volumetric random walk inside the mesh using `sigma_a` / `sigma_s` / HG `g`. `IsVolumetric()` returns true so BDPT uses `kray` for throughput instead of `BSDF·cos/pdf`. | Highly scattering media, dispersive skin (`BioSpecSkin` overrides only the NM form so RGB renderers fall back to the diffusion path), arbitrary non-flat geometry. |
| **Empty SPF + Composite outer layer** | A material with an `IBSDF` only, layered under a transmissive top via `composite_material`. | The composite layered model (§6) handles the boundary; the inner material need only provide the diffuse interior. | Layered surfaces where SSS is one component of a stack. |

The two SSS modes are **mutually exclusive**: a material picks one. The
companion test for energy conservation is the
[BSSRDFFurnace](../scenes/Tests/BSSRDFFurnace/) scene set; the unit test
is [`BSSRDFSamplingTest.cpp`](../tests/BSSRDFSamplingTest.cpp).

## 6. Layered materials via `composite_material`

[`CompositeMaterial`](../src/Library/Materials/CompositeMaterial.h)
synthesises a single `IMaterial` from a `top` and `bottom` material plus
a thickness, an extinction painter, and per-lobe recursion budgets. The
construction merges sub-interfaces independently:

- The `BSDF` is taken from whichever of `top` / `bottom` provides one
  (top wins when both do — there's no per-lobe blend at the BSDF level).
- The `SPF` becomes a `CompositeSPF` that stochastically transmits the
  ray through the top, evaluates the bottom, and returns. Per-lobe
  recursion budgets bound the cost.
- The `Emitter` similarly composites if both layers emit.

This is the classic "varnished wood" / "skin over fat" / "metal flake
under clearcoat" idiom. For most users `composite_material` is the
right layer-stacking primitive; PBR-style additive lobes (sheen + base)
are expressed as their own dedicated materials (`sheen_material`).

## 7. Luminaires — materials that emit

Luminaire materials add an `IEmitter` to the triad:

- [`LambertianLuminaireMaterial`](../src/Library/Materials/LambertianLuminaireMaterial.h)
  — area emission from a (typically Lambertian) BRDF base.
- [`PhongLuminaireMaterial`](../src/Library/Materials/PhongLuminaireMaterial.h)
  — directional Phong-lobe emission.

Both work in tandem with `arealight_shaderop` and the area-light NEE
path. A surface is a luminaire iff its material returns non-null from
`GetEmitter()`; nothing else is special about the geometry. The
[SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) section on `power` covers
the unit story.

## 8. Material catalogue (parser keywords)

These are the 25 material chunks registered in
[`CreateAllChunkParsers()`](../src/Library/Parsers/AsciiSceneParser.cpp).
Grouped by primary lobe shape — see the corresponding `*Material.h`
file for parameter-by-parameter behaviour.

**Ideal / delta lobes** (`SpecularInfo` non-trivial):
- `perfectreflector_material` — perfect mirror (delta reflection).
- `perfectrefractor_material` — perfect refraction with Fresnel split.
- `dielectric_material` — full Fresnel dielectric with optional
  wavelength-dependent IOR for dispersion.
- `polished_material` — Lambertian base with a delta specular layer
  weighted by Fresnel.

**Lambertian / matte:**
- `lambertian_material` — diffuse only.
- `orennayar_material` — rough-diffuse Oren-Nayar generalisation.

**Microfacet:**
- `ggx_material` — anisotropic GGX with two Fresnel modes
  ([`FresnelMode`](../src/Library/Interfaces/IMaterial.h:49)):
  `eFresnelConductor` (default; multiplies real `ior`/`ext` Fresnel by a
  `specular` painter as a tint) and `eFresnelSchlickF0` (treats `specular`
  as F0; required by glTF metallicRoughness).
- `pbr_metallic_roughness_material` — glTF-spec composition. Not its own
  material class; resolved at scene-build time in
  [`Job::AddPBRMetallicRoughnessMaterial`](../src/Library/Job.cpp) into
  a painter graph + a single `ggx_material` in `eFresnelSchlickF0` mode.
- `cooktorrance_material` — Cook-Torrance microfacet.
- `ward_isotropic_material`, `ward_anisotropic_material` — Ward.
- `ashikminshirley_anisotropicphong_material` — Ashikhmin-Shirley.
- `isotropic_phong_material` — classic Phong.

**Layered / additive lobes:**
- `composite_material` — top/bottom layered composition (§6).
- `sheen_material` — Charlie-style sheen lobe (intended to layer over a
  base via `composite_material`).
- `schlick_material` — Schlick approximation as a standalone material.

**Subsurface scattering:**
- `subsurfacescattering_material` — diffusion-profile BSSRDF (§5).
- `randomwalk_sss_material` — random-walk volumetric SSS (§5).
- `donner_jensen_skin_bssrdf_material` — Donner-Jensen multi-layer skin
  diffusion profile.
- `biospec_skin_material` — biophysically-based skin, **spectral-only**
  random walk (RGB renderers fall back to the diffusion variant via the
  `GetRandomWalkSSSParamsNM` override pattern from §3 / §5).
- `generic_human_tissue_material` — bulk-tissue scattering coefficients
  for human-body rendering.

**Translucency:**
- `translucent_material` — thin translucent surfaces (paper, leaves).

**Luminaires** (§7):
- `lambertian_luminaire_material`
- `phong_luminaire_material`

**Data-driven:**
- `datadriven_material` — measured BRDF (MERL / Matusik).

## 9. Adding a new BSDF — checklist

1. **Pick the right base.** If the new lobe layers on top of an existing
   material, it's a `*_material` whose constructor accepts a base
   material reference and is stacked via `composite_material`. If it's a
   standalone closed-form lobe, it's a fresh triad
   (`XxxMaterial` + `XxxBRDF` + `XxxSPF`).
2. **Implement the triad.** `XxxBRDF::value` and `XxxBRDF::valueNM` for
   evaluation; `XxxSPF::Scatter` and `XxxSPF::ScatterNM` for sampling;
   `XxxSPF::Pdf` and `XxxSPF::PdfNM` for MIS — the consistency tests
   ([`SPFBSDFConsistencyTest`](../tests/SPFBSDFConsistencyTest.cpp),
   [`SPFPdfConsistencyTest`](../tests/SPFPdfConsistencyTest.cpp))
   will catch a mismatched pair.
3. **Override `GetSpecularInfo` / `GetSpecularInfoNM`** if the lobe is
   delta, so the integrators handle it correctly (§4).
4. **Override `albedo`** with a closed-form directional-hemispherical
   reflectance estimate so OIDN's albedo AOV is noise-free.
5. **Override `IsVolumetric`** if the SPF embeds Beer-Lambert attenuation
   in `kray` (random-walk SSS, participating media bounded by surfaces).
6. **Wire up parser + API + Job:**
   - New `RISE_API_AddXxxMaterial` (preserve ABI;
     [skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md))
   - New `Job::AddXxxMaterial` and `IJob::AddXxxMaterial`
   - New `XxxMaterialAsciiChunkParser` registered via `add(...)` in
     `CreateAllChunkParsers()` ([Parsers/README.md](../src/Library/Parsers/README.md)
     §"Adding A New Chunk Parser")
   - All five build projects updated ([../CLAUDE.md](../CLAUDE.md) §
     "Source-file add/remove — touch ALL five build projects")
7. **Add a test** under [tests/](../tests/) that exercises the closed
   form against a known reference (white furnace, energy conservation,
   reciprocity) and add a regression scene under
   [scenes/Tests/Materials/](../scenes/Tests/Materials/).

## 10. Cross-references

- Scene-language reference for material chunks:
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
- Scene-authoring traps (light direction, `power` units, transform
  precedence, alpha-mask gotchas):
  [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md)
- Subsurface-scattering math (BSSRDF profiles, RW absorption boundary):
  source in [Materials/BurleyNormalizedDiffusionProfile.h](../src/Library/Materials/BurleyNormalizedDiffusionProfile.h),
  [Materials/MultipoleDiffusion.h](../src/Library/Materials/MultipoleDiffusion.h),
  [Materials/DonnerJensenSkinDiffusionProfile.h](../src/Library/Materials/DonnerJensenSkinDiffusionProfile.h).
- glTF PBR material composition: [GLTF_IMPORT.md](GLTF_IMPORT.md) §4.
- BDPT vertex protocol (medium vs surface vertices, area-measure PDF
  Jacobians) for materials with `IsVolumetric() = true`:
  [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h).
