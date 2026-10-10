# Frozen-feature regression corpus

The scenes in this folder are copies of shipped scenes taken just before
legacy-deprecation Phase 2 migrated the shipped copies off FROZEN features
(slice A at `master` `6ff502457`, slice B1 at `master` `83aa86d2f`, slice B2 at `master` `5bd23470d`) (see
[docs/LEGACY_DEPRECATION_ASSESSMENT.md](../../../docs/LEGACY_DEPRECATION_ASSESSMENT.md)
§11 and the owner rulings in §13).

Owner ruling #17: frozen code keeps running and stays tested.  When migrating a
shipped scene would remove the only regression coverage of a frozen chunk, the
original is kept here and the test that covered it points here instead.

**Never migrate or edit these files.**  `tools/migrate_scenes_legacy_rasterizer.py`
and `tools/migrate_scenes_legacy_families.py` skip this folder by design.  They render exactly as they did before the
migration; a test that pins their render hash measured it on the parent commit.
The ONE change from the originals is the output file name: every
`file_rasterizeroutput` writes `rendered/legacy_<name>` so a render of the copy
cannot overwrite the migrated scene's image (render-hash tests capture the
image in memory and never see the file name).

| Scene | Original location | Covers (frozen / deprecated) | Pinned by |
|---|---|---|---|
| `uniform_green_spectral.RISEscene` | `scenes/Tests/Spectral/` | `pixelintegratingspectral_rasterizer` + `pathtracing_shaderop` chain | `LegacyTierRenderIdentityTest` |
| `transparency_shaderop.RISEscene` | `scenes/Tests/Shaders/` | `transparency_shaderop` under `pixelpel_rasterizer` | `LegacyTierRenderIdentityTest` |
| `gltf_box.RISEscene` | `scenes/Tests/Geometry/` | `ambient_light` under a direct-lighting-only `pixelpel_rasterizer` chain | `LegacyTierRenderIdentityTest` |
| `materials.RISEscene` | `scenes/Tests/Materials/` | the deprecated Cook-Torrance / Schlick / Ward / Ashikhmin-Shirley materials under `pixelpel_rasterizer` | `LegacyTierRenderIdentityTest` |
| `fabric_presets.RISEscene` | `scenes/Tests/Materials/` | the legacy pixel-rasterizer OIDN Auto policy family (64 spp x weight 0.1) | `OIDNAutoDeterminismTest` |
| `arealight_shaderop.RISEscene` | `scenes/Tests/Shaders/` | `arealight_shaderop` | `LegacyTierRenderIdentityTest` |
| `dielectric_dispersion.RISEscene` | `scenes/Tests/Materials/` | `distributiontracing_shaderop`, Whitted `DefaultRefraction` | `LegacyTierRenderIdentityTest` |
| `cornellbox_fg.RISEscene` | `scenes/Tests/GlobalIllumination/` | `finalgather_shaderop`, `global_pel_photonmap` / gather, `irradiance_cache` | `LegacyTierRenderIdentityTest` |
| `sss_ibl.RISEscene` | `scenes/Tests/SubsurfaceScattering/` | `simple_sss_shaderop`, `ambientocclusion_shaderop` | `LegacyTierRenderIdentityTest` |
| `sss.RISEscene` | `scenes/Tests/SubsurfaceScattering/` | `diffusion_approximation_sss_shaderop`, `simple_sss_shaderop` | `LegacyTierRenderIdentityTest` |
| `rgb_dispersive_caustic.RISEscene` | `scenes/Tests/Caustics/` | `caustic_pel_photonmap` / gather | `LegacyTierRenderIdentityTest` |
| `showroom.RISEscene` | `scenes/FeatureBased/Combined/` | `irradiance_cache`, `ambientocclusion_shaderop`, `iridescent_painter`, `polished_material` | `LegacyTierRenderIdentityTest` |
| `translucent_bunny.RISEscene` | `scenes/FeatureBased/Animation/` | `directlighting_shaderop`, `simple_sss_shaderop`, `iridescent_painter` | `LegacyTierRenderIdentityTest` |
| `blurry_floor.RISEscene` | `scenes/Tests/Shaders/` | `distributiontracing_shaderop`, `isotropic_phong_material` | `LegacyTierRenderIdentityTest` |
| `teapot.RISEscene` | `scenes/FeatureBased/Geometry/` | Whitted `DefaultReflection`, `polished_material`, `bezierpatch_geometry` | `LegacyTierRenderIdentityTest` |
| `spectral_dispersive_caustic.RISEscene` | `scenes/Tests/Spectral/` | `caustic_spectral_photonmap` / gather under `pixelintegratingspectral_rasterizer` | `LegacyTierRenderIdentityTest` |
| `different_rmaps.RISEscene` | `scenes/Tests/Shaders/` | per-object `radiance_map` (frozen: only the legacy chain honours it), `distributiontracing_shaderop`; MOVED here, not copied -- it was never migrated | `LegacyTierRenderIdentityTest` |
| `dt_with_irrcache.RISEscene` | `scenes/Tests/Shaders/` | `distributiontracing_shaderop` with `irradiance_caching` and no direct-lighting sibling (DL-209's chain shape) | `LegacyTierRenderIdentityTest` |
| `composite_material.RISEscene` | `scenes/Tests/Materials/` | `composite_material` (frozen) under an `ambient_light` (slice B2 moved the shipped copy's ambient to a uniform environment) | `LegacyTierRenderIdentityTest` |
| `cooktorrance_multiscatter_test.RISEscene` | `scenes/Tests/Materials/` | `cooktorrance_material` multiscatter compensation (its subject); MOVED here, not copied | `CstDeriveGoldenTest` |
| `cooktorrance_vndf_test.RISEscene` | `scenes/Tests/Materials/` | `cooktorrance_material` VNDF sampling (its subject); MOVED here, not copied | `CstDeriveGoldenTest` |
| `ggx_vs_cooktorrance.RISEscene` | `scenes/Tests/Materials/` | `cooktorrance_material` beside `ggx_material` (a comparison that migration would make meaningless); MOVED here, not copied | `CstDeriveGoldenTest` |
| `cornellbox_mlt_fast.RISEscene` | `scenes/Tests/MLT/` | `mlt_rasterizer` (slice B2 migrated the shipped copy to BDPT) | `CstDeriveGoldenTest` (MLT is not hash-pinnable: not reproducible run to run) |
| `mlt_deep_fog.RISEscene` | `scenes/Tests/MLT/` | `mlt_rasterizer` through a medium (the PSSMLT walk-stream layout notes cite it) | `CstDeriveGoldenTest` |
| `mlt_deep_fog_spectral.RISEscene` | `scenes/Tests/MLT/` | `mlt_spectral_rasterizer` | `CstDeriveGoldenTest` |

The ChunkCoverage `cc_*` scenes for frozen and deprecated chunks
(`scenes/Tests/ChunkCoverage/`) are coverage-only scenes and stay where they
are; they are already the frozen-feature coverage for their chunk.
