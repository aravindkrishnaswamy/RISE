# RISE Lights & Light Sampling

How RISE represents emitters and selects them during rendering. The
two halves of the story are complementary: the **emitter taxonomy**
(this section's first half) describes what shows up in a scene file;
the **sampling pipeline** (`LightSampler`, RIS, light BVH, environment
map) describes how the integrators choose among emitters at run time
and weight the result.

This is the orientation doc. Source of truth is the code in
[../src/Library/Lights/](../src/Library/Lights/) and the
luminaire-side glue in
[../src/Library/Rendering/LuminaryManager.h](../src/Library/Rendering/LuminaryManager.h);
this doc points at the right files and explains conventions that
aren't obvious from any single one.

## 1. Two emitter mechanisms

A scene contributes light to the integrator through **either** of two
mechanisms:

| Mechanism | Implements | Carries emission via | Tracked by |
|---|---|---|---|
| **`ILight`** ("hack lights") | [ILight.h](../src/Library/Interfaces/ILight.h) | `radiantExitance()` + `emittedRadiance(dir)` + `generateRandomPhoton()` | `IScene::GetLights()` (registered through `omni_light` / `spot_light` / `directional_light` / `ambient_light` chunks) |
| **Luminaire materials** ("mesh lights" / "area lights") | [IEmitter.h](../src/Library/Interfaces/IEmitter.h), held by `IMaterial::GetEmitter()` | The material's emitter responds to area-measure queries on its host geometry | [`LuminaryManager`](../src/Library/Rendering/LuminaryManager.h) — auto-collects every `IObject` whose material has an emitter |

The key distinction is **delta** (point in space, no spatial extent —
all `ILight` types are delta-position) versus **area** (mesh luminaire,
sampled by area). These two split the sampling pipeline as well: see
§5 below.

There is no separate "area light" chunk. Area lights are constructed
by giving an object a luminaire material
([`lambertian_luminaire_material`](../src/Library/Materials/LambertianLuminaireMaterial.h),
[`phong_luminaire_material`](../src/Library/Materials/PhongLuminaireMaterial.h)).

## 2. Catalogue of `ILight` chunks

| Chunk | Class | Role |
|---|---|---|
| `omni_light` | [`PointLight`](../src/Library/Lights/PointLight.h) | Isotropic point source. `IsPositionalLight() = true`, `emissionConeHalfAngle = π`. |
| `spot_light` | [`SpotLight`](../src/Library/Lights/SpotLight.h) | Cone-restricted point source. Reports `emissionDirection` and `emissionConeHalfAngle` so the BVH and equiangular sampler can use them. |
| `directional_light` | [`DirectionalLight`](../src/Library/Lights/DirectionalLight.h) | Infinitely distant parallel-ray source. `IsPositionalLight() = false`. **`direction` semantics**: vector pointing FROM the surface TO the light source — see [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) for the convention and the foreign-format negation rule. |
| `ambient_light` | [`AmbientLight`](../src/Library/Lights/AmbientLight.h) | Constant directionless ambient. Evaluated deterministically — does not enter stochastic light selection. |

Two consequences of these defaults that bite contributors:

- **Ambient and directional lights have zero `radiantExitance`** as a
  contractual convention — they cannot participate in
  proportional selection by exitance, so the sampler evaluates them
  outside the alias-table / BVH path. Adding a new "infinite extent"
  light type means adding it to the deterministic-evaluation list,
  not just bumping the exitance.
- **`IsPositionalLight()`** controls whether the light shows up as a
  candidate for **equiangular sampling** of participating media (volume
  scattering uses positional lights as anchor points).

## 3. The environment map is a light too

An HDR environment radiance map registered via `radiance_map` (on a
rasterizer chunk or as a per-object override) is sampled by
[`EnvironmentSampler`](../src/Library/Rendering/EnvironmentSampler.h)
— a Lambert-cylindrical 2D importance sampler over the map. It hangs
off `LightSampler::SetEnvironmentSampler` and participates in NEE
through the same evaluation path as the explicit lights.

## 4. `LuminaryManager` — the area-light registry

Every `IObject` whose material returns non-null from `GetEmitter()`
is auto-added to
[`LuminaryManager::LuminariesList`](../src/Library/Rendering/LuminaryManager.h)
during scene attach. This means:

- Adding a new luminaire is purely a material+geometry combination —
  no separate "register this as a light" call.
- Mesh-light area is whatever the geometry's surface area is.
  `LightSampler` queries `IObject::SamplePoint(...)` to get a
  uniform-area position with `pdfPos = 1/area`.
- The cosine-weighted hemisphere emission is queried via the
  emitter's profile (Lambertian luminaires give `cos/π`; Phong
  luminaires give the corresponding directional lobe).

