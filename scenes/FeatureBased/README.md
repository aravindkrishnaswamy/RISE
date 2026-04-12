# Feature-Based Scenes

This directory is the curated showcase suite for RISE. Scenes here should be visually coherent, stress multiple subsystems together, or otherwise earn their place as a "hero" example.

Minimal baselines, comparison scenes, Cornell boxes, filter sweeps, and single-feature regressions now belong under [../Tests/README.md](../Tests/README.md).

## How To Use These Scenes

Build `bin/rise`, set `RISE_MEDIA_PATH` to the repo root, then render a scene by piping commands into the console app:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/Geometry/teapot.RISEscene
```

`run_scenes.sh` renders every `.RISEscene` under this tree.

## Directory Index

- `Animation/`: polished animated showcase scenes rather than basic camera or timing checks
- `BDPT/`: large BDPT showpieces and transport stress scenes
- `Caustics/`: visually rich caustic showcases
- `Combined/`: scenes that intentionally exercise several subsystems together
- `Geometry/`: hero mesh and model scenes, not primitive sanity checks
- `GlobalIllumination/`: heavyweight GI stress scenes
- `MLT/`: visually interesting Metropolis light transport scenes
- `Parser/`: parser-generated showcase scenes
- `PathTracing/`: path-traced showpieces and guided showcase pairs
- `Shaders/`: integrated shader, volume, and SSS showcase scenes

## Recommended Smoke Scenes

- Fastest remaining showcase: `Geometry/teapot.RISEscene`
- Parser showcase: `Parser/pillow.RISEscene`
- Multi-feature gallery scene: `Combined/showroom.RISEscene`
- Caustic showcase: `Caustics/pool_caustics.RISEscene`
- BDPT showcase: `BDPT/bdpt_jewel_vault.RISEscene`
- Path tracing showcase: `PathTracing/pt_jewel_vault.RISEscene`
- GI stress scene: `GlobalIllumination/irradiance_cache_torture.RISEscene`
- Volume or SSS showcase: `Shaders/visiblehuman.RISEscene` or `Shaders/SSS/sss_gi_dragon.RISEscene`

## Recommended Authoring References

- Need a showcase-quality parser example:
  inspect `Parser/kaleidoscope_atrium.RISEscene`, `Parser/photon_cloister.RISEscene`, or `Parser/pillow.RISEscene`.
- Need a stronger multi-system lighting scene:
  inspect `Combined/`, `BDPT/`, or `PathTracing/`.
- Need a focused one-feature example instead of a showcase:
  start in [../Tests/README.md](../Tests/README.md) instead.

## Notes For Contributors

- These scenes should be more than a minimal proof that a feature works. If the scene exists mainly to compare two settings or catch regressions, it belongs in `scenes/Tests`.
- Many are intentionally still renderable in a reasonable time, but visual richness and subsystem coverage matter more here than minimal cost.
- When adding a new user-visible rendering feature, consider landing two scenes:
  a focused regression scene under `scenes/Tests` and a stronger showcase scene here.
- Keep directory names stable. If a category only contains isolated single-feature checks, that category probably belongs under `scenes/Tests` instead.
- A handful of standalone `.RISEscene` files live directly under `scenes/` (e.g. `iorstack`, `povray_colors`, `pr`, `standard_colors`). These are ad hoc or utility scenes, not part of the organized feature suite.
