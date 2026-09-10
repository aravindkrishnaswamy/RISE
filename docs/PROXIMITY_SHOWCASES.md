# Cross-object signal showcases — three composed scenes for `proximity(r)` and `interior(r)`

Status: SPEC, 2026-09-10, revised twice the same day after adversarial rounds
(round 1, 8 P1s: the pavilion's shipped camera frames none of its caps; a
station inside the column; the pool's floor logic inverted; bound claims on
receivers whose containers are exact boxes; a "dry" stone inside the pool; a
`clamp(N.y)` control that is 0 on a sphere's flank; a shelf station with no
closed form; "1e-9" on a composite. Round 2, 6 P1s: the new camera's
elevation was 17°, its lookat inside the column and nothing else in frame; a
station coordinate 9 cm off its stone; the probe renders would have captured
the denoiser; underwater painter stations are unreadable through refraction).
Companion to `docs/CROSS_OBJECT_PROXIMITY_DESIGN.md` (the engine; §8.2 has
the station protocol, §8.4 what Phase 3 built, and `ProximitySignalTest` (l)
already pins the pavilion's cap-top stations against the tracked scene).
Phase 1 shipped two showcases (`plank_closeup`, `weathered_workbench`);
Phases 2 and 3 shipped fully tested machinery with no beauty scene an author
can open. This document specifies the three scenes that fill that gap, each
with stations predicted from the geometry BEFORE the scene is composed,
measured by a harness test against the TRACKED scene, and recorded in the
scene's header.

Every rule below inherits the doctrine in `skills/agent/procedural-textures.md`
(camera-first feature sizes, structure before noise, one field driving colour
and roughness, raking light) and the standing conventions: never over-read
contact, world-length radii, no `DynR` twin for either signal. The signal is
stamped on the winning record in `ObjectManager::IntersectRay`, so every
rasterizer that routes hits through it sees it — including the legacy
`pixelpel_rasterizer` with `DefaultDirectLighting` (verified in review;
`WarnIfNonPTRenderHasLiveSignalConsumer` is called only by the BDPT/VCM/MLT
rasterizers). The painter stations below prove it anyway.

## 0. Shared gate protocol

- **Query stations**: a harness test (`tests/<Showcase>Test.cpp`) loads the
  TRACKED scene through the normal CST path (`Cst::ParseToCst` +
  `Cst::DeriveToJob`, as `MeshClosestPointTest` (g) does), takes the
  `IObjectManager`, and asks `NearestOtherSurface` / `DeepestOtherContainment`
  at world points it DERIVES from the scene's own chunks — never hard-coded
  answers copied from a previous run. Where a station is defined in an
  object's LOCAL frame (a rotated composite) the test pushes the local point
  through that object's `GetFinalTransformMatrix()`, as `ProximitySignalTest`
  (l) does, so the test cannot disagree with the engine about a convention.
- **Tolerances by the CONTAINER/NEIGHBOUR family, not the receiver's**: a
  box or sphere neighbour is exact → 1e-9 (which also absorbs a station that
  lands an ulp on either side of a face by rounding — e.g. a water top that
  computes to 0.02999999999999997). A CSG composite is never exact (design
  §5.6 as corrected in §8.4) → the band `ProximitySignalTest` (l) uses at
  the 1 cm station: `≤ 0.5 + 1e-9` and `≥ 0.4874 − 1e-4` (a `+1 ulp`
  landing miss sends the bracket to its probe; ε = 5e-5 × 5.0498 = 2.52e-4;
  `0.5 − ε/0.02` = 0.4874). A mesh neighbour is exact per triangle but the
  station's TRUE distance has no closed form → the geometry's band, stated.
- **Painter stations**: the same test renders a PROBE copy of the scene and
  a CONTROL copy in-process and reads the pixel ratio under each station.
  Mechanism (all existing API): `Cst::ParseToCst` the tracked file →
  `Cst::DocSetParamValue(doc, chunkId, "def", occurrence, …)` to replace the
  receiver's signal `def` (probe: the raw signal; control: the constant 1)
  and to set `oidn_denoise FALSE` on the rasterizer chunk (MANDATORY: the
  default is TRUE and a denoised probe smears the band; `SourceHygieneTest`
  hard-fails any test defining an `IRasterizerOutput` without the literal
  `oidn_denoise false`) → `Cst::DeriveToJob` → `Job::RemoveRasterizerOutputs()`
  + `GetRasterizer()->AddRasterizerOutput(capturing output)` + `Job::Rasterize()`
  (the `EnvLightBalanceTest` pattern; values are linear doubles, no sRGB).
  Relief modifiers stripped; every material a black Lambertian except the
  receiver and, in showcase 2, the water (which must stay a dielectric —
  see §2). 64 spp on both copies (the two renders seed independently from
  the wall clock, so their per-pixel ratio does not cancel AA jitter; the
  ±0.08 band and 64 spp absorb it). The pixel footprint (mm) is stated; where
  the footprint is coarser than the station spacing the query station
  adjudicates and the pixel is a sanity check with a stated wide band. A
  painter station is read ONLY where the sightline from the probe camera to
  the station is unoccluded and, if it crosses a refractive interface, is
  within 10° of that interface's normal (so the refractive displacement is
  under a pixel — the header shows the angle).
