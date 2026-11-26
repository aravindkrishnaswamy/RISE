# Batch 7 historical image drift investigation

The +9.767% default-seed mean shift is the intended DL-368 pixel-centre correction, implemented in `e19288fe7` (parent `f03359223`) and integrated in `8dcf20d69`. No material implementation was changed by this investigation. The original scene is native whole-path PT, 40x16, 16 spp, box filter, fixed `srand(4242)`, one worker; its shader declaration does not make it a legacy shader-op render.

The native PT rasterizer and box filter changed the sample placement from the half-pixel-shifted film to the camera’s nominal film. DL-368 already records that every render shifts half a pixel, its closed-form pixel tests (0/48→48/0), and regenerated image references. Consequently this small scene’s whole-frame mean changes even though its material parameters are unchanged. DL-354’s BDPT (1,1) change in the same implementation commit does not execute in this PT render.

The fixed diagnostic harness is `tests/ImageDriftHistoryTest.cpp`. Default invocation pins the post-DL-368 hash; `DRIFT_MEASURE_ONLY=1` measures historical versions, `DRIFT_CONTROL=7/8/9` binds every sphere to one supported control (floor unchanged), and `DRIFT_SALT` selects `SobolSamplerTestHooks::ValueSalt`. Each measurement uses a fresh process, not sequential renders sharing global RNG state. Controls are whole-frame variants, not object-local measurements in the mixed scene.

## Default-seed boundary

| Version | Mean | Pixel hash |
|---|---:|---|
| `f03359223` | 0.607363771973183 | `13c3910b9964ab63` |
| `e19288fe7` | 0.666687203410431 | `a4e972d01879f63c` |
| `b2a4460b9` | 0.607363771973183 | `13c3910b9964ab63` |
| `8dcf20d69` | 0.666687203410431 | `a4e972d01879f63c` |
| `98cc0bff7` | 0.666687203410431 | `a4e972d01879f63c` |
| `92d746961` (integrated batch 7) | 0.666687203410431 | `a4e972d01879f63c` |

The adjacent first-parent boundary is `b2a4460b9`→`8dcf20d69`: both sides reproduce the exact endpoint hashes. This narrows the entire observed default-seed drift to that merge. The implementation commit’s direct-parent pair `f03359223`→`e19288fe7` reproduces the same exact two hashes; this is historical attribution, not a newly repaired estimator. No unexplained supported-material change was found, so no new debt row is opened.

## Salted supported controls

Four common salts (101, 202, 303, 404) per variant and side. Sample SD is across complete-image means. The last column is three combined standard errors, `3 sqrt(sd_before²/4 + sd_after²/4)`; these are diagnostic image differences, not an energy-conservation oracle or timing claim.

| Variant | n per side | Before mean / SD | After mean / SD | Change | 3 combined SE |
|---|---:|---:|---:|---:|---:|
| Historical mixed scene | 4 | 0.606006997 / 0.000934905 | 0.667092022 / 0.002476569 | +10.07992% | 0.003970737 |
| All spheres translucent | 4 | 0.601291080 / 0.000424937 | 0.660849073 / 0.000570688 | +9.90502% | 0.001067276 |
| All spheres GGX | 4 | 0.596289198 / 0.000283455 | 0.656628594 / 0.000518803 | +10.11915% | 0.000886782 |
| All spheres Lambertian | 4 | 0.596032659 / 0.000481778 | 0.655259463 / 0.000703764 | +9.93684% | 0.001279311 |

Raw measurements: [BATCH7_IMAGE_DRIFT.csv](BATCH7_IMAGE_DRIFT.csv).

## Reproduction and gates

Every library/test build used the named target `make -C build/make/rise -j8 build-test/ImageDriftHistoryTest`, checked actual return code and warnings, and ran only in the owned worktree. Historical checkouts were detached; master was untouched by the investigation. The default hash gate is red on `f03359223`: **2 passed / 1 failed (rc1)**, n=1, SD=0 (deterministic hash witness). The same harness is **3/0 (rc0)** on `e19288fe7`, reproducing the exact post-convention hash. Current `92d746961` named builds all return rc0 with no warnings; default diagnostic **3/0**, `PixelCenterConventionTest` **48/0**, `CameraImportanceTest` **984/0**, and `DeprecatedMaterialRenderIdentityTest` **3/0**, all actual rc0. All sixteen salted mixed/control images on current master are bit-identical to `8dcf20d69` in both mean and pixel hash. These checks establish attribution for this fixture, not universal material identity for every scene. No production code or frozen feature is modified.
