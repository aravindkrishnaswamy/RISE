# Test Scenes

This tree holds the focused validation scenes for RISE. Unlike `scenes/FeatureBased`, these scenes are allowed to be minimal, repetitive, or comparison-oriented if that makes regressions easier to detect.

## How To Use These Scenes

Build `bin/rise`, set `RISE_MEDIA_PATH` to the repo root, then render a scene by piping commands into the console app:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

## Directory Index

- `Animation/`: focused animation and exposure regression scenes
- `BDPT/`: bidirectional path tracing baselines and comparison scenes,
  including alternate-integrator renders of showcase compositions
- `BSSRDFFurnace/`: energy-conservation regression scenes for subsurface scattering
- `Bench/`: standardized benchmark scenes used by `bench.sh` for wall-time comparisons across commits.  `bench_pt` / `bench_bdpt` / `bench_vcm` are the small Cornell-class controls; `bench_pt_bigmesh` is a procedurally-displaced ~1M-tri ellipsoid (geometry-bound, single deep-BVH mesh) added as the third regime in the BVH-stack benchmark sweep so wide-tree / SoA-leaf optimisations get a fair test (the small bench scenes top out at 47K tris where wide-SIMD never pays off — see [docs/BVH_RETROSPECTIVE.md](../../docs/BVH_RETROSPECTIVE.md) Tier D2-rev for context)
- `Camera/`: isolated physical-exposure validation
- `Cameras/`: isolated camera-model checks (sensor-format presets, tilt-shift, named-camera selection)
- `ChunkCoverage/`: minimal parser-chunk acceptance and derive coverage
- `Caustics/`: compact caustic and SMS comparison scenes
- `Geometry/`: primitive and CSG sanity scenes
- `GlobalIllumination/`: focused GI baselines such as final gather
- `Importers/`: glTF import regression scenes (Khronos sample assets, alpha modes, embedded textures, light-control)
- `LightBVH/`: many-light regression scenes comparing alias-table sampling vs. light BVH (corridor 20/100 lights, spotlights stage, BDPT mixed-light Cornell)
- `Lighting/`: physically based unit and environment-map lighting checks
- `Lights/`: environment, mesh-emitter, and analytic-sky comparisons
- `MLT/`: Metropolis light transport baselines and comparison renders
- `Materials/`: isolated material demonstrations and regression scenes;
  `Materials/Enamel/` contains the silver, swatch, dome, SDF, and dimple
  controls used to build the enamel-watch hero
- `Painters/`: painter- and texture-accessor-specific scenes
- `Parser/`: parser-language regression scenes
- `PathTracing/`: unidirectional PT baselines, path-guiding comparisons, and
  PT renders of showcase compositions used for integrator parity
- `PixelFilters/`: filter comparison scenes
- `SDF/`: signed-distance-field (sphere-traced implicit) geometry checks — `sdf_shadows` (melded blob + analytic sphere/box, mutual shadows + inter-geometry depth), `sdf_volume` (glass SDF bounding a fog interior_medium, ray-march entry/exit driving the IOR stack), `sdf_caustic` (glass SDF torus refracting a ring caustic via the caustic photon map), `sdf_arealight` (a glowing SDF torus as a true NEE-sampled area light), `sdf_luminaire_fog` / `sdf_luminaire_heterofog` (a blobby SDF luminaire inside homogeneous / heterogeneous scattering fog, with nested interior media — the volumetric-media stress pair). The SDF part lists are authored inline in each scene's `sdf_geometry` chunk (repeatable `part` lines; an external `file` remains available for very large SDFs)
- `SMS/`: specular manifold sampling comparisons and visibility checks,
  including smooth, bump-mapped, and displaced Veach-egg controls
- `Shaders/`: shader-op and rasterizer behavior checks
- `Spectral/`: spectral-lighting and dispersive regression scenes
- `Samplers/`: Sobol, ZSobol, and sampler comparison scenes
- `SubsurfaceScattering/`: focused SSS and PT-vs-BDPT comparison scenes (includes `pathtracing_pel_rasterizer` variants)
- `UnifiedLighting/`: direct-light sampling and many-light regression scenes
- `VCM/`: vertex-connection-and-merging regression scenes (Cornell baseline,
  caustics, spectral, and showcase-composition comparisons)
- `Volumes/`: medium and participating-media validation scenes

## Recommended Quick Checks

- Geometry sanity: `Geometry/shapes.RISEscene`
- CST/parser sanity: `Parser/loops.RISEscene` (a flattened native-v7 fixture;
  the filename and historical header comment predate retirement of the
  streaming loop language)
- Path tracing baseline: `PathTracing/cornellbox_pathtracer.RISEscene`
- Pure PT with OIDN: `SubsurfaceScattering/pt_sss_dragon.RISEscene`
- Spectral baseline: `Spectral/cornellbox_spectral.RISEscene`
- Lighting regression: `UnifiedLighting/cornellbox_mixed_lights_pt.RISEscene`
- Medium correctness: `Volumes/medium_transmittance_test.RISEscene`
- SSS with pure PT: `SubsurfaceScattering/pt_sss_dragon.RISEscene`
- SDF implicit geometry + interop: `SDF/sdf_shadows.RISEscene`

## OIDN Denoising Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SubsurfaceScattering/pt_sss_dragon.RISEscene
```

**Expected**: A visibly denoised SSS dragon. The scene uses
`pathtracing_pel_rasterizer` with `oidn_denoise TRUE`. When OIDN is enabled,
the filtered-film resolve is skipped so OIDN receives raw MC noise (see
`docs/ARCHITECTURE.md`).

**Pure PT rasterizer SSS scenes**:
- `SubsurfaceScattering/pt_sss_dragon.RISEscene` — Dragon with SSS via `pathtracing_pel_rasterizer` + OIDN + path guiding
- `SubsurfaceScattering/pt_sss_wax_sphere.RISEscene` — Wax sphere via pure PT

## Notes For Contributors

- Prefer obvious names such as `baseline`, `guided`, `pt`, `bdpt`, `sms`, or `nosms` when a scene exists to compare variants.
- Keep regression scenes as small and fast as practical unless the point of the scene is explicitly to stress a hard path.
- If a scene becomes visually rich enough to serve as a showcase, keep the regression version here and add a separate curated variant under `FeatureBased/`.
