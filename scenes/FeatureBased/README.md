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
- `GeometrySignals/`: the canonical showcase for the geometry-derived shading signals
  (curv/curvR, occlusion(r), thickness(r)) from docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md.

  `weathered_reliquary.RISEscene` is "The Weathered Reliquary" -- a dusk-lit museum
  tableau on one carved stone plinth, path traced (the signals are fully correct only
  under PT; BDPT/VCM/MLT evaluate them as their neutral fallback in parts of their
  transport). Five things, each proving one facet of the design doc: (1) a bronze
  guardian idol (`skeleton_geometry`, an 18-joint creature SDF) whose base colour,
  metallic AND roughness are all driven by ONE shared `curv` + `occlusion()` wear
  field -- bright polished highlights on convex masses, deep patina in genuinely
  enclosed folds (occlusion, not curv alone, is what tells an armpit crease from a
  shallow dimple) -- the exactness showcase, since curv is a true differential
  quantity on an implicit surface, not a mesh-faceted approximation; (2) a carved
  stone plinth (`sdf_geometry`, box + roundbox CSG subtraction) where soot pools in
  the recessed panels and grooves via `occlusion()` while the swept flat faces stay
  clean -- the CSG-subtraction cavity path; (3) an alabaster votive shell
  (`sdf_geometry`, sphere-minus-offset-sphere) with a genuinely varying wall --
  thin at the rim, thick at the base -- where `1 - thickness(r)` drives BOTH the
  reflectance and transmittance of a `translucent_material`, lit from within by a
  small warm point light, so the thin rim glows and the thick base stays opaque;
  isolated (light-only, no external key) this renders as an unmistakable bright-top
  fading-to-dark-bottom gradient -- the thickness showcase and the scene's emotional
  centerpiece; (4) three scales of the SAME guardian geometry (1.0 / 0.5 / 0.3)
  sharing one material, proving `curv`'s world-scale fold and `occlusion()`/
  `thickness()`'s dimensionless object-space radius make the wear pattern land in
  the same anatomical places at every scale -- corresponding features across the
  three instances measured within 0.06 of each other; (5) a lathe-turned votive
  goblet (`lathe_geometry`, baked to a real `ITriangleMeshGeometryIndexed`)
  exercising the MESH per-vertex signal bake -- edge wear via curv (faceted here,
  only as good as the vertex normals) and crevice dirt via `occlusion()` at a
  LITERAL radius (a mesh cannot answer a computed one). Every art-directable number
  is a `param` with min/max/step/label; the header comment doubles as a reading
  guide to the five demonstrations.
- `GlobalIllumination/`: heavyweight GI stress scenes
- `GuillocheWatch/`: thin-film guilloché watch showcase and authored variants
- `Hair/`: the hair/fur showcases -- one hero, one variety plate, one macro.

  `cottontail_dusk.RISEscene` is a rabbit crouched in dry late-summer grass, shot into a
  low Hosek-Wilkie sun placed almost directly behind it so the whole coat is rimmed by the
  Chiang/Yan TT lobe. It is the showcase companion to the five focused regressions in
  [../Tests/Hair](../Tests/README.md), which prove one axis each; this scene runs them all
  at once on a real subject. The body is a 30-joint `skeleton_geometry` (aspect-flattened
  ears, tucked forepaws, a cottontail puff) that the coat then grows on. The coat is a
  TWO-LAYER groom -- 420k fine, heavily-medullated undercoat fibres under 110k longer,
  coarser, thinner-medulla guard hairs -- because one layer alone reads as felt. Its
  regions are stated in the animal's own anatomy rather than in a UV chart: `hair_geometry`
  evaluates `density` and `length_painter` at each candidate ROOT with the root's object
  position and normal in scope, so a `scalar_painter { expression ... }` shortens the coat
  over the head, thins it on the ears until the pink membrane shows through, and carves the
  two eye beads and the nose leather out of the groom so no hair grows across them.
  `hair_material`'s melanin ports take painters too, giving countershading, a bleached ring
  around each eye, and an agouti tick on the guard coat. Whiskers are a third groom on a
  sphere buried inside the muzzle -- an unrendered base whose only visible product is the
  fibres that push out through the flesh -- masked by the root NORMAL onto the whisker pads.
  The meadow is the same generator three more times on flat `cartesian_disk_geometry` bases,
  using the other colour tier (Tier 3 `color`) and reading each blade's own height above its
  disk to run green at the root and straw at the tip. Shot on a `thinlens_camera` at
  100 mm / f4 focused on the eye. `oidn_denoise FALSE` is the measured answer, not a
  default -- [../../docs/HAIR_FUR_DESIGN.md](../../docs/HAIR_FUR_DESIGN.md) section 6.5
  found OIDN removes real strand structure on a groom and the deficit does not close with
  sample count -- so samples carry the convergence instead.

  `variety_gallery.RISEscene` is the breadth plate: NINE identical turned milliner's blocks
  (one `lathe_geometry`, instanced nine times -- rotational symmetry makes "identical forms"
  true by construction) on three staggered risers under one unchanged three-panel rig, each
  wearing a different groom, so every visible difference is `hair_geometry` and
  `hair_material` and nothing else. Jet-black straight, golden blond, copper ringlets,
  afro-textured coils, silver-grey, a two-layer fox guard coat, angora undercoat, coarse
  boar bristle and crimped sheep fleece -- ordered for adjacency contrast, so the
  multiple-scattering glow of eumelanin 0.28 sits directly beside the near-total absorption
  of 4.5. All THREE of `hair_material`'s colour tiers are on the plate under the same light:
  Tier 1 melanin on seven specimens, Tier 2 `sigma_a` on the silver (the only way to ask for
  a genuinely achromatic fibre, since both pigments are coloured), and Tier 3 artist `color`
  on the fleece. `medulla_ratio` is the hair-vs-fur axis: 0 on the three human grooms, 0.90
  on the angora, which is why that one reads as velvet rather than as strands. Two shared
  `scalar_painter { expression ... }` coverage fields -- a hairline for the five hair
  specimens, a whole-block pelt for the four fur ones -- do most of the work of separating
  the two families, and both are correct for all nine placements because a groom's root
  painters see the BASE geometry's object space, not the `standard_object` transform.
  It also records a measured limitation in its header: on a surface of revolution (and on an
  `sdf_geometry`'s cylindrical wrap) `comb`'s tangent frame is AZIMUTHAL, so a comb field
  flares and parts hair but cannot sweep it down the form -- `gravity` is the only drape
  lever, and `hair_guides` would not help because a guide is replayed in the same frame.

  `dandelion_clock.RISEscene` is the macro: a seed head into a low sun with two seeds
  already adrift. It is the one scene in the folder framed tightly enough that INDIVIDUAL
  FIBRES RESOLVE -- 100 mm at 0.42 m puts the frame at ~0.10 mm per pixel, so an 0.08 mm
  pappus filament finally lands on the sensor as a line rather than as part of a converged
  mass. The head is two grooms: 900 tan medullated spokes on the rendered receptacle, and
  22 000 near-white filaments grown on an invisible 17 mm shell sphere that is never bound
  to a `standard_object`, which is what puts the parachutes in a band around the head
  instead of packing them into the middle. It is also the folder's counter-example on strand
  budget: the first pass used 260 000 filaments and rendered a featureless white ball, and
  the structure only came back at 22 000 -- when a groom is meant to be SEEN as fibres
  rather than felt as a mass, fewer is the correct direction. High key on purpose (the
  backdrop wash faces away from the subject), because a one-pixel filament staying legible
  against a bright ground is the harder test.
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