- **Framing rule**: a receiver's 2 cm ring must image at ≥ 8 px across in
  the RADIAL direction (tangential size × sin(elevation)), its face seen at
  ≥ 25° elevation, and the receiver inside the depth of field at a
  1-pixel circle of confusion; the header shows the arithmetic (distance,
  focal length, sensor — `sensor_size` is the HORIZONTAL extent — f-stop,
  focus distance, DoF limits). The camera must not lie inside any solid.
- **Beauty**: the scene renders at its shipped settings; a crop around the
  contact is written under `rendered/` (gitignored) and judged in the header
  honestly (what the signal drew, what it did not).
- **Costs**: the scene's wall clock with the signal live vs the same scene
  with the signal's `def` replaced by the constant 0 in EVERY chunk that
  declares it (the expression VM has no constant folding, so the rest of the
  program still executes and the delta isolates the signal call and its L1
  memo lookup), `render_thread_reserve_count 0`, 2 warm-ups then ≥3
  interleaved pairs, every run listed. Target ≤ 1.15× (the plank's
  1.11–1.13× is the reference). The header names the CALL RATE regime: a
  `pixelpel_rasterizer` scene evaluates the painter about once per camera
  sample, a path-traced scene many times more — the three figures are not
  comparable with each other and §8.5 says so.
- **Ledgers**: `CstDeriveGoldenTest` gains one row per NEW scene (no tracked
  scene is edited by this spec, so no existing row drifts); the
  `scenes/FeatureBased/README.md` entry follows the plank entry's shape; the
  scene header records every measured number and every deviation from this
  spec; §8.5 of the design doc gets one paragraph per showcase pointing here.

## 1. `pavilion_colonnade` — contact dust at the foot of a fluted column (Phase 3, CSG neighbour)

**Why a new scene.** `glass_pavilion`'s shipped camera (85 mm at
(−3.5, 2, 6), f/1.4 focused at 7 m) frames NONE of the four caps (cap1/2/3
are outside even the 14.8° half-diagonal; cap4 at 10.6 m is defocused to
5.6 px while its 2 cm ring images at 3.6 px, under a pixel at 10° grazing).
The pavilion keeps its subject and its `capped TRUE` pin; the signal gets a
foot study of its own.

