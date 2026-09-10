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
- `Materials/`: integrated material showcases (GGX, thin film, glints, enamel, rain-wet coat
  layering -- `rainwet_cobbles.RISEscene`, the docs/WETNESS_COAT_DESIGN.md section 6.5
  `add_wetness` worked example: a `polished_material` cobblestone patch whose pooled
  joints/damp flats/dry crowns come from one `curv`+`occlusion()` mask prelude shared
  byte-for-byte across the albedo/tau/scattering chunks;
  `lacquer_and_rain_still_life.RISEscene`, the Phase-2 `coated_material` hero still
  life: amber-varnished turned wood whose tint deepens at curvature transitions via a
  `curv`-driven `coat_thickness`, a worn oiled-brass box whose `coat_weight` rides
  `occlusion`/`curv` so the coat survives in the recess and wears off handled edges,
  and a wet/dry stone pair whose darkening+saturation come purely from the layered
  transport's per-wavelength recycling -- no `pow(base,k)` anywhere; the wet gleam
  needed a small hard raking light, the documented soft-light-on-mirror-lobe lesson;
  `rainwet_courtyard_night.RISEscene`, the Phase-1 hero: a paver courtyard wetted by
  driving the real `add_wetness` verb (coat-wrap emission, params hand-tuned from the
  emitted min/max) beside a hand-recipe-wetted bollard showing both wetness shapes,
  an authored-dry sheltered alcove (cross-object AO does not exist -- the dryness is
  an authored material choice, said so in-scene), a sunk flat puddle placed by the
  mirror reflection-point construction so it catches the lantern, and three housed
  practicals -- every light in frame has fixture geometry).

  Three more are the `fabric_material` showcase suite (docs/CLOTH_FABRIC_DESIGN.md
  Phase 1, section 9.9 gate 11).  All three are path traced, all three light the cloth
  with a hard grazing key plus a back rim over a dim dome used as LIGHT ONLY
  (`radiance_background FALSE`, so the frame stays black) -- and that rig is not a
  style choice: the Charlie sheen lobe's mass sits at GRAZING half-vectors, so a
  front-on key makes every preset look alike and a dome is the only source that
  supplies a grazing direction at every point of a folded surface at once.

  `velvet_cushion.RISEscene` is section 9.8's worked example as shipped: a buttoned
  velvet cushion with a piped seam whose nap is WORN where the piping stands proud and
  DUSTY where it tucks into the groove.  Its LIGHTING is derived from gate 9's
  measurement rather than copied from the rig: the Charlie lobe peaks at a grazing
  half vector, which needs the light AND the eye both near grazing, so the dome is
  what wraps the fringe round the whole silhouette (directional lights alone render
  this cushion as dark glossy plastic) and the rim is nearly HORIZONTAL -- at 30-55
  degrees elevation `N.L` lands around 0.6 on a top-facing shoulder and no halo
  appears there at all.  The two painters driving `sheen_color` and
  `sheen_roughness` share a byte-identical `curv`+`occlusion()` prelude -- same params,
  same seed, same defs, same order -- which section 9.8 states as a hard requirement
  rather than a convention: let them drift and the tint and the roughness disagree
  about where the seam is.  The body is an `sdf_geometry` (two flat-panelled
  superellipsoid halves welded around a thin welt disc, with a crown tuft subtracted)
  because `curv` is a true differential quantity on an implicit surface, so the wear
  lands on the piping's real ridge line rather than on a tessellation artefact -- and
  because a cushion has to be flat-PANELLED for the wear field to have anything to pick
  out: `curv` on a plain dome is large and positive everywhere and saturates the mask.
  The nap creases are a BUMP map, not a displacement, which is the point: `curv` comes
  from the geometric normal field and is invariant under bump maps, so the creases add
  cloth micro-relief without polluting the wear field.  Substrate is a dark
  `lambertian_material`, not section 9.8's sketched Oren-Nayar, because the shipped
  preset table calibrates `velvet` against Lambertian and warns for anything else.

  `fabric_swatches.RISEscene` is the seven presets on draped swatches, read left to
  right matte to lustrous -- cotton, linen, wool, denim, silk, satin, velvet -- one
  shared displaced panel instanced seven times so the ONLY thing that varies across the
  row is the material.  It is the drape companion to
  [../Tests/Materials/fabric_presets](../Tests/README.md), which puts the same seven on
  spheres: a sphere is the right shape for a controlled regression and the wrong shape
  for showing cloth, whose whole appearance is what the sheen does across a fold.  Each
  preset carries its recommended substrate, and since Phase 2 (2026-09-03) the three
  DIRECTIONAL ones -- denim, silk, satin -- sit on a `weave_material` rather than on an
  anisotropic `ggx_material`.  That swap is the whole content of the phase: gate 9b
  measured the GGX shape, found that 95-99 % of its anisotropy survived being wrapped in
  the isotropic sheen (so the sheen was not washing the direction out), and found it
  still read as brushed metal, because a single elliptical lobe with a painted rotation
  field has no PATTERN SCALE.  A `weave_material` has two thread families with their own
  directions, dyes and fibre lobes, mixed by a weave draft: denim's 3/1 twill wale, silk's
  and satin's 5-harness satin float.  Each carries its authored dye on the WARP and a
  companion tone on the WEFT -- all three drafts float the warp most of the time, so the
  authored colour stays dominant while the weft supplies the second tone a weave has and
  a single lobe cannot.

  `weave_scale` is left at WeavePresets.h's shipped thread-count default for silk and
  satin (8000 / 6000 cells per UV unit -- sub-pixel at this ~1x1.5m swatch framing) and
  overridden to 350 for denim.  Confirmed by rendering and inspecting the PNG this
  session: at the shipped default, silk and satin show their intended broad directional
  float sheen with no visible cell structure at all (this is what a satin identity is --
  the sheen, not the grain), while denim at the SAME default fades to a flat, textureless
  blue -- physically bounded, but missing the fine diagonal grain that is a twill's own
  visual signature even at a distance. 350 was chosen by iterating and re-rendering until
  the wale read as a subtle diagonal texture rather than as printed stripes (an earlier
  revision's 170-190 override, since corrected -- see docs/CLOTH_FABRIC_DESIGN.md 10.3's
  `weave_scale` debt).

  The three painted angle fields survive the swap and moved down one layer: they now bind
  `weave_material.weave_rotation`, which turns both thread families together (the cloth's
  grain), and the `fabric_material` wrappers leave their own rotation slot unwritten,
  because writing the same field on both would ADD the two rotations and turn the yarn
  twice.  The fields remain CONSTANT base angles plus a low-amplitude continuous fbm
  drift.  Built instead from their real section 5.3 cell formulas (`floor(u*N)`,
  `mod(i + k*j, 5)`) they rendered as a BLOCKY CHECKERBOARD -- a cell field is piecewise
  constant, and a narrow lobe is either lit or dark on each side of a cell boundary with
  nothing in between; retuning the per-cell excursion from 0.42 rad to 0.11 rad gave a
  fainter checkerboard of the same size, so the discontinuity rather than the amplitude
  was the defect (docs/SCENE_CONVENTIONS.md section 8.7).  The cell structure now lives
  where it belongs, INSIDE `weave_material`, as a coverage field with smoothed yarn edges
  and a footprint fade to the draft's own mean -- which is what makes it anti-alias
  instead of tile.  Section 5.5's `fw` fade stays on the drift, so the grain direction
  relaxes to its constant base angle under minification.

  `denim_and_satin_drape.RISEscene` is the hero and the subject of section 9.9's
  gate-9b measurement: one hanging cloth seamed down the middle, `fabric denim` left and
  `fabric satin` right.  Under Phase 1 the two halves differed only in sheen roughness
  (0.45 vs 0.12) and substrate anisotropy ratio (1.5 vs 5.7) steered by `weave_rotation`,
  and gate 9b's verdict on that frame was BRUSHED METAL.  Both halves now sit on a
  `weave_material`, so what separates them is what separates the real fabrics: denim's
  3/1 twill draft against satin's 5-harness float, and a broad 14-degree yarn lobe against
  the 2.5-degree flat float that is the tightest in the reference table.

  `weave_scale` differs between the halves, and NOT for the reason an earlier revision of
  this file claimed.  That revision authored 190 (denim) / 310 (satin) and asserted the
  render showed "a diagonal wale across the whole drape" and "a tight, high-contrast float
  band" -- an unverified claim that a fresh render CONTRADICTED (denim read as coarse
  diagonal stripes, satin as a near-black body with a hard highlight; see
  docs/CLOTH_FABRIC_DESIGN.md 10.3's `weave_scale` debt and REVIEW_P2R3.md finding P1-3).
  Measured instead, this session: at WeavePresets.h's shipped thread-count defaults (denim
  2500, satin 6000 cells per UV unit -- sub-pixel at this panel's ~2x2.6m framing) satin's
  half shows the intended broad, high-contrast directional float sheen banding along each
  fold with NO visible cell structure, confirmed by inspecting the rendered PNG. Denim's
  half at the SAME default fades to a flat, textureless blue -- physically bounded, but
  without the fine diagonal grain that is denim's own visual signature even at a distance
  (unlike satin, whose identity is the sheen rather than the grain).  Denim is therefore
  authored at 380, a deliberate hero-close-up override (the kind
  docs/CLOTH_FABRIC_DESIGN.md WeavePresets.h documents as always available): confirmed by
  inspection to put the wale at a few pixels per cell, fine enough to read as fabric grain
  rather than as 190's printed-stripe failure.  The two halves are two instances of one
  panel butt-joined at x = 0 with an INTEGER number of fold cycles in u, so the join is
  C1-continuous and every visible edge down the middle of the frame is a material boundary
  rather than a modelling one -- which is what makes the halves comparable.

  `sheer_curtain.RISEscene` is the Phase-2 slice P2-B (thin-cloth transmission) hero: a
  linen `weave_material` curtain, `transmission thin`, hangs in front of a bright window
  cut into a back wall, with a floor catching the transmitted glow. The camera sees room
  context on both axes -- dark wall framing the opening, a bright sliver of DIRECT window
  light on both sides of the curtain (narrower than the opening from the camera's own
  perspective), the curtain's own soft glow in between, and a warm floor bounce in the
  foreground -- confirmed by rendering and inspecting both the OIDN-denoised (the
  showcase default) and the raw PNG this session; the raw twin confirms the denoiser is
  not inventing the window/fold/wall contrast. Folds come from `fabric_swatches.RISEscene`'s
  own drape recipe (a pinned-top `expression_function2d`, `displaced_geometry` over
  `clippedplane_geometry`) and read through ordinary FRONT-lit shading of the displaced
  surface, not through the transmission lobes themselves -- the delta lobe is a straight,
  undeviated pass-through with no directional dependence on the local fold tangent, so it
  cannot by itself encode "this fold is thinner/thicker"; a room key + rim pair front-lights
  the cloth so the fold geometry reads the way any hanging cloth's would.

  VERIFIED, NOT ASSUMED: three isolated probes (a bright window filling the frame, camera
  facing it directly, EXR output, linear radiance read back per pixel) measured, this
  session: (a) window alone, mean linear radiance 1.273240; (b) the SAME window behind a
  linen curtain at `transmission none`, mean EXACTLY 0.0 (opaque, correct); (c) the same
  again at `transmission thin`, `sheer 0.2`, `warp_transmit`/`weft_transmit` 0.25, mean
  0.259 -- i.e. (c)/(a) = 20.3%, matching the delta lobe's OWN closed-form prediction
  (`sheer x L_window` = 0.2 x 1.273 = 0.2546) almost exactly, with only a small further
  contribution from the diffuse-transmission lobe. An EARLIER pass through this same
  probe reported the diffuse lobe's response scaling close to `transmit^2` (later
  `transmit^1.7`) under PATH TRACING instead of linearly, while BDPT stayed exactly
  linear -- **RESOLVED, docs/CLOTH_FABRIC_DESIGN.md section 15 debt 21**. Two
  overlapping measurement issues produced that finding, neither of which was a
  `weave_material` defect: (1) `file_rasterizeroutput`'s `color_space` parameter
  defaults to `sRGB` even for 32-bit EXR, and an isolated probe that does not
  override it to `Rec709RGB_Linear` reads a gamma-encoded value back as if it were
  linear radiance -- every measurement scene in this table sets that override
  explicitly for exactly this reason; (2) a genuine, smaller PT bug survived even
  with the colour-space artifact removed: `ClippedPlaneGeometry`'s shadow rays were
  spuriously self-shadowed by their OWN originating surface roughly 94% of the time,
  because `RayBilinearPatchIntersection`'s self-intersection epsilon (`NEARZERO`,
  a fixed `1e-12`) was too tight for the FP round-off actually produced at this
  scene's coordinate scale -- fixed by making that epsilon scale-relative (see the
  debt-21 writeup for the full mechanism and why it looked like an MIS-weighting
  bug rather than a geometry one). With both fixed, PT is exactly linear in
  `transmit` and matches BDPT within a few percent at every `transmit` value
  tested; see `tests/FabricRenderTest.cpp::TestAreaLitSheerWeave` for the
  regression guard. PRACTICAL CONSEQUENCE FOR THIS SCENE: `sheer` (the delta lobe)
  and `warp_transmit`/`weft_transmit` (the diffuse lobe) are BOTH reliable, linear
  brightness levers under PT now; `sheer` is tuned to 0.22 (near the requested
  0.15-0.25 range) and `warp_transmit`/`weft_transmit` are left near the linen
  preset's own default (0.30) rather than pushed further, since there is no longer
  a PT-side non-linearity to route around. The separate, still-open BDPT/VCM
  vertex-connection limitation on this material class (debt 20 -- a distinct bug,
  triggered only at short curtain-to-light distance) does not apply to this scene's
  geometry. The window's exitance scale (4.5) was picked
  by reading back the rendered EXR's direct-window and through-curtain pixel values this
  session rather than by eye; `weave_scale` is left at the linen preset's own shipped
  default (sub-pixel at this framing, by design).

  `wool_throw_fuzz` is the docs/CLOTH_FABRIC_DESIGN.md Phase 3 (`add_fuzz`)
  showcase: a rolled wool bolster (an `sdf_geometry` roundbox -- genuinely
  CURVED on every side, unlike a flat panel, whose only "edge" is a
  straight rectangular boundary a normal-grown strand does not cross on its
  own) with `fabric_material wool` over an Oren-Nayar substrate, plus a
  fuzz shell -- a hair_geometry/hair_material/standard_object triad grown
  on the SAME geometry, the exact triad shape `add_fuzz` mints -- under a
  LIGHT studio dome (`radiance_background TRUE`, ~0.36, velvet_cushion
  .RISEscene's own rig) with a strong rim light behind-and-above. An
  earlier revision used a flat displaced panel against a black background
  and read as plain cloth with no visible fringe at all -- two compounding
  mistakes the Phase 3 evaluation's own findings predicted: a flat
  silhouette gives a normal-grown strand nothing to cross on its own, and
  the evaluation's own `D_brightbg` probe found the fringe reads AGAINST a
  bright background and nearly vanishes against black. Fixing both at once
  (curved subject, light background) produces an unmistakable, clearly
  visible fibrous halo along the entire silhouette. The fuzz recipe is
  hand-tuned rather than run through `add_fuzz`'s own auto-scaling (kept as
  a single inspectable file); `add_fuzz`'s density model itself was fixed
  this same round to read each object's REAL surface area
  (`IObject::GetArea()`) rather than an equivalent-sphere estimate off its
  bounding box, specifically so it no longer over-mints a flat object the
  way an earlier revision of this scene's own fuzz shell did.
