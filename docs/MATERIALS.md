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
a thickness, an extinction scalar painter (inline scalar or `scalar_painter`
reference — a colour painter is rejected), and per-lobe recursion budgets. The
construction merges sub-interfaces independently:

- The `BSDF` is taken from whichever of `top` / `bottom` provides one
  (top wins when both do — there's no per-lobe blend at the BSDF level).
- The `SPF` becomes a `CompositeSPF` that stochastically transmits the
  ray through the top, evaluates the bottom, and returns. Per-lobe
  recursion budgets bound the cost.
- The `Emitter` similarly composites if both layers emit.

This is the classic "varnished wood" / "skin over fat" / "metal flake
under clearcoat" idiom. `composite_material` is the general
layer-stacking primitive; PBR-style additive lobes (sheen + base)
are expressed as their own dedicated materials (`sheen_material`).

**But for a transparent film over an opaque substrate — water, oil,
lacquer, clearcoat — reach for `coated_material` (§6.1) instead.**

## 6.1 `coated_material` — a film with *coverage*, and a real layered BSDF

[`CoatedMaterial`](../src/Library/Materials/CoatedMaterial.h) +
[`CoatedBRDF`](../src/Library/Materials/CoatedBRDF.h) +
[`CoatedSPF`](../src/Library/Materials/CoatedSPF.h), with the layer
algebra in [`CoatedLayer.h`](../src/Library/Materials/CoatedLayer.h).
Full design: [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) §7.

**Why it exists rather than reusing §6.** Both existing routes to a
coated surface answer direct lighting with the *wrong* BSDF:
`composite_material` forwards one sub-material's BSDF (top wins), and
`polished_material` returns a bare `LambertianBRDF` for a
Fresnel-coated surface. So NEE, BDPT vertex connections, and MIS
denominators all saw an **uncoated** substrate.
`CoatedBRDF::value`/`valueNM` is the closed-form *combined* response,
which is the point of the material.

| parameter | pipe | notes |
|---|---|---|
| `base` | material ref | **allowlisted** — see below |
| `coat_weight` | `IScalarPainter` | [0,1] sub-pixel **coverage** fraction; the spatially varying slot |
| `coat_ior` | `IScalarPainter` | 1.33 water, 1.5 varnish; clamped to [1, 3] |
| `coat_roughness` | `IScalarPainter` | coat lobe's GGX α; clamped to [1e-3, 1] |
| `coat_thickness` | `IScalarPainter` | world length; feeds Beer–Lambert |
| `coat_absorption` | `IScalarPainter` | 1/length; **scalar pipe deliberately** (a large coefficient on the colour pipe would be clamped by the JH uplift) |
| `coat_tint` | `IPainter` | colour of one normal-incidence traversal; genuinely a colour |

Names follow the OpenPBR surface spec so a future MaterialX/OpenPBR
import is a rename rather than a redesign.

**The substrate allowlist.** `base` accepts only `lambertian_material`,
`orennayar_material`, `ggx_material`, and
`pbr_metallic_roughness_material` (which resolves to a `ggx_material`
at scene-build time), and refuses anything that emits. Two independent
reasons: the layered model needs the substrate's *directional albedo*
to run its interreflection series, which a luminaire / BSSRDF /
volumetric random walk cannot supply; and §7.5's lobe budget only
closes because the substrate's lobe count is known. Refusal happens at
**parse time** with a diagnostic naming the allowlist, in both
`Job::AddCoatedMaterial` and `RISE_API_CreateCoatedMaterial`.

**Three things worth knowing before extending it:**

1. **`coat_weight` is coverage, not gloss.** At coverage `c` the
   response is the mixture `c·(coated) + (1−c)·(bare)`, and the bare
   branch reaches the substrate through *air* — no Fresnel
   transmission, no interreflection. Both branches conserve energy, so
   partial coverage does not darken a white surface.

2. **The wet darkening is transport, not a fitted exponent.** The
   Saunderson recycling `1/(1 − r_i·R(λ))` is evaluated *per
   wavelength*, so the channels where the substrate is bright are
   amplified most — darkening and chroma boost fall out together. This
   is why there is no `substrate_wet_exponent`: an additional `R^k`
   would double-count the same physics.

3. **Anything shared by both directions must be view-independent.** The
   recycling term multiplies `f_base` symmetrically, so it reads
   `IBSDF::hemisphericalAlbedo` (whose contract forbids touching
   `ri.ray`) and *not* `IBSDF::albedo`, which is the OIDN AOV and is
   legitimately view-dependent. Using the latter made the BRDF
   non-reciprocal by ~28 % at grazing — invisible to every
   self-consistency check, which is why
   `SPFBSDFConsistencyTest`'s Part E sweeps `f(a→b)` against `f(b→a)`.

Guards: `LayeredWhiteFurnaceTest` configs 11–18 (energy, including a
red-proof pair that measures the ~45 % loss when the interreflection
term is removed), `SPFBSDFConsistencyTest` (value↔Scatter, pointwise,
and reciprocity), `SPFPdfConsistencyTest` (the real mixture PDF),
`CoatedMaterialChunkTest` (parser, allowlist, editor introspection),
and [`scenes/Tests/Materials/coated_material.RISEscene`](../scenes/Tests/Materials/coated_material.RISEscene).

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
[`CreateAllChunkParsers()`](../src/Library/Parsers/ChunkParserRegistry.cpp).
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
  ([`FresnelMode`](../src/Library/Interfaces/IMaterial.h)):
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
- `coated_material` — a transparent dielectric film over a
  **restricted** substrate, with the film's *coverage* as a first-class
  spatially varying slot (§6.1). The wetness / clearcoat / varnish /
  oil-film material.
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
     [AGENTS.md change checklist](../AGENTS.md))
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
