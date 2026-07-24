# Public Scene Corpus Cleanup — 2026-07-24

**Status:** executed. This record covers every tracked `.RISEscene` below
`scenes/` except `scenes/Internal/`.

The cleanup applies the placement rules in [README.md](README.md): focused
validation and comparisons belong in `Tests/`; visually coherent,
multi-feature, or end-to-end stress scenes belong in `FeatureBased/`.

## Result

- The public corpus is self-contained native-v7. No public `.RISEscript` or
  scene-v6 header remains.
- Functionally duplicate scenes were removed.
- Output-format-only LightBVH files were folded into their owning scenes.
- Useful integrator variants were retained as focused tests rather than
  duplicated across the showcase tree.
- One presentation owner remains in `FeatureBased/` for each consolidated
  composition.
- The CLI default and GUI starter remain outside the sample taxonomy because
  they are runtime product assets.

## Removed

The following scenes had no distinct active configuration after comments,
output filenames, and equivalent numeric spellings were ignored:

- `Tests/UnifiedLighting/torture_100lights_corridor_pt.RISEscene`
- `Tests/UnifiedLighting/torture_corridor_pt.RISEscene`
- `Tests/UnifiedLighting/cornellbox_mixed_lights_bdpt.RISEscene`
- `Tests/UnifiedLighting/cornellbox_pointlight_bdpt.RISEscene`
- `Tests/SMS/sms_k2_glasssphere_boxfilter.RISEscene`

`FeatureBased/MLT/mlt_cornell_simple.RISEscene` was also removed: it was a
bring-up configuration already covered by the two Cornell MLT tests.

The include-era support files `colors.RISEscript`,
`standard_colors.RISEscene`, and `povray_colors.RISEscene` were removed.
Native-v7 scenes contain their painter definitions inline.
Some inline painter blocks retain their original `File:` attribution headers;
those comments record provenance and are not file dependencies.

## Consolidated and Repaired

- The HDR outputs formerly represented by
  `corridor_100lights_{alias,bvh}_hdr.RISEscene` are now second output blocks
  in the corresponding PNG scenes.
- Every surviving `Tests/LightBVH/*_alias.RISEscene` explicitly sets
  `light_bvh FALSE`. This repairs the alias-vs-BVH matrix after the production
  default changed to `TRUE`.
- `pt_alchemists_sanctum` and `pt_jewel_vault` are the presentation owners for
  their compositions. Their BDPT renders now live under `Tests/BDPT/`.
- `bdpt_torus_chain_atrium` is the presentation owner for the torus-chain
  composition. Its PT and MLT renders now live under their test categories.
- The luminous-orb, reflected-caustic, and Veach-egg MLT compositions retain
  their MLT showcase owners. BDPT, PT, SMS, and VCM comparison renders moved
  to the corresponding test categories, and their filenames/output patterns
  now start with the integrator actually exercised.
- The displaced VCM egg is the VCM showcase at
  `FeatureBased/VCM/vcm_veach_egg_displaced.RISEscene`; the Android catalog
  points to that path and describes the correct integrator.

## Placement Changes

Moved to focused regression coverage:

- `Tests/Animation/motion_blurred_caustic.RISEscene`
- `Tests/Geometry/{aphrodite_mesh,teapot_analytic}.RISEscene`
- `Tests/Materials/iorstack.RISEscene`
- `Tests/Materials/Enamel/` — silver reference, material swatches, dome,
  flat-SDF, and dimpled-SDF controls
- alternate-integrator renders under `Tests/{BDPT,MLT,PathTracing,SMS,VCM}/`

Moved or retained as showcases:

- `FeatureBased/Combined/composite_wacky_creature.RISEscene`
- `FeatureBased/Materials/enamel_grandfeu_opaque.RISEscene`
- `FeatureBased/EnamelWatch/enamel_watch.RISEscene`
- `FeatureBased/VCM/vcm_veach_egg_displaced.RISEscene`

`pr.RISEscene` remains root-level because `src/RISE/commandconsole.cpp` loads
it when no scene path is supplied. `Templates/empty_starter.RISEscene`
remains the GUI/agent product template.

## Intentionally Repetitive Families

These small variants remain because their changed setting is the regression
target:

- `Tests/PixelFilters/*`
- `Tests/Samplers/*`
- path-guiding OneSampleMIS/RIS pairs
- LightBVH alias/BVH/high-SPP-reference matrices
- spectral sample/reference matrices
- `Tests/Volumes/pt_painter_*`
- `Tests/ChunkCoverage/*`
- PT/BDPT/VCM parity renders for vertex colors, SSS, emitters, and Cornell
  compositions
- optional `FeatureBased/Geometry/sponza_new*` import packages

Native-v7 has no recursive scene include mechanism, so retained matrices stay
self-contained until a supported parameterization mechanism exists.

## Required Gate

Any later scene add, move, or removal must regenerate and review the CST
corpus manifest:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
make -C build/make/rise ../../../bin/tests/CstDeriveGoldenTest
./bin/tests/CstDeriveGoldenTest --generate
git diff tests/data/cst_derive_golden.txt
./bin/tests/CstDeriveGoldenTest
```
