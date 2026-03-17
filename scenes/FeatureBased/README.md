# Feature-Based Scenes

This directory is the canonical sample suite for RISE. It functions both as user-facing examples and as lightweight end-to-end coverage for the scene language and renderer.

The historical manual already points users at these files. This guide turns the directory into an index for contributors and agents.

## How To Use These Scenes

Build `bin/rise`, set `RISE_MEDIA_PATH` to the repo root, then render a scene by piping commands into the console app:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/Geometry/shapes.RISEscene
```

`run_scenes.sh` renders every `.RISEscene` under this tree.

## Directory Index

- `Animation/`: temporal sampling, field rendering, exposure, animated caustics, animated translucent scenes
- `Cameras/`: fisheye, thin lens, and realistic camera variants
- `Caustics/`: photon-mapped and dispersive caustic setups
- `Geometry/`: analytic primitives, mesh loading, bezier and bilinear patches, CSG
- `GlobalIllumination/`: final gather and path tracing examples
- `Materials/`: dielectric, dispersion, skin, composite, and general material demonstrations
- `Painters/`: procedural, texture, spectral, and blackbody-driven painters
- `Parser/`: parser-oriented examples such as loops and generated structure
- `PixelFilters/`: filter comparisons
- `Shaders/`: shader ops, irradiance cache behavior, area lights, transparency, and visible human scenes
- `SpectralRendering/`: spectral-only or spectral-heavy render paths

## Recommended Smoke Scenes

- Fast geometry sanity check: `Geometry/shapes.RISEscene`
- Parser behavior: `Parser/loops.RISEscene`
- Final gather coverage: `GlobalIllumination/cornellbox_fg.RISEscene`
- Path tracing coverage: `GlobalIllumination/cornellbox_pathtracer.RISEscene`
- Irradiance cache and shader behavior: `Shaders/dt_with_irrcache.RISEscene`
- Spectral path: `SpectralRendering/cornellbox_spectral.RISEscene`
- Animation path: `Animation/basic_animation.RISEscene`

## Recommended Authoring References

- Need a chunk example for a specific material, painter, or shader op:
  start with the matching directory here before diving into parser code.
- Need a parser language example:
  inspect `Parser/` scenes first.
- Need spectral examples:
  inspect `SpectralRendering/` and `Materials/spectral_skinmodel.RISEscene`.

## Notes For Contributors

- These scenes are primarily teaching and coverage assets, not necessarily final-quality production settings.
- Many are intentionally configured to render in a reasonable time; expect to increase samples and photon counts for cleaner results.
- When adding a new user-visible rendering feature, prefer adding one focused scene here rather than burying the only example elsewhere in `scenes/`.
- A handful of standalone `.RISEscene` files live directly under `scenes/` (e.g. `iorstack`, `povray_colors`, `pr`, `standard_colors`). These are ad hoc or utility scenes, not part of the organized feature suite.
