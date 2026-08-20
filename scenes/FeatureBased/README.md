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
- `EnamelWatch/`: the complete vitreous-enamel watch hero
- `Geometry/`: hero mesh and model scenes, not primitive sanity checks
- `GlobalIllumination/`: heavyweight GI stress scenes
- `GuillocheWatch/`: thin-film guilloché watch showcase and authored variants
- `MLT/`: visually interesting Metropolis light transport scenes
- `Materials/`: integrated material showcases (GGX, thin film, glints, enamel)
- `Parser/`: parser-generated showcase scenes
- `PathTracing/`: path-traced showpieces and guided showcase pairs
- `SDF/`: visually rich signed-distance-field stress scenes
- `Shaders/`: integrated shader, volume, and SSS showcase scenes
- `Textures/`: the doc-88 procedural-texture arc (Phases 1+2) showcase suite -- three scenes,
  each earning its keep on different mechanisms.

  `weathered_workbench.RISEscene` is the composition hero: a wooden workbench whose top
  is driven by ONE `expression_painter` grain field consumed twice -- a `ramp_painter`
  for wood colour and a `scalar_painter{painter}` bridge for GGX roughness, so gloss
  follows grain. `scatter_painter` scatters oil-stain stamps that a `blend_painter{mode
  multiply}` darkens onto the wood. A clamped-on iron vise is worley-pitted, with its
  colour field reprojected via `mapping_painter{projection world}` and its roughness a
  standalone `scalar_painter{expression}` worley field. A tool tray demonstrates
  `stochastic_tile_painter` de-tiling a `checker_painter` source -- no tileable photo/scan
  asset exists in-repo, so the scene substitutes a procedural checker source to exercise
  the de-tiling mechanism; `stochastic_tile_painter`'s intended use is a small tiling
  photo or scan, per the skill.

  `oxidized_copper.RISEscene` exercises per-channel scalars, blend modes, and the
  `voronoi3d_painter` `space` parameter on two copper spheres sharing one material.
  A vec3-typed `scalar_painter{expression}` (`HasPerChannelVariation`) drives the
  material's `extinction` slot directly with three independent per-channel mixes,
  shifting the conductor Fresnel colour between clean copper and a duller patina tint --
  no colourspace conversion, no JH uplift. A second vec3-typed `expression_painter`
  feeds `alphax`/`alphay` from its R and G channels through two separate
  `scalar_painter{painter channel}` bridges, producing anisotropy that visibly follows
  the field. `voronoi3d_painter{space world}` scatters verdigris patina cells across
  world-space coordinates spanning both sphere centres, so the SAME painter bound to
  both objects reads as one continuous patina field carved through both of them rather
  than two independent per-object patterns; `blend_painter{mode overlay}` composites
  the patina over the clean-copper base.

  `receding_pier.RISEscene` demonstrates the S9 ray-differential texture footprint: a
  long plank deck, tessellated via `displaced_geometry` (zero displacement -- used
  purely for the unconditional bake to a real `ITriangleMeshGeometryIndexed`, since
  `box_geometry`/`infiniteplane_geometry` alone are analytic and never populate
  `txFootprint`), viewed from a low, grazing pinhole camera. Its `expression_painter`
  plank texture runs a high-frequency `fbm` field along the receding (Z) axis; `fbm`
  fades its own octaves against the per-sample world-space filter width `fw`, which
  grows with distance, so the render shows crisp wood grain within a few metres of the
  camera and a smooth, shimmer-free fade toward the horizon with no extra code.
- `VCM/`: vertex-connection-and-merging showpieces.
  `vcm_sdf_luminaire_jellyfish.RISEscene` is held as a transport torture test rather
  than a picture: emissive SDF geometry inside a dielectric bell inside a scattering
  medium, under a displaced dielectric water surface. It exercises the
  `CanBeAreaLight` guard in both directions in a single load (two SDF emitters
  area-sample and become NEE lights, one provably cannot and falls back to
  BSDF-hit-only emission), and PT and VCM disagree on it by ~2.2x in frame mean with
  VCM's speckle unresolved at 1024 spp. Its header carries the full provenance,
  the reconstruction note, and the measured A/B.

## Recommended Smoke Scenes

- Fastest remaining showcase: `Geometry/teapot.RISEscene`
- Parser showcase: `Parser/pillow.RISEscene`
- Multi-feature gallery scene: `Combined/showroom.RISEscene`
- Caustic showcase: `Caustics/pool_caustics.RISEscene`
- BDPT showcase: `BDPT/bdpt_crystal_garden.RISEscene`
- Path tracing showcase: `PathTracing/pt_jewel_vault.RISEscene`
- GI stress scene: `GlobalIllumination/irradiance_cache_torture.RISEscene`
- Volume or SSS showcase: `Shaders/visiblehuman.RISEscene` or `Shaders/SSS/sss_gi_dragon.RISEscene`

## Recommended Authoring References

- Need a showcase-quality parser example:
  inspect `Parser/kaleidoscope_atrium.RISEscene`, `Parser/photon_cloister.RISEscene`, or `Parser/pillow.RISEscene`.
- Need a stronger multi-system lighting scene:
  inspect `Combined/`, `BDPT/`, `PathTracing/`, or `VCM/`.
- Need a focused one-feature example instead of a showcase:
  start in [../Tests/README.md](../Tests/README.md) instead.

## Notes For Contributors

- These scenes should be more than a minimal proof that a feature works. If the scene exists mainly to compare two settings or catch regressions, it belongs in `scenes/Tests`.
- Many are intentionally still renderable in a reasonable time, but visual richness and subsystem coverage matter more here than minimal cost.
- When adding a new user-visible rendering feature, consider landing two scenes:
  a focused regression scene under `scenes/Tests` and a stronger showcase scene here.
- Keep directory names stable. If a category only contains isolated single-feature checks, that category probably belongs under `scenes/Tests` instead.
- The root-level `pr.RISEscene` is the CLI's no-argument runtime default, not
  part of the showcase taxonomy.
