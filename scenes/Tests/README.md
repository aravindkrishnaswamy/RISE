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
- `Geometry/`: primitive and CSG sanity scenes, plus the object-parenting scene-graph check
- `GlobalIllumination/`: focused GI baselines such as final gather
- `GUI/`: minimal fixture scenes for the Mac GUI's headless `ViewportReattachProbe` (see `build/XCode/rise/RISE-GUI/App/RISEApp.swift`) — two tiny, distinct, fast-rendering scenes (`viewport_reattach_probe_a`/`_b`) used to drive the probe's scene-A-then-scene-B sequence; not meant to be rendered for their own sake
- `Importers/`: glTF import regression scenes (Khronos sample assets, alpha modes, embedded textures, light-control)
- `LightBVH/`: many-light regression scenes comparing alias-table sampling vs. light BVH (corridor 20/100 lights, spotlights stage, BDPT mixed-light Cornell)
- `Lighting/`: physically based unit and environment-map lighting checks
- `Lights/`: environment, mesh-emitter, and analytic-sky comparisons, plus
  `rect_light_sidedness` (the one-chunk area light emits toward `facing`
  only — two identical panels, one facing down and one up, over one floor)
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
- Scene-graph parenting: `Geometry/object_parenting.RISEscene` (an articulated
  arm built from container nodes; the hand's `scale` is authored once and
  inherited by the fingers through composition)
- Scene-graph instancing: `Geometry/object_instancing.RISEscene` (one authored
  four-node lantern, then the three things `source` does with it -- a whole
  SUBTREE clone, a LEAF source that collapses to a single object, and a
  `count_u`/`count_v` array with per-instance `expr(...)`.  42 live objects
  from 7 authored object chunks.)  **This is the scene to open in the GUI to
  check 87's outliner**: the Objects section is a tree, the 30-object array is
  ONE editable row, and picking any copy in the viewport highlights that row.
  Its header comment lists what to look for.
- Surface of revolution: `Geometry/lathe_basic.RISEscene` (the three cases
  `lathe_geometry` exists for -- a VASE whose profile starts and ends at r = 0,
  so both rings collapse to a single pole vertex and the vessel is watertight
  with no caps; a TURNED TABLE LEG with beads, coves and the duplicate-a-point
  hard-edge idiom at its two flat ends; and a 140-degree CUTAWAY, which opens
  the surface along two radial half-planes and gets a flat ear-clipped cap on
  each)
- **Expressive-geometry STRESS scenes** (`Geometry/*_stress.RISEscene`): six
  inspection instruments, not demos.  Every specimen in them exercises a case
  that has actually produced a bug or that the implementation's own comments
  flag as delicate, and each file's header comment says, per specimen, what it
  stresses and what CORRECT looks like -- so a regression is spottable by eye
  from one render.  Each takes a few seconds and leaves `RISE_Log.txt` empty.
  - `Geometry/lathe_stress.RISEscene` -- ten `lathe_geometry` specimens in two
    bands.  The headline is the INTERIOR POLE (a symmetric hourglass whose
    profile touches the axis mid-way): the shared pole vertex is claimed by two
    bands whose outward normals provably disagree in axial sign, which is the
    P1 where `vGeomNormal` ended up pointing INTO the solid near the pinch.
    Also: pole-at-start-only, pole-at-both-ends, the duplicate-a-point hard-edge
    idiom, `smooth TRUE` vs `FALSE` at `n_radial 12`, a level-ended TUBE at
    `sweep_degrees 200` (the `endsLevel` cap rule), a 140-degree solid cutaway
    turned so both ear-clipped caps face the camera, a REVERSED profile beside
    its forward twin (the signed-volume orientation derivation), and one
    specimen each on `axis x` and `axis z` (the frame was only ever pinned
    for `y`).
  - `Geometry/sweep_stress.RISEscene` -- `profile_rect 2.0 1.0 0.5` sitting
    EXACTLY on the stadium boundary `r == min(w,h)/2`, where adjacent corner
    arcs meet at coincident profile points; `path_closed TRUE` on a TREFOIL,
    which has genuine torsion (a planar ring has zero holonomy and proves
    nothing, which is why an earlier closed-loop test was vacuous); a
    `point_scale` round taper beside a `point_width` x-only taper on identical
    paths; a 0.09-radius tube on a three-turn helix; and caps on vs off.
  - `Geometry/skeleton_stress.RISEscene` -- a three-child hub, a DEGENERATE
    bone (`|r_parent - r_child| > bone length`, aimed straight at the camera:
    the case that exposed both an AABB under-bound and an on-axis field error),
    an isolated joint beside an ordinary bone, a nine-joint monotonic taper,
    and `blend 0` beside `blend 1.2` on the same skeleton.  This is the ONE
    stress scene that prints a diagnostic: the degenerate bone intentionally
    trips `skeleton_geometry`'s own non-rejecting warning ON THE CONSOLE.
  - `Geometry/superellipsoid_stress.RISEscene` -- the whole `superellipsoid`
    SDF-part continuum in one frame (ellipsoid, cushion, rounded box, near-box
    beside a plain `box` CONTROL, cylinder, octahedron), plus the three cases a
    broken CONSERVATIVE distance bound breaks first.  The primitive has no
    exact closed-form SDF, so its field is a bound; when a bound over-estimates
    the sphere trace steps THROUGH surface, which shows as speckle and eaten
    silhouettes rather than as an error.  The headline is the OCTAHEDRON PAIR
    at `e1 = e2 = 2`: the exact top of the supported range (past it the solid
    stops being convex and the bound stops holding) and where the inradius
    bound is loosest, authored once in range and once out of range so the
    clamp is visible as two identical objects.  Also: the CYLINDER beside its
    exponent-TRANSPOSED twin (a square-section barrel -- the by-eye proof that
    e1 and e2 have not been swapped), a superellipsoid `smin`-blended with
    roundcones into a creature (the composition that justifies shipping this
    as an SDF part rather than a chunk), an octahedral `subtract` carve, an
    octahedron `intersect`ed with a sphere-exponent twin so that exactly its
    six vertices are sliced into spherical caps (a shape neither part makes
    alone, so a broken clip cannot fake it), and a 5:1 non-uniform scale under
    rotation (where the conservative `min|scale|` factor does the most work).
    This scene prints exactly ONE diagnostic, from the out-of-range clamp
    specimen.
  - `Geometry/scenegraph_stress.RISEscene` -- five levels of nesting with a
    non-identity transform at every one; `source` instancing of a multi-node,
    multi-level subtree; `count_u 5` driving BOTH `position` and `orientation`
    from `expr(i)`; an EMISSIVE node INSIDE the instanced subtree, so each
    clone has to be its own registered area light (three lamps, three separate
    pools on the floor); and a hand-placed TWIN sitting exactly on top of one
    array element as a correctness control -- if the composition drifts, an
    extra blade appears.
  - `Geometry/misc_geometry_stress.RISEscene` -- the two thinnest-covered
    chunks: `cartesian_disk_geometry`, which had ZERO test scenes anywhere
    (a checker disk proving the LINEAR CARTESIAN UV, plus the same displaced
    field at `mesh_n` 300 and 44 so the sampling lattice and the staircase rim
    are visible), and `path_instances_geometry` with a NON-TRIVIAL template
    (a lathe-turned finial, not a sphere) threaded along a curved 3D path,
    with a `slant` + `scale` variant beside it.
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
