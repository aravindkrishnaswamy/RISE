# Legacy deprecation assessment

Assessment only, 2026-10-09, on branch `legacy-assessment` from `master` `f849269ef`.
Nothing in `src/`, `scenes/`, `tests/` or the build files was changed. This document
lists every user-facing feature family, says which parts are legacy, and proposes how
"deprecated and not supported" should work. The owner makes the final calls; the
numbered list at the end is set up for item-by-item replies.

**Owner's goal (verbatim):** "RISE today contains state of the art materials and
rendering modes and also a bunch of legacy stuff. I want to mark the legacy stuff as
legacy and deprecate and not support them so that we can focus on the state of the art."

## How the numbers were counted

Every count below comes from one of these commands, run in this worktree (clean, so the
working tree equals the tracked files):

- **Scene usage** (Tests / FB / other): the number of scene files with a chunk-header
  line, `grep -rlE "^[[:space:]]*KEYWORD([[:space:]]|$)" scenes/<dir> --include='*.RISEscene' | wc -l`,
  for `scenes/Tests`, `scenes/FeatureBased`, and "other" (`scenes/Benchmarks`,
  `scenes/Templates`, `scenes/pr.RISEscene`). There are 466 tracked scene files.
  `scenes/Tests/ChunkCoverage/cc_*` files exist only to cover a parser, so a count of
  "1 (coverage)" means no real scene uses the chunk.