- `Parser/`: parser-generated showcase scenes
- `PathTracing/`: path-traced showpieces and guided showcase pairs
- `SDF/`: visually rich signed-distance-field stress scenes
- `Shaders/`: integrated shader, volume, and SSS showcase scenes
- `Textures/`: the doc-88 procedural-texture arc (Phases 1+2) showcase suite -- four scenes,
  each earning its keep on different mechanisms.

  `weathered_workbench.RISEscene` is the composition hero: a wooden workbench whose top
  is driven by ONE `expression_painter` grain field consumed twice -- a `ramp_painter`
  for wood colour and a `scalar_painter{painter}` bridge for GGX roughness, so gloss
  follows grain. `scatter_painter` scatters oil-stain stamps that a `blend_painter{mode
  multiply}` darkens onto the wood. A bolted-on cast-iron bench vise -- a fifteen-part
  `sdf_geometry` whose silhouette (base flange, fixed and sliding jaws, proud machined jaw
  pads, lead screw, T-handle) says what it is before any painter touches it -- carries ONE
  weathering field consumed four ways: `ramp_painter` for the paint-to-rust colour, a
  `scalar_painter` bridge to GGX roughness, the same field raw into `relief_modifier`, and
  two more bridges into `ior`/`extinction`, so the field that says "this point is paint"
  also stops it being a conductor; `occlusion()`, `-N.y`, `curv` and an object-space part
  mask decide where the grime, rust, chips and machined faces go. Since 2026-09-08 the
  bench TOP also carries `proximity(0.02)` -- the cross-object signal -- so grime collects
  where the vise's flange actually rests on it; the header used to say that band was "out
  of reach ... `occlusion()` is self-occlusion only", and that retraction is written into
  it. A tool tray demonstrates
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

  `plank_closeup.RISEscene` is the answer to "these painters look generated, not organic":
  a 0.35 m close-up of the corner of a rough-sawn softwood plank with a hand-forged cut
  nail, authored under a seven-rule doctrine written into its header (camera-first
  feature sizes, structure before noise, anisotropy, geometry signals, rare events, one
  field driving colour + roughness + relief, raking light) and distilled into
  `skills/agent/procedural-textures.md`. Growth rings are built from the distance to a
  tilted off-board pith axis with an asymmetric early/late sawtooth and a per-ring
  `cellhash` season, noise only warps them, a single hand-placed knot deflects them,
  and `relief_modifier` carves the same field the `ramp_painter` colours. The plank is
  an `sdf_geometry` so `occlusion()` finds real dirt in a subtracted end check. Since
  2026-09-08 it is also the flagship for the CROSS-OBJECT signal: `proximity(0.002)` --
  a 2 mm world radius, unlike `occlusion`'s per-object fraction -- puts contact grime
  where the nail actually touches the board. Which is not along the shank: the head disc
  is thicker than it is, so the nail rests on its buried tip; its head rim nearly rests
  (0.146 mm clear) and the shank lifts from the tip to 3.5 mm at the head. The header
  records the gaps (0.146 mm clear at the rim, 3.51 / 1.31 / 0.00 mm along the shank,
  0.55 mm of burial at the tip) measured with the signal itself. That fade
  is what no ambient occlusion could draw, which is why cross-object AO was prototyped
  twice and declined twice before this.

  `tidal_stones.RISEscene` is the showcase for `interior(r)`, the SIGNED half of the
  cross-object channel `proximity(r)` shares: five stones (a sphere, a yawed ellipsoid,
  an `sdf_geometry` pebble, a sphere buried in dry sand, and a tilted flagstone) sit in
  a 0.36 m water box whose top face is FLUSH with the surrounding sand -- by design, so
  the only visible tell is the wet stones themselves, not a dramatic pool edge.
  `buried = interior(0.02)` is declared three times (a `def` does not cross chunks) and
  drives all THREE of a shared `coated_material`/`ggx_material` recipe's wetness-linked
  slots at once: `rd` mixes a dry stone colour toward a dark wet one, `alphax`/`alphay`
  fall as the surface gets wetter (a real GGX glint under the key light, not a fixed
  specular term), and `coat_weight` is the wetness fraction itself. Two of the five
  stones (the plain sphere and the flagstone) are RECEIVERS whose wet ramp is read by a
  purpose-built harness: the flagstone's own top face crosses the waterline at a shallow
  angle from the beauty camera, well outside the signal family's 10 deg
  refraction-displacement rule, so its ramp is read from a dedicated OVERHEAD probe
  camera instead of the beauty one. `transparent_shadows TRUE` is mandatory (recorded in
  the header) so every submerged station's NEE shadow ray can refract out through the
  water rather than being blocked outright by the default binary occlusion test.
  `tests/TidalStonesShowcaseTest.cpp` re-derives every station from the scene's own
  chunk geometry (never a number copied from a render) and proves the painter ramp
  against an in-process probe/control pair built entirely through `Cst::DocSetOrAddParamValue`.
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
