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
- `BDPT/`: bidirectional path tracing baselines and comparison scenes
- `BSSRDFFurnace/`: energy-conservation regression scenes for subsurface scattering
- `Cameras/`: isolated camera-model checks
- `Caustics/`: compact caustic and SMS comparison scenes
- `Geometry/`: primitive and CSG sanity scenes
- `GlobalIllumination/`: focused GI baselines such as final gather
- `MLT/`: Metropolis light transport baselines
- `Materials/`: isolated material demonstrations and regression scenes
- `Painters/`: painter- and texture-accessor-specific scenes
- `Parser/`: parser-language regression scenes
- `PathTracing/`: unidirectional PT baselines and path-guiding comparisons
- `PixelFilters/`: filter comparison scenes
- `RussianRoulette/`: continuation-probability correctness scenes
- `SMS/`: specular manifold sampling comparisons and visibility checks
- `Shaders/`: shader-op and rasterizer behavior checks
- `Spectral/`: spectral-lighting and dispersive regression scenes
- `StabilityControls/`: sample clamping, bounce limits, RR, and glossy filtering
- `SubsurfaceScattering/`: focused SSS and PT-vs-BDPT comparison scenes
- `UnifiedLighting/`: direct-light sampling and many-light regression scenes
- `Volumes/`: medium and participating-media validation scenes

## Recommended Quick Checks

- Geometry sanity: `Geometry/shapes.RISEscene`
- Parser sanity: `Parser/loops.RISEscene`
- Path tracing baseline: `PathTracing/cornellbox_pathtracer.RISEscene`
- Spectral baseline: `Spectral/cornellbox_spectral.RISEscene`
- Lighting regression: `UnifiedLighting/cornellbox_mixed_lights_pt.RISEscene`
- Medium correctness: `Volumes/medium_transmittance_test.RISEscene`

## Notes For Contributors

- Prefer obvious names such as `baseline`, `guided`, `pt`, `bdpt`, `sms`, or `nosms` when a scene exists to compare variants.
- Keep regression scenes as small and fast as practical unless the point of the scene is explicitly to stress a hard path.
- If a scene becomes visually rich enough to serve as a showcase, keep the regression version here and add a separate curated variant under `FeatureBased/`.