**New scene** `scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene`,
built from the pavilion's chunks (the same `colcylgeom`, `flutegeom`,
`capgeom`, `column1` at (−2.5, 2.5, 2.5) un-rotated, the floor, the ceiling
and back wall for bounce, the two omni lights), framed on cap1's foot with
the vase brought into the mid-ground so the frame is not only marble:
`vase` relocated to (−1.75, 0.15, 2.05) (the worker may move it within the
frame but never onto a station's sightline). Camera, verified in review
against the framing rule:
```
thinlens_camera
  location        -2.5    0.9452  3.8407
  lookat          -2.4111 0.175   2.7443      # the S1 station
  up              0 1 0
  sensor_size     36
  focal_length    50
  fstop           16                          # DoF [0.98, 2.12] m holds the whole cap
  focus_distance  1.343
```
Arithmetic: camera→S1 horizontal 1.100 m, Δy 0.770 → elevation 35.0°;
distance 1.343 m → a 2 cm ring images at 16.6 px tangential, 9.5 px radial
(22.22 px/mm on the 36 mm/800 px sensor); at f/16 with a 1-px (0.045 mm)
circle of confusion the DoF is [0.98, 2.12] m, holding S1 (1.343), S7
(1.377), S6 (1.578) and the cap's far corner (1.891). The camera is 1.341 m
from column1's axis, above the floor, outside cap1's footprint. `pixelpel_rasterizer`
as the pavilion, 64 spp, `oidn_denoise` at the pavilion's setting for the
beauty render (the probe copies force it off).

**Receiver.** One new material `marble_cap` on `cap1` (the only cap in the
new scene; the scene may also carry `cap2..cap4` and their columns for the
bounce light, bound to the same material, but the frame holds cap1): a
`lambertian_material` whose reflectance is an `expression_painter` —
`def dust proximity(0.02)`; `expr mix(vec3(marble white), vec3(0.42, 0.36,
0.30), clamp(dust*0.85 + 0.08*fbm(P*180.0, 3, 0.5, 2.0), 0, 1))` with the
pavilion's marble white (no `jitter` — it is not a context variable; a
`seed` line plus `def jitter vec3(seed, seed*1.7, seed*2.3)` is the wear
prelude's way if breakup jitter is wanted). No relief, no roughness. A
`def` is per chunk.

**What the picture shows.** The band follows the COMPOSITE's cross-section
(the design's §8.4 crop): an outer ring hugging the round wall, BROKEN in a
wedge ~8 cm wide at the slot mouth (from a point 1 cm outside the mouth the
nearest real surface is the slot-wall/cylinder corner, 4.21 cm away), and
two thin bands INSIDE the mouth hugging the slot walls (at |x| = 0.03 the
wall is 1 cm away → 0.5). A cylinder neighbour would draw an unbroken ring;
the break-plus-inner-bands is the tell that the query walked the composite.
The scene inherits the pavilion's pre-existing interpenetrations (the cap
sits 7.5 cm into the floor box, the column base inside both); at r = 0.02
none reaches a station, and `interior` is not used here.

**Stations** (cap1's top face y = 0.1 + 0.075 = 0.175; column1 axis at
(−2.5, ·, 2.5), un-rotated; slot faces at local z = ±0.25, slot half-width
0.04, mouth wedge half-angle asin(0.04/0.25) = 9.21°; the slot is open at
the bottom, `flutegeom` height 5.2 > 5, so the cap top inside the mouth is
exposed). Query radius 0.02 unless stated. `ProximitySignalTest` (l) pins
the 90°-azimuth wall stations, the flute refusal and the corner on the
tracked pavilion; this test re-asserts the equivalent stations on the new
scene and ADDS S6, S7, the painter stations and the cost.
- S1 the wall at 20° azimuth from the mouth (NOT 90°: from this camera the
  90° station's sightline grazes the cylinder with 3.9 mm clearance, a
  contaminated pixel): local (0.088924, y, 0.244275), 1 cm outside the round
  wall (|x| = 0.0855 > 0.04, so the nearest surface is the wall, corner
  4.9 cm away) → world (−2.411075, 0.175, 2.744320) → 0.5 within the
  composite band; the sightline's closest approach to the column IS the
  station (unoccluded).
- S2 2 cm out on the same azimuth (radius 0.27) → 0; S3 4 cm out (0.29) → 0.
- S4 flute mouth local +z, 1 cm out: (−2.5, 0.175, 2.76) → 0 on the scene
  query at r = 0.02; the per-object `column1->DistanceToSurface` at r = 0.1
  REFUSES (the scene-wide query at 0.1 answers 0.075 from the floor top at
  y = 0.1 — the cross-check (l) uses).
- S5 (only if the scene carries the rotated `column2`) local (0.088924, y,
  0.244275) and local (0, y, 0.26) pushed through
  `column2->GetFinalTransformMatrix()` → 0.5 (band) and 0.
- S6 cap1 top, 6.5 cm from the wall = 0.315 from the axis along local +x:
  (−2.185, 0.175, 2.5) → 0 (nothing within 2 cm; the cap's half-width 0.35
  keeps it on the cap; the floor top is 7.5 cm below); sightline clears the
  column at 0.305 m.
- S7 inside the slot mouth on the exposed cap top: local (0.03, y, 0.20) →
  world (−2.47, 0.175, 2.70): inside the cylinder radius (0.2022 < 0.25),
  inside the slab (|x| = 0.03 < 0.04), so the composed field is
  `max(f_A, −f_B)` = +0.01 (outside the composite) and the nearest composite
  surface is the +x slot wall 1 cm away → 0.5 within the composite band
  (the boundary arm fires on the box's exact face; verified in review). The
  sightline enters the cylinder's disc at local x = 0.029 ≤ 0.04, through
  the mouth.
- Painter stations at S1, S6, S7 from the shipped camera: 0.5 / 0 / 0.5
  within 0.08; footprint 0.6 mm/px radial at S1 stated.
- Beauty: the outer ring on cap1 with its wedge break at the near mouth and
  the inner bands inside it; judged honestly, including what the 20°
  azimuth of the camera hides on the far side.

**Cost.** Live vs `def dust 0` at 64 spp under `pixelpel_rasterizer` (about
one painter evaluation per camera sample); the header and §8.5 name that
regime and do not present the figure as comparable to the path-traced
showcases.

## 2. `tidal_stones` — a burial line from `interior(r)` (Phase 3, signed half)

New scene `scenes/FeatureBased/Textures/tidal_stones.RISEscene`, 0.4 m
close-up. Layout (axis-aligned boxes unless stated; every dimension is
width × height × depth = x × y × z; centres given):
- `water`: `dielectric_material` box 0.36 × 0.60 × 0.36 at (0, −0.27, 0)
  → x, z ∈ [−0.18, 0.18], y ∈ [−0.57, 0.03]; the WATERLINE is y = 0.03.
  Deep so that on every stone's side the TOP face is the nearest face.
- `pool_bed`: opaque sand box 0.34 × 0.53 × 0.34 at (0, −0.285, 0) → y ∈
  [−0.55, −0.02], inside the water; the stones rest on its top (y = −0.02),
  visible through the water as the pool floor. Its top computes to
  −0.019999999999999962 in double while a stone bottom at 0.02 − 0.04 is
  −0.02 exactly, so a resting point is inside the bed by 5.6e-17 and
  `DeepestOtherContainment` DOES count it — at depth 5.6e-17, dominated by
  the water's depth under the running MAXIMUM. The header says this; the
  test asserts the water's value.
- `sand`: four opaque strips, top at y = 0.03, inner faces 1 mm outside the
  water (no coincident faces): `sand_px` 0.219 × 0.10 × 0.80 at (0.2905,
  −0.02, 0) → x ∈ [0.181, 0.40]; `sand_nx` mirrored at x = −0.2905; `sand_pz`
  0.362 × 0.10 × 0.219 at (0, −0.02, 0.2905) → x ∈ [−0.181, 0.181], z ∈
  [0.181, 0.40]; `sand_nz` mirrored. No two strips overlap, none enters the
  pool footprint.
- `stone_a`: `sphere_geometry` r = 0.04 at (0, 0.02, 0): bottom −0.02 on the
  bed, top 0.06 (3 cm in air).
- `stone_b`: `ellipsoid_geometry` radii (0.05, 0.03, 0.035) at (0.09, 0.01,
  −0.05), `orientation 0 20 0` (a yaw leaves the y semi-axis vertical; bottom
  at 0.01 − 0.03 = −0.02 on the bed; its footprint x ∈ [0.04, 0.14] stays
  inside the bed). Visual variety; its container is the water box (exact).
- `stone_c`: `sdf_geometry` pebble — `sphere` r 0.03 at part origin and
  `sphere` r 0.022 offset (0.02, 0, 0), joined by `smin` k 0.01 — at
  (−0.08, 0.01, 0.04) so its lowest point (0.01 − 0.03 = −0.02) rests on the
  bed. Container: the water box (exact).
- `stone_e`: a tilted FLAGSTONE — `box_geometry` 0.10 × 0.02 × 0.06 at
  (−0.02, 0.005, −0.10), `orientation 0 0 −30` (rotated about z by −30°, so
  its top face rises toward +x), placed so the top face's centre is at
  y = 0.015 and the face crosses the waterline along a line. This is the
  receiver whose wet ramp is READABLE: the top face's normal is (sin 30°,
  cos 30°, 0), 30° from vertical, so a camera above sees the ramp across a
  face at ≥ 60° elevation and the sightline crosses the water surface within
  10° of its normal — no refractive displacement worth a pixel. Stations on
  its top face: depth = 0.03 − y, with y = 0.015 + s·sin 30° for a point at
  signed distance s along the face's rise from its centre.
- `stone_d`: `sphere_geometry` r = 0.03 at (0.26, 0.035, 0.09), on `sand_px`
  (x ∈ [0.23, 0.29] inside the strip's [0.181, 0.40]): equator ring 5 mm
  above the sand, bottom 2.5 cm under. Its burial line is visible AT the
  sand surface; the buried ramp is not visible (opaque sand), so it carries
  no painter station.
Camera: `thinlens_camera` at (0.10, 0.36, 0.34) looking at (0, 0.0, −0.02),
50 mm, f/8, focus on stone_e's top face (the worker verifies the framing
arithmetic in the header: the 2 cm ramp on stone_e ≥ 8 px radial, elevation
≥ 45°). A key light rakes across the water from behind camera-left; a dim
sky fills. `pathtracing_pel_rasterizer`.

**Why `interior` and not `proximity`.** A point on a stone below the
waterline is 1–5 cm from the water's TOP face; `proximity(r)` would paint
everything under water uniformly (or nothing, past r). `interior(0.02)` =
clamp(depth/2 cm): 0 above the waterline, a ramp over the first 2 cm, 1
below — a crisp line AT the waterline and a sheen that saturates 2 cm down.

**Receivers' recipe.** Each stone: colour from an `expression_painter`
(`def buried interior(0.02)`; `expr mix(dry, wet, buried)`; a fine `fbm`
grain scaled by `(1 − buried)` so grain shows only on the dry part), and
roughness from a `scalar_painter` (`expression mix(0.55, 0.12, buried)`
with its OWN `def buried interior(0.02)` — a `def` does not cross chunks;
the second call is an L1 memo hit) bound to whichever shipped material
kind offers BOTH an `IPainter` reflectance slot and a scalar roughness slot
taking a `scalar_painter` and is NOT a conductor (`cooktorrance_material`'s
descriptor supports only `fresnel_mode conductor`, a metal; `coated_material`
is the repo's wet-look material and the `add_wetness` verb's target — the
worker reads its descriptor, names the slot, and if no such kind exists the
stones are Lambertian colour-only and the header says so as a deviation).

**Stations** (the container at every station is the water box or the sand
strip — exact families; tolerance 1e-9; every station point is ON the
receiver's surface, derived from the chunk's centre, radii and orientation):
- B1 stone_a top (0, 0.06, 0) → 0.
- B2 stone_a at the waterline: ring radius sqrt(0.04² − 0.01²) = 0.0387298;
  (0.0387298, 0.03, 0) → 0 (the water's top face computes to
  0.02999999999999997, so the point is 3e-17 outside; 1e-9 covers either
  rounding).
- B3 stone_a 1 cm under: (0.04, 0.02, 0) → depth 0.01 → 0.5.
- B4 stone_a 2 cm under: (0.0387298, 0.01, 0) → depth 0.02 → 1.0.
- B5 stone_a bottom (0, −0.02, 0): water top 0.05 away, floor 0.55 away,
  bed counted at 5.6e-17 → running maximum 0.05 → 1.0.
- B6 stone_d in sand_px (sand top 0.03, centre (0.26, 0.035, 0.09), r 0.03):
  equator point (0.29, 0.035, 0.09) → 5 mm above the sand → 0; the point at
  y = 0.02 at horizontal radius sqrt(0.03² − 0.015²) = 0.0259808 toward +x,
  (0.2859808, 0.02, 0.09) → depth 0.01 → 0.5 (the strip's top is nearest:
  its inner face is 0.105 away, its outer 0.114); the bottom (0.26, 0.005,
  0.09) → depth 0.025 → 1.0.
- B7 stone_b: the bottom point (0.09, −0.02, −0.05) → depth 0.05 → 1.0; a
  waterline point on the ellipsoid: solve the ellipsoid at y = 0.03 in its
  local frame (y_local = 0.02 = 2/3 of the 0.03 semi-axis, so the local xz
  ellipse has semi-axes 0.05·sqrt(5)/3 and 0.035·sqrt(5)/3), take the point
  at local +x, rotate by the 20° yaw, translate → 0.
- B8 stone_c: the test finds a surface point by bisecting the pebble's field
  ALONG −x at y = 0.02, z = 0.04 from x = −0.15 toward the centre
  (`SDFGeometry::EvaluateParts(geo->GetParts(), ptObject)` in object space,
  reached by `dynamic_cast<const SDFGeometry*>(obj->GetGeometry())` as
  `ProximitySignalTest` does; bisection tolerance 1e-10 stated) → the side
  point at y = 0.02 → depth 0.01 → 0.5 exact (container is the box).
- B9 stone_e (the flagstone): three points on its top face at rise s = −0.03,
  −0.01, +0.01 from the face centre (y = 0.015 + s/2 = 0.0, 0.01, 0.02 →
  depths 0.03, 0.02, 0.01 → 1.0, 1.0, 0.5) and one at s = +0.04 (y = 0.035,
  above the waterline → 0). The test builds the points from the chunk's
  centre, dimensions and orientation.
- Painter stations, probe camera = the shipped camera: B1 on stone_a (dry
  top, 0) and the four B9 points on the flagstone's top face read through the
  water at near-normal incidence (0 / 1.0 / 0.5 / 1.0 within 0.08; the
  sightline angle to the water's normal stated). B3/B4 (a sphere's flank
  under water) are QUERY-only: their normals are near-horizontal and any
  sightline to them refracts obliquely.
- Beauty: a crisp waterline on every wet stone, a sheen saturating 2 cm
  down on the flagstone and the sphere, dry grain above, the ellipsoid and
  pebble carrying the same line, stone_d's burial line in dry sand. Judged
  honestly.

**Cost.** Live vs `def buried 0` in BOTH chunks of every stone at the
shipped spp; the box containers are the cheapest family, so this gate bounds
`interior`'s own overhead (the running maximum over the TLAS's containing
leaves) in the path-traced regime.

