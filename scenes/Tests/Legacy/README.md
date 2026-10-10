# Frozen-feature regression corpus

The scenes in this folder are **verbatim copies** of shipped scenes taken at
`master` `6ff502457`, just before legacy-deprecation Phase 2 migrated the
shipped copies off FROZEN features (see
[docs/LEGACY_DEPRECATION_ASSESSMENT.md](../../../docs/LEGACY_DEPRECATION_ASSESSMENT.md)
§11 and the owner rulings in §13).

Owner ruling #17: frozen code keeps running and stays tested.  When migrating a
shipped scene would remove the only regression coverage of a frozen chunk, the
original is kept here and the test that covered it points here instead.

**Never migrate or edit these files.**  `tools/migrate_scenes_legacy_rasterizer.py`
skips this folder by design.  They render exactly as they did before the
migration; a test that pins their render hash measured it on the parent commit.

| Scene | Original location | Covers (frozen / deprecated) | Pinned by |
|---|---|---|---|
| `uniform_green_spectral.RISEscene` | `scenes/Tests/Spectral/` | `pixelintegratingspectral_rasterizer` + `pathtracing_shaderop` chain | `LegacyTierRenderIdentityTest` |
| `transparency_shaderop.RISEscene` | `scenes/Tests/Shaders/` | `transparency_shaderop` under `pixelpel_rasterizer` | `LegacyTierRenderIdentityTest` |
| `gltf_box.RISEscene` | `scenes/Tests/Geometry/` | `ambient_light` under a direct-lighting-only `pixelpel_rasterizer` chain | `LegacyTierRenderIdentityTest` |
| `materials.RISEscene` | `scenes/Tests/Materials/` | the deprecated Cook-Torrance / Schlick / Ward / Ashikhmin-Shirley materials under `pixelpel_rasterizer` | `LegacyTierRenderIdentityTest` |
| `fabric_presets.RISEscene` | `scenes/Tests/Materials/` | the legacy pixel-rasterizer OIDN Auto policy family (64 spp x weight 0.1) | `OIDNAutoDeterminismTest` |

The ChunkCoverage `cc_*` scenes for frozen and deprecated chunks
(`scenes/Tests/ChunkCoverage/`) are coverage-only scenes and stay where they
are; they are already the frozen-feature coverage for their chunk.