A scene with no `ILight` chunks but a single mesh under a
`lambertian_luminaire_material` is fully lit. That is the recommended
modern setup; the four `ILight` types are retained for compactness
and historical compatibility.

## 5. `LightSampler` — the unified NEE / emission pipeline

[`LightSampler`](../src/Library/Lights/LightSampler.h) is the single
entry point for both PT and BDPT light sampling. It provides two
distinct services:

### 5.1 Direct lighting (NEE) — `EvaluateDirectLighting` / `EvaluateDirectLightingNM`

Both PT and BDPT call this to add a shadow-ray contribution at every
non-specular eye-subpath vertex. It:

1. Selects one light proportional to a configurable importance metric
   (see §5.3).
2. Samples a position on the chosen light (delta for point/spot;
   uniform-area for mesh).
3. Casts a shadow ray; on miss, evaluates BRDF · cos / pdf.
4. Computes the MIS weight against BSDF sampling (see §5.4).
5. Returns the weighted contribution.

Lights with zero exitance (ambient, directional) are handled
deterministically — they cannot enter proportional selection and are
evaluated as a separate term.

### 5.2 Emission sampling — `SampleLight`

Used by BDPT and VCM to start light subpaths. Selects a light
proportional to exitance (using the alias table; the BVH is *not*
used here because BVH selection is shading-point-conditional and a
fresh light subpath has no shading point), then samples position +
direction with explicit PDFs:

| Light type | `pdfPosition` | `pdfDirection` |
|---|---|---|
| Point / spot | 1 (delta) | uniform on sphere or cone — queried via `pdfDirection(dir)` |
| Mesh luminaire | 1/area | `cos(θ)/π` (Lambertian) or the appropriate emitter lobe |

### 5.3 Selection mode — three options

`LightSampler` supports three light-selection modes; **only one is
active at a time** and each has different MIS implications:

| Mode | Knob | When | MIS support |
|---|---|---|---|
| **Light BVH** (Conty & Kulla 2018) | `light_bvh TRUE` (default — see [LightBVH.h](../src/Library/Lights/LightBVH.h) for the construction details) | Production default. O(log N) shading-point-conditional importance with bbox + orientation cone + power. PDF is fully evaluable. | **Full MIS** with BSDF sampling. |
| **Plain alias table** | `light_bvh FALSE`, `pathguiding_ris_candidates` absent | Compact scenes (≲ 10 lights) where the per-shading-point cost of BVH traversal isn't worth it. | Full MIS via the alias-table selection PDF. |
| **RIS spatial resampling** | `light_bvh FALSE` plus `pathguiding_ris_candidates M` (only N=2 currently implemented; reserved for future expansion) | Many-light path-guiding mode. Draws M candidates from the alias table, reweights by `exitance / dist²` at the shading point, picks one. | **MIS DISABLED** (see §5.4). |

### 5.4 MIS subtleties — the failure modes that bite

Several MIS-weight gotchas live in this code; each has its own scene
test under [scenes/Tests/LightBVH/](../scenes/Tests/LightBVH/) or the
SMS / VCM test directories:

- **Delta lights need no MIS.** A point or spot light's position is a
  delta; only NEE can reach it. `w_nee = 1` and the integrator skips
  the BSDF-sampling counter-term entirely.

- **Area lights with BVH or alias table: full MIS.** The selection
  PDF is converted to solid angle measure
  (`pdfSelect · pdfPosition / G(x→y)`) and combined with the BSDF
  sampling PDF via the power heuristic. When a BSDF-sampled ray
  hits an emitter, `CachedPdfSelectLuminary` returns the same
  selection PDF for the MIS denominator — the symmetry is what
  makes the weights balance.

- **Area lights with RIS: MIS off, BSDF emission suppressed.** The
  exact finite-M RIS technique density requires marginalising over
  all M-candidate sets — not closed-form. We set `w_nee = 1` and
  suppress the BSDF-hit emitter contribution in
  `PathTracingShaderOp` to avoid double-counting. Adding RIS to a
  new code path means BOTH pieces, or you get the symptom in
  [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md):
  scenes 1.5–2× too bright in the well-lit regions, fireflies
  galore in dim regions.