## 3. `shelf_bunny` — mesh-to-mesh and mesh-to-plank contact (Phase 2)

New scene `scenes/FeatureBased/Textures/shelf_bunny.RISEscene`, a 0.5 m
close-up. Layout:
- `shelf`: `box_geometry` 0.60 × 0.025 × 0.24 at (0, −0.0125, 0) → top face
  y = 0, x ∈ [−0.30, 0.30], z ∈ [−0.12, 0.12]; a painted, slightly worn
  `expression_painter` (the plank's wear recipe at the author's discretion,
  plus `def dust proximity(0.02)` mixed into a warm grey).
- `wall`: `box_geometry` 0.60 × 0.40 × 0.02 at (0, 0.19, −0.13) → its front
  face at z = −0.12 is flush with the shelf's back edge; a matte plaster
  painter; it reads no signal.
- `bunny`: `risemesh_geometry models/risemesh/bunny.risemesh`, `position 0
  Y_b 0` with Y_b = −(lowest vertex y) so the lowest vertex touches y = 0
  (scene D: −0.0329874 for a plane at 0 — the same number here, re-derived
  by the test from the vertex array and asserted at 1e-6). The bunny's
  footprint must stay inside the shelf and > 2 cm from the wall: the test
  asserts the bunny's bounding box against the plank's extents.