- **tests/**: the number of files under `tests/` containing the keyword as a word
  (`grep -rlw KEYWORD tests | wc -l`). When a row uses a family pattern instead of one
  keyword, the pattern is named in that row.
- **Other surfaces**: `git ls-files <surface> | xargs grep -lw KEYWORD | wc -l` over
  `skills/agent`, `src/Library/Agent`+`SceneEditor*`, `evals/{scenarios,runconfigs,fixtures}`,
  `src/Blender`, the Mac GUI (`build/XCode/rise`), the Windows GUI
  (`build/VS2022/RISE-GUI`), Android (`android/app/src`), and `src/Library/Importers`.
- **Ledger rows**: the "open rows" column lists rows in `docs/DEBT_LEDGER.md` that are
  not struck through (`grep '^| DL-[0-9]'`, 26 rows today), read one by one.
  "Historical footprint" counts struck and open rows whose text mentions the family's
  class or chunk names (`grep -E '^\| (~~)?DL-[0-9]+' | grep -E '<pattern>'`). These
  mention counts are approximate: a row that names a class in passing still counts.
- **Code size**: `cat <files> | wc -l`, as a rough measure only.

Class key: **SOTA** = state of the art, keep and support. **LEGACY** = a modern
replacement exists. **LEGACY-NR** = legacy with no replacement, so removing it loses a
capability. **INFRA** = infrastructure, not a rendering choice.

Recommendation key: **keep** (SOTA); **deprecate-warn** (parses and renders as before,
with a warning — the existing DL-323 mechanism); **freeze** (deprecate-warn, plus no
further debt work: bugs are won't-fix); **remove** (after migrating the scenes);
**keep-for-now** (needs an owner call, usually because there is no replacement).

## 1. One-page summary

| # | Item | Class | Replacement | Scene usage (Tests/FB/other) | tests/ files | Open ledger rows | Recommendation | Owner call? |
|---|---|---|---|---|---|---|---|---|
| R1 | `pixelpel_rasterizer` (shader-op pipeline, RGB) | LEGACY (partly NR) | `pathtracing_pel_rasterizer`; direct-only use needs a decision (D3) | 111/25/1 | 37 | DL-451, DL-465(1) | freeze, then migrate, then remove the chunk | **yes** (D1-D3) |
| R2 | `pixelintegratingspectral_rasterizer` | LEGACY | `pathtracing_spectral_rasterizer` | 20/0/0 | 11 | — | freeze (already "soft-deprecated" in docs) → remove | light |
| R3 | Legacy chain ops: `distributiontracing_`, `finalgather_`, `directlighting_`, `arealight_` shaderops and the built-in Default{Reflection,Refraction,Emission,DirectLighting} ops | LEGACY | pure integrators | DT 5/4, FG 1/3, DL 1/2, AL 1/0; 82 scenes are direct-lighting-only chains | 25 (family) | DL-451 | freeze → remove together with R1 | via D1 |
| R4 | `pathtracing_shaderop` (+ alias `mis_pathtracing_shaderop`) | INFRA today | — | 28/1 (alias 1 coverage) | 6 | — | keep until PT's SSS continuation stops shading through it; remove the alias | yes (D13) |
| R5 | Photon maps + gathers (caustic/global/translucent/shadow, Pel/Spectral), `irradiance_cache`, `finalgather_shaderop` | LEGACY | VCM (caustics), PT/BDPT + OIDN (GI) | 26 scenes in total (7 FB showcases) | 29 | none open | freeze → migrate → remove | **yes** (D4) |
| R6 | Point-cloud SSS shader ops: `simple_sss_`, `diffusion_approximation_sss_`, `donner_jensen_skin_sss_shaderop` | LEGACY | `subsurfacescattering_material` / `randomwalk_sss_material` under PT | 9 scenes in total (simple 6/3, DA 1/1, DJ 0/0) | 4 | — | freeze → remove | light |
| R7 | `ambientocclusion_shaderop` | LEGACY-NR | none as a render mode (draft preview AO; the `occlusion` painter signal) | 2/2 | 2 | — | keep-for-now or freeze | **yes** (D6) |
| R8 | `alpha_test_shaderop`, `transparency_shaderop` | LEGACY | material `alpha_coverage` (DL-214), honoured by every integrator | 1 (coverage) + 1/0 | 4 | — | deprecate-warn → remove | light |
| R9 | `sms_shaderop` | LEGACY | `pathtracing_*_rasterizer` `sms_*` parameters | 1 (coverage) | 0 | DL-451 | freeze → remove | no |
| R10 | `directvolumerendering_shader`, `spectraldirectvolumerendering_shader` (medical volume rendering) | LEGACY-NR | none | 0/1 (`visiblehuman`) | 2 / 1 | — | keep-for-now (freeze) | **yes** (D7) |
| R11 | `ambient_light` | LEGACY | uniform environment `radiance_map` | 52/2/0 | 10 | none (historical DL-207) | deprecate-warn → migrate → remove | **yes** (D5, look change) |
| R12 | `mlt_rasterizer`, `mlt_spectral_rasterizer` | LEGACY (declared so in `UNIFIED_INTEGRATOR_DECISION.md`, not in code) | BDPT / VCM / auto | 15 scenes in total (6/6 + 3/0) | 30 | DL-465, DL-475, DL-481..483 (shared with BDPT) | freeze → remove | **yes** (D8) |
| R13 | 7 already-deprecated materials (Cook-Torrance, Phong, Ashikhmin-Shirley, Schlick, Ward ×2, Polished) | LEGACY | `ggx_material` / `coated_material` (§11.5 table) | 35 scenes in total | 53 (family) | DL-481 (shared), DL-339(a) (polished) | move from deprecate-warn to freeze → migrate → remove | **yes** (D9) |
| R14 | `iridescent_painter` (view-angle colour lookalike) | LEGACY | `ggx_material` thin-film mode | 4/6/3 | 3 | — | deprecate-warn | yes (look change) |
| R15 | `composite_material` | SOTA-capable, high debt | `coated_material` covers most layered looks | 1/0/0 | 15 | DL-221, DL-407, DL-472 | keep or freeze | **yes** (D10) |
| R16 | `translucent_material` | LEGACY-NR (no thin-sheet replacement) | none | 8/4 | 16 | DL-472 (via composite), DL-481 (shared) | keep | yes (D11) |
| R17 | `phong_luminaire_material` | LEGACY-NR | none (cos^N emission profile) | 1 (coverage) | 7 | — | freeze | light |
| R18 | `biospec_skin_material`, `generic_human_tissue_material` | specialised research models | none | 2/0, 1 (coverage) | 8 / 5 | none open (historical DL-126, DL-183) | keep-for-now | **yes** (D12) |
| R19 | `datadriven_material` (MERL) | SOTA-adjacent (measured) | — | 1 (coverage) | 11 | — | keep | no |
| R20 | `subsurfacescattering_material` (diffusion profile), `donner_jensen_skin_bssrdf_material` | SOTA-adjacent | `randomwalk_sss_material` is the other model, not a superset | 8/1; 0/0 | 25; 2 | DL-408(1), DL-482 | keep both | light |
| R21 | `rawmesh_geometry` (only producer of the non-indexed `TriangleMeshGeometry`) | INFRA, legacy class | same file format loaded into `TriangleMeshGeometryIndexed` | 37/0/1 | 0 | DL-382(1) | keep the chunk, retire the class | yes (D14) |
| R22 | `3dsmesh_geometry` | LEGACY | `plymesh_geometry`, `gltf_import` | 1 (coverage) | 0 | — | deprecate-warn → remove | no |
| R23 | `bezierpatch_geometry` | SOTA-adjacent (classic teapot) | — | 7/6 | 3 | DL-382(1) | keep | no |
| R24 | `onb_pinhole_camera` | LEGACY (alternative parametrisation) | `pinhole_camera` | 1 (coverage) | 2 | — | deprecate-warn → remove | no |
| R25 | `standard_shader` / `advanced_shader` / `defaultshader` | INFRA | — | 357/66/6 and 10/1 | 145 / 9 | — | keep; becomes vestigial in Phase 3 | via D13 |
| R26 | Legacy front-ends and build files: `src/3DSMax`, `src/PRISE`, `src/DRISE`, `build/VS2005`, `Config.SGI*`/`Config.Solaris` | INFRA, legacy | — | n/a | n/a | — | remove 3DSMax/VS2005/SGI/Solaris; DRISE/PRISE need a call | **yes** (D15) |
| R27 | Lights: `omni_light`, `spot_light`, `directional_light` | SOTA (standard delta lights) | — | 65/14/3, 6/10/2, 80/20/3 | 75/16/31 | DL-465(3) | keep | no |
| R28 | Pure integrators: PT, BDPT, VCM, auto (Pel + Spectral/HWSS), `rect_light`, `shape_light`, `hosek_wilkie_skylight`, media, modern materials, painters | SOTA | — | — | — | the rest of the open rows | keep | no |

"in total" counts are the union of the chunks in that row. Five families together —
R1/R2 (legacy rasterizers), R5 (photon), R13 (deprecated materials), R11 (ambient), and
R6 (point-cloud SSS) — appear in **198 of the 466 tracked scenes** (163 Tests,
34 FeatureBased, 1 `pr.RISEscene`). **154** scenes declare **only** a legacy rasterizer
and no pure-integrator rasterizer.

## 2. Rasterizers and the shader-op pipeline

### 2.1 What the legacy pipeline is

`pixelpel_rasterizer` and `pixelintegratingspectral_rasterizer` drive a `RayCaster` that
runs the hit object's shader (`standard_shader` / `advanced_shader`), which is a chain of
`IShaderOp`s ([RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §1.1). The pure
integrators (PT/BDPT/VCM/MLT) never call `SelectShader` or `Shade` in their own loops.
The only `pShader`/`SelectShader` users in `src/Library/Rendering` are `RayCaster.cpp`
and the interactive viewport's private shaders.

**Coupling that matters for removal (verified by reading the code):**

1. **The PT rasterizers are subclasses of the legacy classes.**
   `PathTracingPelRasterizer : PixelBasedPelRasterizer`,
   `PathTracingSpectralRasterizer : PixelBasedSpectralIntegratingRasterizer`, and
   `InteractivePelRasterizer : PixelBasedPelRasterizer`. Removing the legacy pipeline
   therefore means removing **chunks and the shader-dispatch branch**, not the
   `PixelBased*` classes. Those classes stay as PT's base (about 5.5k lines).
2. **PT still shades through the shader chain in one place.** `Job::SetPathTracingPelRasterizer`
   builds its `RayCaster` around the scene's `defaultshader`. PT's BSSRDF and random-walk
   continuations call `caster.CastRay` (`PTCastRay`, `PathTracingIntegrator.cpp` ~1453),
   and `CastRay` shades the exit hit through `SelectShader`, then
   `PathTracingShaderOp` → `IntegrateFromHit`. This is why almost every modern scene
   still declares `standard_shader { name global; shaderop DefaultPathTracing }`
   (357 Tests / 66 FB declare `standard_shader`). `pathtracing_shaderop` and the
   shader-dispatch path are load-bearing **infrastructure** until that continuation is
   moved to call the integrator directly (D13).
3. Legacy photon maps reach into modern code: DL-05's delta pass-through shadow rays
   are suppressed "while a radiance photon map exists", and `IScene` carries
   `IIrradianceCache`. The VCM light-vertex store and `SMSPhotonMap` are **separate
   clones** of the kd-tree (`VCMLightVertexStore.cpp` header, `SMSPhoton.h`), so removing
   `src/Library/PhotonMapping/` does not affect VCM or SMS.

### 2.2 How shipped legacy-rasterizer scenes are used

The shader-op sets of the 157 scenes that declare a legacy rasterizer (op chunk types
plus `Default*` op names in each file):

| Chain | Scenes | What it is | Migration target |
|---|---|---|---|
| `DefaultDirectLighting` only | 82 | direct lighting, no GI (mostly `Tests/Geometry`, `Painters`, `Cameras`) | **No exact replacement** (D3): PT with `max_diffuse_bounce 0` adds specular paths and MIS; or keep a direct-only mode |
| `pathtracing_shaderop` (alone or with others) | 24 + 5 | legacy PT | `pathtracing_*_rasterizer` (set `oidn_denoise FALSE` to keep the look: the PT default is `true`, `RasterizerDefaults.h:108`) |
| Default{Reflection,Refraction}+DL (± Emission) | 11 | Whitted ray tracing | PT (look change: adds GI) |
| photon-map / final-gather / irradiance-cache chains | 15 | see §3 | VCM or PT |
| point-cloud SSS chains | 9 | see §4.4 | PT + SSS material |
| `distributiontracing_shaderop` chains | 9 | distribution ray tracing | PT |
| AO / area-light / transparency chains | 5 | see R7/R8 | case by case |

### 2.3 Items

**R1 `pixelpel_rasterizer`.** *Usage:* 111 Tests (33 in `Tests/Geometry`), 25 FB, 1 other;
37 test files; 3 of the 8 Android-bundled scenes (`Tests/Geometry/shapes`,
`FeatureBased/Parser/kaleidoscope_atrium`, `FeatureBased/Combined/tidepools`); the
CLAUDE.md/README quickstart sample render (`scenes/Tests/Geometry/shapes.RISEscene`); the
Blender add-on enum item `0 "Pixel Rasterizer — Legacy shader-op stack, no PT — fastest
preview"` (`properties.py`; the default is Auto). The agent leaves it ungated "because it
is REQUIRED for alpha-mask scenes" (`AgentSession.cpp` ~2086,
`skills/agent/scene-skeleton-and-conventions.md` ~180). **That reason has been stale
since DL-214**: material alpha coverage now works in every integrator, and both importers
now call `SetMaterialAlpha`. *Debt:* open DL-451 (HWSS shader-op path lacks extended SMS)
and DL-465(1) (motion blur single-threaded "for PT, BDPT, MLT and the legacy
rasterizers"). Historically, 37 ledger rows mention the pipeline (DL-171/209 legacy-chain
MIS, DL-26, DL-315, DL-185, ...). Its own docs already say "never use a legacy `pixelpel_*`
render as a measurement reference" (RENDERING_INTEGRATORS §7). *Removal breaks:* direct-only
preview renders, custom op-chain composition, the Blender "Pixel Rasterizer" option, the
quickstart sample, and Android's catalogue. *Migration:* automatic for the PT-chain and
Whitted scenes (with a look change from GI and OIDN); direct-only scenes depend on D3.
*Recommendation:* **freeze** in Phase 1, migrate in Phase 2, remove the chunk in Phase 3.
Keep the `PixelBased*` classes.

**R2 `pixelintegratingspectral_rasterizer`.** Already marked "Soft-deprecated ... no removal
date" in RENDERING_INTEGRATORS.md §3 and §4 (no guiding, adaptive sampling, optimal MIS
or inline AOV). 20 Tests scenes (15 in `Tests/Spectral`), 0 FB, 11 test files.
`Job.cpp` ~9765 also builds `PixelBasedSpectralIntegratingRasterizerRGB` through it.
*Recommendation:* **freeze → remove**, moving the 20 scenes to `pathtracing_spectral_rasterizer`.

**R3 legacy chain ops.** `distributiontracing_shaderop` (5/4: `kaleidoscope_atrium`, `pillow`,
`spotlight_drama`, `showroom`, `blurry_glass`, ...), `finalgather_shaderop` (§3),
`directlighting_shaderop` (1/2), `arealight_shaderop` (1/0, `Tests/Shaders/arealight_shaderop`;
replaced by `rect_light` or luminaire NEE), and the always-registered
`DefaultReflection/Refraction/Emission/DirectLighting` ops (`Job.cpp` 679-689). These have
no meaning outside R1/R2, so they share R1's fate. About 5.1k lines including the photon
shader ops. Note the dead helper: `RISE::Utilities::WireAlphaAdvancedShader`
(`AdvancedShaderWiring.h`) has no remaining caller, since both importers moved to
`SetMaterialAlpha`.

**R4 `pathtracing_shaderop`.** INFRA until D13 is done (see §2.1 item 2). Remove the
`mis_pathtracing_shaderop` alias (registered as "Legacy alias", 1 Tests scene) with a
one-line migrator.

**R25 shaders.** `standard_shader`/`advanced_shader` stay. After Phase 3 plus D13,
`defaultshader` on the pure rasterizers could become optional, so modern scenes stop
carrying the vestigial `global` shader.

## 3. Photon mapping, final gather, irradiance cache (R5)

*Chunks:* `caustic_pel_photonmap` (7/4), `caustic_spectral_photonmap` (1/0),
`global_pel_photonmap` (1/5), `global_spectral_photonmap`, `translucent_pel_photonmap`,
`shadow_photonmap` (each 1, coverage only), their six `*_gather` chunks, `irradiance_cache`
(3/5), `finalgather_shaderop` (1/3). 26 scenes in total. The FB showcases that would change
look: `Caustics/pool_caustics`, `Animation/caustic_animation`, `Parser/photon_cloister`,
`Combined/crystal_lens`, `Combined/gi_spheres`, `GlobalIllumination/irradiance_cache_torture`,
`Shaders/SSS/sss_gi_dragon`; `showroom` and `tidepools` use `irradiance_cache` (with AO).
29 test files use them. IJob: 26 virtuals in `tests/IJobVtableManifest.txt` match
`PhotonMap|IrradianceCache|Gather`. Code: `src/Library/PhotonMapping/` about 5.5k lines.

*SOTA replacement:* VCM merges photons under one MIS umbrella and is "the only integrator
reaching SDS / dielectric-caustic transport" (RENDERING_INTEGRATORS §2). PT+SMS covers
point-light caustics. For GI, PT/BDPT with OIDN replace final gather and the irradiance
cache. *Debt:* no open rows; historical footprint is 22 rows (DL-39 translucent tracer,
DL-239 gather, DL-292 graded-index photons, DL-298/431 emitter records, DL-315, DL-320, ...).
Every recent transport fix has had to touch the photon tracers as a sibling. *Capability
loss:* biased fast GI previews and a saved photon map (`Save/Load*PhotonMap` in `Job.cpp`).
Neither is needed for state-of-the-art output.

*Recommendation:* **freeze now**; in Phase 2 convert caustic scenes to `vcm_pel_rasterizer`
and GI scenes to PT (each FB showcase reviewed by eye); **remove** in Phase 3. This is the
largest single cut in maintenance (about 11k lines with R3) and is cleanly separable from
VCM and SMS.

## 4. Materials

### 4.1 Already deprecated (R13)

DL-323 follow-through (2026-10-02, [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) §11.5):
`cooktorrance_material`, `isotropic_phong_material`,
`ashikminshirley_anisotropicphong_material`, `schlick_material`, `ward_isotropic_material`,
`ward_anisotropic_material` and `polished_material` warn once per type per load and
render bit-identically (`DeprecatedMaterialRenderIdentityTest`). Scene usage
(Tests/FB/other): Cook-Torrance 8/3/0, Phong 7/2/0, Ashikhmin 2/2/1, Schlick 3/1/0,
Ward-iso 3/2/0, Ward-aniso 4/2/0, Polished 4/13/0; 35 scenes in total, matching §11.5.
53 test files use the family. Agent eval scenarios/fixtures name them (4-5 files each;
these are hand-written and stay). Their eight IJob virtuals remain; the 3DSMax plugin calls
them. Code: about 12k lines.

*Debt:* this family generated the most correctness debt of any in the tree. Rows naming
the classes: Schlick 17, Cook-Torrance 16, Polished 16, Ward 13, Phong/Ashikhmin 13
(DL-67, 69, 98-103, 125, 127, 170, 176-178, 212, 216, 225, 285, 310, ...). Open: DL-481
lists them among the endpoint types (shared with GGX and others), and DL-339(a) names
`polished` (shared with dielectric and random-walk SSS).

*Recommendation:* promote from deprecate-warn to **freeze** (no further fixes, and their
names stripped from shared rows). Migrate the 35 scenes with the §11.5 table in Phase 2.
Look changes are expected because "No translation is exact". **Remove** in Phase 3.
Note that 17 of the 35 are Polished in FB showcases (139 chunks), so this is the biggest
look change in the programme (D9).

### 4.2 Other legacy or questionable materials

**R14 `iridescent_painter`** (a painter, listed here because it fakes a material effect).
Its own descriptor says "A cheap look-alike only -- the physical thin-film model is
ggx_m[aterial]". Used in 4/6/3 scenes (`showroom`, `tidepools`, `teapot`, `aphrodite`,
`f16`, three dreamscape benchmarks). *Recommendation:* deprecate-warn; migration changes
the look.

**R15 `composite_material`.** Rebuilt under DL-24/DL-341/DL-297 into a conserving layered
evaluator, but it is still expensive in debt: three of the 26 open rows (DL-221 HWSS
companion pricing "ruled UNCLOSABLE", DL-407 stack/seeding, DL-472 walker-only transport)
and 22 historical rows. Usage: 1 Tests scene (`Materials/composite_material`), 15 test
files. The glTF importer's sheen-composite path is `#if 0` (dead,
`GLTFSceneImporter.cpp` 1763-1829). `coated_material` covers coat-over-base, which is the
common layered look. Composite's unique ability is stacking two **arbitrary** materials.
*Recommendation:* owner call (D10). Freezing it closes three open rows.

**R16 `translucent_material`.** §11.5 already ruled "NOT deprecated (no real replacement)":
it is the only two-sided thin-sheet diffuse transmitter (leaf, paper, lampshade). It was
rebuilt as one consistent sampler/density/evaluator (DL-157/DL-41). 8/4 scenes, 16 test
files, 27 historical rows. *Recommendation:* **keep** (D11 confirms).

**R17 `phong_luminaire_material`.** A cos^N emission profile with no replacement. Used in
1 coverage scene only; 7 test files. *Recommendation:* **freeze** (LEGACY-NR, so it
cannot be removed without losing the feature).

**R18 `biospec_skin_material`, `generic_human_tissue_material`.** These are the owner's
own published research models (BioSpec: Krishnaswamy & Baranoski). They are spectral,
SPF-only (`GetBSDF()` is null), and needed special handling in BDPT/VCM/MLT (DL-126,
DL-183). Usage: 2 Tests (`spectral_skin_fast`, `spectral_skinmodel`, both on
`pixelintegratingspectral`) and 1 coverage scene; 8 and 5 test files. *Recommendation:*
**keep-for-now** (D12). Their two scenes must move off R2 in any case.

**R19 `datadriven_material`** (MERL measured data). DL-325 gave it an SPF. Only a coverage
scene uses it. *Recommendation:* keep; measured BRDFs are legitimate.

**R20 SSS material models.** `subsurfacescattering_material` (diffusion profile, 8/1,
25 test files), `randomwalk_sss_material` (4/0, 22 test files),
`donner_jensen_skin_bssrdf_material` (0/0, 2 test files). Diffusion and random walk are
both current industry models (Cycles and Arnold ship both). Diffusion carries the open
DL-408(1); both carry DL-482. *Recommendation:* keep both. The Blender bridge exports
only random walk.

**Keep (SOTA):** `ggx_material`, `pbr_metallic_roughness_material`, `coated_material`,
`fabric_material`, `weave_material`, `sheen_material`, `hair_material`,
`lambertian_material`, `orennayar_material`, `dielectric_material`,
`perfectreflector_material`, `perfectrefractor_material`, `lambertian_luminaire_material`.

## 5. Lights

**R11 `ambient_light`.** It adds `color*power` at every shading point, with no position,
no direction and no shadow ray ([lighting-recipes.md](../skills/agent/lighting-recipes.md)
~88). The agent already **refuses** to insert it on every path. Usage: 52 Tests / 2 FB
(`cloud_showcase`, `Materials/sheer_curtain`); 32 of the 54 also use `pixelpel`. 10 test
files; 7 agent-source files (refusal logic); eval scenarios 2. The Blender bridge emits
one when `use_world_ambient` is on (`properties.py`, default `False`;
`rise_blender_bridge.cpp` ~3215). IJob `AddAmbientLight`. *Replacement:* a uniform
environment via the rasterizer's `radiance_map` (60 scenes already use `radiance_map`).
*Look change:* large. The environment is occluded and ambient is not, so migrated scenes
get darker in creases. This is intended, but it is a look change (D5). Debt: no open rows;
DL-207 historically. DL-465(3) notes that ambient is not in the light-selection tables.
*Recommendation:* **deprecate-warn** in Phase 1, migrate with the look reviewed, **remove**
in Phase 3. Retire or replace the Blender toggle with a uniform-world radiance map.

**R27 `omni_light`, `spot_light`, `directional_light`.** These are the standard delta
lights in PBRT, Mitsuba and Cycles. Known gap: VCM cannot sample directional lights (a
VCM gap, not a light defect). glTF `KHR_lights_punctual` needs them. Keep.

**SOTA:** `rect_light`, `shape_light`, `hosek_wilkie_skylight`, and luminaire materials.

## 6. Integrators

**R12 MLT.** `UNIFIED_INTEGRATOR_DECISION.md` already states "**MLT is deprecated**
(inherits BDPT, adds maintenance surface for no measured win on the corpus)" and "Retire
as covered: MLT now". The code still treats it as a first-class choice: no deprecation
flag; the Blender enum offers `MLT (Pel)`/`MLT (Spectral)`; it is mentioned in one Mac GUI
file. It has no OIDN, guiding, adaptive sampling or SMS (feature matrix §4). Usage:
`mlt_rasterizer` 6/6 (5 `FeatureBased/MLT` showcases plus `bdpt_torus_chain_atrium`),
`mlt_spectral_rasterizer` 3/0; 30 test files; about 3.5k lines plus `PSSMLTSampler.h`.
*Debt:* open rows DL-465, DL-475, DL-481, DL-482 and DL-483 all say "BDPT/MLT" or
"BDPT/VCM/MLT". MLT shares BDPT's generator, so most of that cost is BDPT's, and freezing
MLT removes only the MLT-specific verification burden (DL-08, DL-461 history).
*Recommendation:* **freeze → remove** (D8). The FB/MLT showcases move to BDPT or VCM.

**SOTA:** `pathtracing_{pel,spectral}`, `bdpt_{pel,spectral}`, `vcm_{pel,spectral}`,
`auto_{,spectral_}rasterizer` (the default in Blender), HWSS spectral, extended SMS
(opt-in), OIDN, guiding.

## 7. SSS shader ops (R6), alpha ops (R8), AO (R7), volume rendering (R10)

**R6 point-cloud SSS** (`Shaders/SSS/`, about 2.7k lines including `PointSetOctree`):
`simple_sss_shaderop` (6/3: `translucent_bunny`, `sss_gi_dragon`, `spotlight_drama`,
`Tests/SubsurfaceScattering/sss*`), `diffusion_approximation_sss_shaderop` (1/1),
`donner_jensen_skin_sss_shaderop` (0/0). These run only under R1. They are replaced by the
SSS materials under PT, which received the DL-49/DL-291/DL-306/DL-334 corrections; the
octree path received only DL-291's stack forwarding. *Recommendation:* freeze → remove.
The look of `translucent_bunny` and `spotlight_drama` changes.

**R8 alpha ops.** `alpha_test_shaderop` (coverage only) and `transparency_shaderop` (1 Tests
scene) are replaced by `alpha_coverage`/`alpha_mode`/`alpha_cutoff` (DL-214), which works
in every integrator. Deprecate-warn → remove.

**R7 `ambientocclusion_shaderop`** (2/2: `showroom`, `tidepools`, `ambocc_ibl`, `sss_ibl`).
No modern render mode produces AO. The draft-quality preview has its own AO
(`InteractivePelRasterizer.cpp` `CombinedAmbientOcclusion`), and painters can read the
`occlusion` signal. Removing it loses a stylised/utility pass (D6).

**R10 direct volume rendering.** `directvolumerendering_shader` (FB `Shaders/visiblehuman`
plus the top-level `volume/` data), `spectraldirectvolumerendering_shader` (0 scenes).
This is a medical-visualisation transfer-function renderer with no physically based
equivalent. `Volume/` is about 2k lines. *Recommendation:* keep-for-now and frozen (D7).

## 8. Geometry, cameras, painters, scene format

- **R21 `rawmesh_geometry`** (37 Tests: `Samplers` 9, `Spectral` 10, `UnifiedLighting` 5,
  ...; 1 `pr.RISEscene`) is the **only** producer of the legacy non-indexed
  `TriangleMeshGeometry` (`Job::AddRAWTriangleMeshGeometry` calls
  `RISE_API_CreateTriangleMeshGeometry`; every other loader builds
  `TriangleMeshGeometryIndexed`). That class has no watertightness certification
  (DL-96 residual, DL-382(1)). *Recommendation:* keep the chunk and file format, but load
  into the indexed class and retire the non-indexed class (D14). The look should be
  unchanged except where certification now applies; this needs a render A/B.
- **R22 `3dsmesh_geometry`**: coverage scene only → deprecate-warn → remove.
- `rawmesh2_geometry` (8), `risemesh_geometry` (12/5), `plymesh_geometry` (7/1),
  `gltf_import`/`gltfmesh_geometry`, `bezierpatch_geometry` (R23, 7/6; carries DL-382(1)
  and DL-20 history), analytic primitives, SDF, sweep/skeleton/lathe, hair: keep.
- **R24 `onb_pinhole_camera`**: coverage scene only; same capability as `pinhole_camera` →
  deprecate-warn → remove. Other cameras: keep.
- **Painters:** apart from R14, nothing is legacy. `spectral_painter` is legitimate as a
  colour painter; its old misuse in scalar slots was already migrated
  (`tools/migrate_scenes_iscalarpainter.py`). `mandelbrot_painter`/`lines_painter` are toy
  procedurals (2/1, 2/2), and dropping them is optional. Parameter-level deprecation already
  exists in prose for `ggx_material.tangent_rotation` (DL-16) but has no descriptor flag.
- **Scene format:** every tracked scene is `RISE ASCII SCENE 7` (467 files with the header;
  0 at v6). Migrators `tools/migrate_scenes_*.py` (v5→v7, colour space, IScalarPainter,
  light colorspace, relief) are cheap; keep them as INFRA.

## 9. Front-ends, bridges and build (R26)

- **Blender** (`src/Blender`): exposes Pixel Rasterizer (R1), MLT ×2 (R12), and
  `use_world_ambient` (R11). Materials: only SOTA kinds. Uses `SetMaterialAlpha`.
- **glTF importer:** only SOTA kinds; `composite_material` code is `#if 0`;
  `directional_light` for punctual lights.
- **Mac / Windows GUIs:** no hard-coded legacy rasterizer pickers. Node palettes already
  sort and badge any `deprecated` chunk type generically (DL-401:
  `SceneEditController.cpp` ~15980, `NodeGraphPalette.swift`, `NodeGraphCanvas.cpp` ~794),
  so new deprecations show up in the GUIs with no GUI code.
- **Android:** bundles 8 scenes; 3 use `pixelpel` and 3 use deprecated materials
  (`pt_jewel_vault`, `kaleidoscope_atrium`, `tidepools`). Phase 2 must migrate them or
  swap them out.
- **Agent:** refuses `ambient_light`; gates MLT/BDPT/VCM/auto; leaves pixel rasterizers
  ungated on a stale premise (§2.3 R1). Skills already steer to modern materials.
- **Legacy front-ends:** `src/3DSMax` (34 files, "not built here" per §11.5, calls the
  legacy IJob entry points), `src/PRISE` (19 files, only referenced by the `deps` rule),
  `src/DRISE` (29 files, still built by `build/make/rise/Makefile` as
  `drise_server`/`drise_client`; it gained `MCPClientConnection` in July 2026, so someone
  uses it), `build/VS2005` (27 files, last touched 2026-07-01), and
  `build/make/rise/Config.SGI`, `Config.SGI.MIPSPro`, `Config.Solaris`.

## 10. Policy: what "deprecated and not supported" should mean

### 10.1 Four tiers, one flag on the descriptor

Replace the boolean `ChunkDescriptor::deprecated` with a tier (keeping `deprecated` as
"tier >= Deprecated" so existing consumers keep working):

| Tier | Parses / renders | Warning | Debt policy | Agent | GUI |
|---|---|---|---|---|---|
| **Supported** | yes | none | normal | normal | normal |
| **Deprecated** | yes, bit-identical | one per type per load (existing DL-323 text) | fixes allowed but not sought | may edit, should not insert (skills steer) | sorted last, badged (DL-401) |
| **Frozen** | yes, bit-identical | `` `X` is LEGACY and UNSUPPORTED (frozen <date>): it renders as before but receives no fixes. Use Y; migrate with tools/migrate_legacy.py. `` | won't-fix (see 10.2) | **refuse insert** (as `ambient_light` today); editing existing chunks still allowed | sorted last, badged "(legacy, unsupported)"; hidden behind "show legacy" in add-node menus |
| **Removed** | parse **error** naming the replacement and migrator (the v5-header pattern) | n/a | rows closed | n/a | gone |

Also needed: a **parameter-level** flag (`ParameterDescriptor::deprecated`) for cases like
`tangent_rotation` and, after D13, `defaultshader` on pure rasterizers. Non-chunk surfaces
need their own marking: the Blender enum items (relabel "(legacy)"), `use_world_ambient`,
and `RISE_API_*` / IJob entry points (log a warning, keep them in the vtable until a
declared ABI break, because the vtable manifest is append-only).

### 10.2 Ledger policy

- Add a **"Frozen features"** section at the top of `DEBT_LEDGER.md` listing each frozen
  family, its freeze date, and the commit.
- An open row whose **only** subject is a frozen feature is closed as
  **`WON'T-FIX (frozen: <family>)`**: struck, with the reason. Today that is DL-451
  (if R1/R9 are frozen) and, for composite (if D10 = freeze), DL-221, DL-407 and DL-472.
- **Shared rows** (DL-465, DL-481, DL-482, DL-483, DL-339, DL-408, DL-382) are
  **narrowed**: the frozen part is struck in place, and the row stays open for the SOTA part.
- New findings in frozen code go into a one-line "known issues" list under the frozen
  family, never into new DL rows. **Exceptions** that are still fixed: crashes, hangs,
  memory corruption, security, and data loss (D16 confirms the exception list).
- Sibling audits (the `audit-by-bug-pattern` skill) stop at frozen code. That is where
  most of the saving comes from: historically every MIS/IOR/emitter fix had to visit the
  photon tracers, the legacy chain and the seven legacy SPFs as siblings.

### 10.3 Test policy

- For each frozen family keep **one** render-identity hash test (the
  `DeprecatedMaterialRenderIdentityTest` pattern) plus parser coverage. Freeze the
  correctness suites for frozen code as they are: they keep running, are never extended,
  and if a SOTA change turns one red, the frozen-feature check is re-pinned rather than
  the frozen code fixed (D17).
- Tests that use a legacy feature **as a reference or harness** for SOTA code (for
  example a `pixelpel` direct-only render inside a signal or BDPT test) must move off it
  in Phase 2. There are 37 test files naming `pixelpel_rasterizer`; each needs triage into
  "tests the legacy feature" versus "uses it as a harness".
- The ChunkCoverage `cc_*` scenes for frozen chunks stay until removal.

### 10.4 Docs

- One index page (this document, made into `docs/LEGACY.md` when decided) with the tier
  table. A banner line at the top of each frozen feature's design doc. CLAUDE.md
  high-value facts that concern only frozen features get condensed to a pointer (several
  long entries are photon-tracer or legacy-chain material). Fix the stale "pixelpel is
  required for alpha-mask scenes" claim in `AgentSession.cpp`, `AgentChatCodecs.cpp`,
  `AgentMcpAdapter.cpp` and `skills/agent/scene-skeleton-and-conventions.md`. Move the
  quickstart sample render off `shapes.RISEscene`'s `pixelpel`, or migrate that scene.

## 11. Phased plan and order of work

**Phase 0 — decisions** (this document). The owner answers §12.

**Phase 1 — mark and freeze (one slice; zero render change).**
1. Add the tier and the parameter flag to the descriptor; update the warning text; add the
   agent refusal for Frozen; add the GUI "show legacy" filter.
2. Mark the chunks per §12's decisions (R1-R3, R5, R6, R8, R9, R11-R14, R17, R22, R24, and
   R10/R7/R15 depending on the calls). Relabel the Blender enum items.
3. Ledger sweep per 10.2. Docs per 10.4.
4. Gate: one render-identity hash per newly marked family against the parent commit, plus
   `CstDeriveGoldenTest` with no drift. Because nothing renders differently, this phase is
   safe to land first.

**Phase 2 — migrate shipped content (several slices; look changes are reviewed).**
Order by risk, lowest first:
1. Pure renames with no look change: `mis_pathtracing_shaderop` alias, `onb_pinhole_camera`,
   `3dsmesh` coverage, alpha ops → `alpha_coverage`.
2. `pixelpel` + `pathtracing_shaderop` (29) and `pixelintegratingspectral` (20) →
   `pathtracing_*_rasterizer` with `oidn_denoise FALSE`. Write
   `tools/migrate_scenes_legacy_rasterizer.py`.
3. The 82 direct-only scenes per D3.
4. The 37 test-harness files (10.3).
5. Look-changing families, each FB scene reviewed by the owner: R13 materials (35 scenes,
   using the §11.5 table, as a tool), R11 ambient (54), R5 photon/FG (26 → VCM/PT),
   R6 point-cloud SSS (9), R14 iridescent (13), R12 MLT (15).
6. Android catalogue, quickstart sample, and Blender defaults last, once the replacement
   scenes exist.

**Phase 3 — remove (several slices; each slice must update all five build projects per
CLAUDE.md).** Order from least coupled:
1. Point-cloud SSS ops + `PointSetOctree` (R6).
2. Photon maps, gathers, irradiance cache, final gather (R5), including the DL-05
   suppression hook and the `IScene`/IJob photon APIs.
3. Alpha/transparency/AO/area-light/DT/SMS shader ops (R3, R7?, R8, R9).
4. D13: route PT's SSS continuation straight to the integrator; then remove the pixel
   rasterizer **chunks** (R1, R2) and the Default* op registrations; `defaultshader`
   becomes optional. Keep the `PixelBased*` classes as PT's base.
5. MLT (R12).
6. The seven legacy material classes (R13) last, because many MIS/consistency tests
   instantiate them as multi-lobe fixtures and need replacement fixtures first.
7. IJob: keep the removed methods as logged no-ops returning `false` until a declared ABI
   break (the vtable manifest is append-only; the 3DSMax plugin, if kept, links them).

## 12. Decisions needed from the owner

Reply by number. Each has a recommended default in brackets.

1. **Legacy shader-op pipeline (`pixelpel`, chain ops):** freeze now and remove the chunks
   in Phase 3? [yes]
2. **Custom shader-op composition as a user feature:** accept losing it (AO + transparency
   + PT chains and so on), with modern integrators as the only rendering modes? [yes]
3. **Direct-lighting-only renders (82 scenes):** (a) migrate to PT with
   `max_diffuse_bounce 0` and accept the look change; (b) add a `direct_only` flag to
   `pathtracing_pel_rasterizer`; or (c) keep `pixelpel` frozen just for this. [b]
4. **Photon mapping / final gather / irradiance cache:** freeze and then remove; the FB
   showcases move to VCM or PT with looks reviewed. [yes]
5. **`ambient_light`:** deprecate and migrate to a uniform `radiance_map`, accepting
   occluded (darker) results; retire Blender's `use_world_ambient`. [yes]
6. **`ambientocclusion_shaderop`:** (a) remove with no replacement; (b) keep frozen;
   (c) add an AO AOV to the PT rasterizer. [a, unless AO passes are still wanted]
7. **Direct volume rendering (`visiblehuman`, `Volume/`):** keep frozen or remove? [keep frozen]
8. **MLT:** freeze and remove, as `UNIFIED_INTEGRATOR_DECISION.md` already decided;
   drop it from the Blender enum. [yes]
9. **The seven deprecated materials:** promote to frozen (won't-fix) now; migrate 35 scenes
   (17 Polished FB showcases change look); remove in Phase 3. [yes]
10. **`composite_material`:** keep supported (three open rows stay) or freeze in favour of
    `coated_material` (closes DL-221/407/472). [freeze, unless arbitrary two-material
    stacking is a product feature]
11. **`translucent_material`:** confirm it stays supported as the thin-sheet model. [yes]
12. **BioSpec skin / generic human tissue:** keep supported, keep frozen, or remove?
    [keep supported; they are the owner's research models]
13. **PT's SSS continuation shading through the shader chain:** approve rerouting it
    directly to the integrator, so the shader pipeline can shrink to infrastructure-free.
    [yes, in Phase 3]
14. **`rawmesh_geometry`:** load into the indexed mesh class and retire the non-indexed
    `TriangleMeshGeometry`. [yes, with a render A/B]
15. **Legacy front-ends and build files:** remove `src/3DSMax`, `build/VS2005`, and the SGI
    and Solaris configs? Keep or remove `src/DRISE` / `src/PRISE`? [remove the first
    group; DRISE/PRISE need the owner's knowledge of current use]
16. **Frozen-code exceptions:** still fix crashes, hangs, memory corruption, security and
    data loss in frozen code? [yes]
17. **Frozen-code tests:** keep them running and re-pin instead of fixing when SOTA changes
    turn them red? [yes]
18. **Smaller items in one batch:** deprecate `iridescent_painter`, `onb_pinhole_camera`,
    `3dsmesh_geometry`, the `mis_pathtracing_shaderop` alias, and `sms_shaderop`; freeze
    `phong_luminaire_material`; keep `datadriven_material`, `bezierpatch_geometry`, both
    SSS material models, and the delta lights. [yes to all]