- **Optimal MIS** (Kondapaneni et al. 2019) plugs in via
  `LightSampler::SetOptimalMIS`. When set and solved, NEE uses the
  variance-minimising weights from the optimal-MIS accumulator
  instead of the power heuristic. Configuration via
  `optimal_mis TRUE` on `pixelpel_rasterizer` or
  `pathtracing_pel_rasterizer` only — BDPT (pel + spectral), VCM
  (pel + spectral), PT-spectral, and MLT chunks hard-fail at parse
  time on `optimal_mis*` lines (the integrators don't consume
  `rc.pOptimalMIS`; see [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
  §2.4, §2.10 for the rationale).  Details in
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
  "MIS Weight Parameters".

### 5.5 Self-exclusion

When the shading object is itself a luminaire (e.g. an emitting
panel reflecting off its own back side), `LightSampler` excludes
that entry from selection: zero resampling weight under RIS;
rejection draw with a `(1 − p_self)` correction under plain alias.
Self-illumination from a flat or convex emitter is physically zero,
so the sample would be wasted regardless. Skipping it is purely a
variance optimisation — correctness is preserved either way.

### 5.6 Light-sample Russian roulette

`SetLightSampleRRThreshold(τ)` probabilistically terminates mesh-luminary
shadow samples whose estimated geometric contribution falls below
`τ`. Survivors are unbiased (divided by survival probability). When
`τ = 0` (default) every shadow sample is evaluated. Helpful in
geometry-heavy scenes where many small luminaires contribute
negligibly per pixel.

## 6. Tests and regression scenes

Unit tests:

- [`LightBVHTest.cpp`](../tests/LightBVHTest.cpp) — Conty-Kulla
  invariants: orientation-cone merging, importance bounds, PDF /
  sampling consistency, leaf coverage.
- [`LightExitanceTest.cpp`](../tests/LightExitanceTest.cpp) — the
  exitance / radiance contract for each `ILight` type.

Regression scenes:

- [`scenes/Tests/LightBVH/`](../scenes/Tests/LightBVH/) — alias-vs-BVH
  comparisons across light counts (corridor 20-light, 100-light;
  spotlight stage; BDPT mixed-light Cornell).
- [`scenes/Tests/UnifiedLighting/`](../scenes/Tests/UnifiedLighting/) —
  direct-light NEE correctness with mixed `ILight` and luminaire
  materials.
- [`scenes/Tests/Spectral/cornellbox_pointlight_spectral.RISEscene`](../scenes/Tests/Spectral/cornellbox_pointlight_spectral.RISEscene)
  — locks in the spectral-NEE-with-non-mesh-lights fix
  (`EvaluateDirectLightingNM` used to silently skip non-mesh lights;
  scene must render non-black).

## 7. Adding a new emitter — checklist

Pick the right shape first. **Most new emitters belong as luminaire
materials, not as new `ILight` types.** Mesh-luminaire is more
flexible (any geometry, any cosine-weighted profile, automatic area
sampling) and integrates with the BVH for free.

If you genuinely need a new `ILight` type (e.g. a sun-disc
directional, a portal-shaped distant source):

1. **Subclass `ILight`** under [src/Library/Lights/](../src/Library/Lights/).
2. **Implement** `CanGeneratePhotons`, `radiantExitance`,
   `emittedRadiance`, `position`, `generateRandomPhoton`,
   `pdfDirection`, `ComputeDirectLighting`. Override
   `IsPositionalLight`, `emissionDirection`,
   `emissionConeHalfAngle` if your light is positional / directional
   so the BVH and equiangular media sampler can use it.
3. **Decide whether your light has finite or infinite exitance.**
   Finite → enters alias-table / BVH selection. Infinite (ambient /
   directional analogue) → return zero exitance and add a
   deterministic-evaluation branch in `EvaluateDirectLighting`.
4. **Wire up parser + API + Job** the usual way
   ([src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
   "Adding A New Chunk Parser",
   [skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md)).
5. **Update the LightBVH bounding contract.** In `LightBVH::Build`,
   add a case that returns your light's `OrientationCone` (axis +
   half-angle) and AABB. Without this, the BVH cannot reason about
   your light's directional emission and the importance metric
   becomes overly conservative.
6. **Add a regression scene** under
   [scenes/Tests/UnifiedLighting/](../scenes/Tests/UnifiedLighting/)
   or [scenes/Tests/LightBVH/](../scenes/Tests/LightBVH/) and a unit
   test under [tests/](../tests/) following the
   `LightExitanceTest.cpp` pattern (assert finite exitance, sane
   `pdfDirection` integral, non-NaN `emittedRadiance`).

## 8. Cross-references

- Scene-language reference for light chunks:
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
- Light-direction sign convention, `power` semantics, foreign-format
  negation rule: [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md)
- Materials that emit (luminaires): [MATERIALS.md](MATERIALS.md) §7
- BDPT / VCM / SMS MIS-weight failure modes:
  [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)
- Conty-Kulla light BVH paper: cited inline in
  [LightBVH.h](../src/Library/Lights/LightBVH.h)