- `dragon`: `risemesh_geometry models/risemesh/dragon_small.risemesh` at
  scene D's offset relative to the bunny, (−0.0318315, 0.135862595,
  −0.014760295) + (0, Y_b + 0.0329874, 0), `scale 0.35 0.35 0.35` (three
  components — a single number derives to a DEGENERATE (0.35, 0, 0)
  transform silently), its lowest vertex on the bunny's highest, re-derived
  and asserted at 1e-6.
Camera: `thinlens_camera` at (0.22, 0.20, 0.42) looking at (0, 0.06, 0),
50 mm, f/11, focused on the bunny's foot; the worker verifies the framing
arithmetic (the 2 cm ring on the shelf ≥ 8 px radial, the shelf top seen at
≥ 25°). A raking key from the left; `pathtracing_pel_rasterizer`.

**What it shows.** Mesh neighbours as SHEETS: the shelf reads
`proximity(0.02)` and draws a dust ring around the bunny's single contact
vertex (the bunny touches at ONE vertex; scene D's "four" were four probe
directions from it); the BUNNY reads `proximity(0.02)` from the dragon's
feet (mesh ↔ mesh, the case scene D pins at 1.0 with the bunny as self) and
darkens there; the dragon reads nothing.

**Stations** (r = 0.02; a `NearestOtherSurface` point query does not depend
on the receiver's family, so scene D's measured values transfer to the plank
at the same relative offsets).
- M1 shelf top (y = 0), 1 mm outside the contact vertex's xz in each of ±x,
  ±z → ≥ 0.9 (scene D: 0.9519 / 0.9740 / 0.9865 / 0.9913 — re-measured
  here, not copied).
- M2 shelf top, 2 mm outside, worst direction → ≥ 0.9 (scene D: 0.9037 —
  the margin is stated).
- M3 shelf top, 5 cm from the contact vertex in the direction the test
  DERIVES as clear: the test scans the bunny's vertex array and picks the
  horizontal direction (among ±x, ±z) in which no vertex lies within 2.5 cm
  of the shelf top at that station's xz (the belly overhang is a property of
  the mesh, not an assumption) → 0; the chosen direction and the belly
  clearance found are printed and recorded in the header.
- M4 the bunny's highest vertex with the bunny as `self` → 1.0; 5 mm below
  it (inside the bunny — a query-only station) → 0.75 closed form.
- Painter stations at M1 (the derived clear direction) and M3 (constant-1
  control, `oidn_denoise FALSE`): ≥ 0.85 and 0 within 0.08. No painter
  station on the bunny at M4: the pixel there shows the dragon's foot, not
  the bunny.
- Beauty: the dust ring around the foot, the darkening under the dragon's
  claws, nothing on the dragon. Judged honestly.

**Cost.** Live vs `def dust 0` on both receivers; both meshes answer on
their own BVHs; the mesh family's figure in the path-traced regime.

## 4. Deliverables per showcase (one worker each, parallel, separate worktrees)

1. The scene, header with the doctrine paragraph, the station table with
   predicted AND measured numbers, the framing arithmetic, the cost table,
   and every deviation from this spec named.
2. `tests/<Showcase>Test.cpp`: query stations derived from the scene's
   chunks, painter stations from the in-process probe/control renders (with
   the literal `oidn_denoise false` the hygiene test looks for), the
   assertions above; linked with `make -C build/make/rise build-test/<Name>`
   (never `make tests`).
3. Renders are not tracked; the header describes the crops the test writes
   under `rendered/`.
4. `CstDeriveGoldenTest` row; README entry; design doc §8.5 paragraph.
5. Gate suites: the new test, ProximitySignalTest, CstDeriveGoldenTest,
   SourceHygieneTest, plus `grep -l` for any class the worker touches.

Each worker's branch goes through the implementation-review-loop to zero
P1 before merge; the three merge in the order they finish, each rebased on
the previous, with the README and golden-file conflicts resolved by the
supervisor.
