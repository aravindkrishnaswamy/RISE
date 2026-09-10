# Cross-object signal showcases — three composed scenes for `proximity(r)` and `interior(r)`

Status: SPEC, 2026-09-10, revised the same day after one adversarial round
(8 P1s: the pavilion's shipped camera frames none of its caps; a station
inside the column; the pool's floor logic inverted; bound claims on
receivers whose containers are exact boxes; a "dry" stone inside the pool; a
`clamp(N.y)` control that is 0 on a sphere's flank; a shelf station with no
closed form; "1e-9" asserted on a composite). Companion to
`docs/CROSS_OBJECT_PROXIMITY_DESIGN.md` (the engine; §8.2 has the station
protocol these gates copy, §8.4 what Phase 3 built, and `ProximitySignalTest`
(l) already pins the pavilion's cap-top stations against the tracked scene).
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
`pixelpel_rasterizer` with `DefaultDirectLighting` (verified against the
code in review; `WarnIfNonPTRenderHasLiveSignalConsumer` is called only by
the BDPT/VCM/MLT rasterizers). The painter stations below prove it anyway.

## 0. Shared gate protocol

- **Query stations**: a harness test (`tests/<Showcase>Test.cpp`) loads the
  TRACKED scene through the normal CST path (`Cst::ParseToCst` +
  `DeriveToJob`, as `MeshClosestPointTest` (g) does), takes the
  `IObjectManager`, and asks `NearestOtherSurface` / `DeepestOtherContainment`
  at world points it DERIVES from the scene's own chunks — never hard-coded
  answers copied from a previous run. Where a station is defined in an
  object's LOCAL frame (a rotated composite) the test pushes the local point
  through that object's `GetFinalTransformMatrix()`, as `ProximitySignalTest`
  (l) does, so the test cannot disagree with the engine about a convention.
- **Tolerances by the CONTAINER/NEIGHBOUR family, not the receiver's**: a
  box or sphere neighbour is exact → 1e-9. A CSG composite is never exact
  (design §5.6 as corrected in §8.4: no composite exports exactness) → the
  band `ProximitySignalTest` (l) already uses: `≤ 0.5 + 1e-9` and
  `≥ 0.4874 − 1e-4` at the 1 cm station (a `+1 ulp` landing miss sends the
  bracket to its probe, ε = 5e-5 × 5.0498 = 2.52e-4, `0.5 − ε/0.02` = 0.4874).
  A mesh neighbour is exact per triangle but the station's TRUE distance has
  no closed form → the geometry's band, stated (scene D's 0.9519 etc.).
- **Painter stations**: the same test renders a probe copy of the scene
  IN-PROCESS (`Job::Rasterize()` with a capturing `IRasterizerOutput`, the
  pattern `EnvLightBalanceTest` uses — no EXR round-trip, no sRGB-default
  trap): relief modifiers stripped, every material a black Lambertian except
  the receiver, whose painter is the RAW signal; divided pixel-by-pixel by a
  CONTROL render of the same copy with the signal's `def` replaced by the
  constant 1 (NOT `clamp(N.y,0,1)`, which is 0 on any flank facing sideways
  or down — a sphere's equator has `N.y = 0`). 16 spp. The pixel under each
  station is read; the pixel footprint (mm) is stated; where the footprint
  is coarser than the station spacing the query station adjudicates and the
  pixel is a sanity check with a stated wide band.
- **Framing rule** (the reason showcase 1 is a new scene): a receiver's
  2 cm ring must image at ≥ 8 px across and its face must be seen at ≥ 25°
  elevation, and the receiver must lie inside the depth of field; the header
  shows the arithmetic (distance, focal length, sensor, f-stop, focus).
- **Beauty**: the scene renders at its shipped settings; a crop around the
  contact is written under `rendered/` (gitignored) and judged in the header
  honestly (what the signal drew, what it did not).
- **Costs**: the scene's wall clock with the signal live vs the same scene
  with the signal's `def` replaced by the constant 0, `render_thread_reserve_count 0`,
  2 warm-ups then ≥3 interleaved pairs, every run listed. Target ≤ 1.15×
  (the plank's 1.11–1.13× is the reference; there is no per-family CSG or
  mesh cost figure in the design yet — these gates PRODUCE those figures,
  on receivers that are in frame, so the bracket and the mesh BVH are
  measured on camera-visible hits).
- **Ledgers**: `CstDeriveGoldenTest` gains one row per NEW scene (no tracked
  scene is edited by this spec, so no existing row drifts); the
  `scenes/FeatureBased/README.md` entry follows the plank entry's shape; the
  scene header records every measured number and every deviation from this
  spec; §8.5 of the design doc gets one paragraph per showcase pointing here.

## 1. `pavilion_colonnade` — contact dust at the feet of fluted columns (Phase 3, CSG neighbour)

**Why a new scene.** `glass_pavilion`'s shipped camera (85 mm at
(−3.5, 2, 6), f/1.4 focused at 7 m) frames NONE of the four caps: cap1/2/3
are outside even the half-diagonal, and cap4 at 10.6 m is defocused to
5.6 px while its 2 cm ring images at 3.6 px, foreshortened to under a pixel
at 10° grazing. The pavilion keeps its subject and its `capped TRUE` pin;
the signal gets a composition of its own.

**New scene** `scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene`,
built from the pavilion's geometry and materials (the same `colcylgeom`,
`flutegeom`, `capgeom`, four `subtraction` columns at (±2.5, 2.5, ±2.5)
with the same orientations 0/45/30/60, the floor, ceiling, back wall, the
pedestal + glass sphere and the bronze vase as background), with a LOW
camera at the front row: `thinlens_camera` at about (−2.55, 0.62, 3.9)
looking at cap1's foot (−2.35, 0.175, 2.5), 50 mm, f/5.6, focused on the
foot, so cap1's top is seen at ≈ 25–30° elevation ~1.5 m away (the ring
spans ≥ 12 px at 800×600; the header shows the arithmetic) and cap2 and
column2 stand in the mid-ground with the rotated slot mouth facing the
camera. `pixelpel_rasterizer` as the pavilion, 64 spp.

**Receiver.** One new material `marble_cap` bound to `cap1..cap4` ONLY
(`marble_col` keeps the other 14 objects): a `lambertian_material` whose
reflectance is an `expression_painter` — `def dust proximity(0.02)`;
`expr mix(vec3(marble white), vec3(0.42, 0.36, 0.30), clamp(dust*0.85 +
0.08*fbm(P*180 + jitter, 3, 0.5, 2.0), 0, 1))` with the pavilion's marble
white. No relief, no roughness. The `def` is per chunk (a `def` never
crosses chunks).

**What the picture shows.** The band follows the COMPOSITE's cross-section
(the design's §8.4 crop): an outer ring hugging the round wall, BROKEN in a
wedge ~8 cm wide at each slot mouth (the nearest real surface there is the
slot-wall/cylinder corner, 4.21 cm from the mouth's axis), and two thin
bands inside the mouth hugging the slot walls (at |x| = 0.03 the wall is
1 cm away → 0.5). A cylinder neighbour would draw an unbroken ring; the
break-plus-inner-bands is the tell that the query walked the composite.

**Stations** (cap1's top face y = 0.1 + 0.075 = 0.175; column1 axis at
(−2.5, ·, 2.5), un-rotated; slot faces at local z = ±0.25, slot half-width
0.04, mouth wedge half-angle asin(0.04/0.25) = 9.21°). Query radius 0.02
unless stated. `ProximitySignalTest` (l) already pins the 0.5/0/0 wall
stations, the flute refusal and the 4.21 cm corner on the tracked
`glass_pavilion`; this test re-asserts them on the NEW scene (same chunks,
so the same predictions) and ADDS S6, S7, the painter stations and the cost.
- S1 wall local +x, 1 cm out: local (0.26, y, 0) → world (−2.24, 0.175, 2.5)
  → 0.5 within the composite band.
- S2 2 cm out (0.27) → 0; S3 4 cm out (0.29) → 0.
- S4 flute mouth local +z, 1 cm out: (−2.5, 0.175, 2.76) → 0 on the scene
  query at r = 0.02; the per-object `column1->DistanceToSurface` at r = 0.1
  REFUSES (the scene-wide query at 0.1 answers 0.075 from the floor top at
  y = 0.1 — the cross-check (l) uses).
- S5 the rotated column2: local (0.26, y, 0) and local (0, y, 0.26) pushed
  through `column2->GetFinalTransformMatrix()` → 0.5 (band) and 0.
- S6 cap1 top, 6.5 cm from the wall = 0.315 from the axis along local +x:
  (−2.185, 0.175, 2.5) → 0 (nothing within 2 cm; the cap's half-width 0.35
  keeps it on the cap).
- S7 inside the slot mouth on the exposed cap top: local (0.03, y, 0.20)
  (inside the slot, 1 cm from the +x slot wall, well inside the cylinder
  radius) → 0.5 within the composite band — the inner band.
- Painter stations at S1, S6, S7 from the shipped camera: 0.5 / 0 / 0.5
  within 0.08; footprint stated.
- Beauty: the outer ring on cap1 with its wedge breaks, the inner bands in
  the near mouth, the same on cap2 seen obliquely; judged honestly.

**Cost.** Live vs `def dust 0` at 64 spp; this is the first composite-bracket
cost measured on camera-visible hits — the header states the number and the
design §8.5 records it as the CSG family's figure.

## 2. `tidal_stones` — a burial line from `interior(r)` (Phase 3, signed half)

New scene `scenes/FeatureBased/Textures/tidal_stones.RISEscene`, 0.4 m
close-up. Layout (all boxes axis-aligned unless stated):
- `water`: `dielectric_material` box 0.36 × 0.60 × 0.36 centred (0, −0.27, 0)
  → spans y ∈ [−0.57, 0.03]; the WATERLINE is y = 0.03. Deep so that, on
  every stone's side, the TOP face is the nearest face (skill rule).
- `pool_bed`: opaque sand box 0.34 × 0.53 × 0.34 centred (0, −0.285, 0)
  → spans y ∈ [−0.55, −0.02], INSIDE the water. The stones rest ON its top
  (y = −0.02). It is visible through the water as the pool floor. A stone
  point resting on it has `f_bed = 0`, which `interior` does not count
  (strictly negative only), so the bed never contributes depth.
- `sand`: four opaque strips around the pool, each 0.22 × 0.10 × 0.80 (or
  0.80 × 0.10 × 0.22), top at y = 0.03 flush with the water, leaving the
  pool footprint |x|,|z| ≤ 0.18 uncovered. Overlapping solids are avoided by
  construction (no strip enters the pool footprint).
- `stone_a`: `sphere_geometry` r = 0.04 at (0, 0.02, 0): rests on the bed
  (bottom y = −0.02), top at 0.06 (3 cm in air).
- `stone_b`: `ellipsoid_geometry` (0.05, 0.03, 0.035) at (0.09, 0.01, −0.05),
  rotated 20° about y, resting on the bed at its minor-axis bottom
  (0.01 − 0.03 = −0.02). Visual variety; its stations are exact because its
  CONTAINER is the water box.
- `stone_c`: `sdf_geometry` pebble (smooth union of two spheres, r 0.03 and
  0.022, offset 0.02 along x, `smin` 0.01) at (−0.08, 0.008, 0.04), resting
  on the bed. Same: exact container.
- `stone_d`: `sphere_geometry` r = 0.03 at (0.26, 0.035, 0.09) — OUTSIDE the
  pool footprint (x ∈ [0.23, 0.29] > 0.18), sitting in the +x sand strip
  (sand top y = 0.03; the sphere's equator ring at y = 0.035 is 5 mm above
  the sand; its bottom at 0.005 is 2.5 cm under).
A key light rakes across the water from behind camera-left; a dim sky fills.
`pathtracing_pel_rasterizer` (the water is a dielectric).

**Why `interior` and not `proximity`.** A point on a stone below the
waterline is 1–5 cm from the water's TOP face; `proximity(r)` would paint
everything under water uniformly (or nothing, past r). `interior(0.02)` =
clamp(depth/2 cm): 0 above the waterline, a ramp over the first 2 cm, 1
below — a crisp line AT the waterline and a sheen that saturates 2 cm down.

**Receivers' recipe.** Each stone is a `cooktorrance_material` (the
material whose scalar slot is `facets` — there is no `roughness`
parameter) with `reflectance` an `expression_painter` (`def buried
interior(0.02)`; `expr mix(dry, wet, buried)`; a fine `fbm` grain scaled by
`(1 − buried)` so grain shows only on the dry part) and `facets` a
`scalar_painter` whose `expression` is `mix(0.55, 0.12, buried)` with its
OWN `def buried interior(0.02)` (a `def` does not cross chunks; the second
call is an L1 memo hit). One field drives colour and roughness in spirit;
two chunks declare it in syntax.

**Stations** (the container at every station is the water box or the sand
strip — exact families; tolerance 1e-9; every station point is ON the
receiver's surface, derived from the chunk's centre and radii):
- B1 stone_a top (0, 0.06, 0) → 0 (above the waterline).
- B2 stone_a at the waterline: the sphere's ring at y = 0.03 has radius
  sqrt(0.04² − 0.01²) = 0.0387298; point (0.0387298, 0.03, 0) → depth 0 →
  0 (`interior` counts strictly negative `f` only).
- B3 stone_a 1 cm under: y = 0.02 (the equator), point (0.04, 0.02, 0) →
  depth 0.01 → 0.5.
- B4 stone_a 2 cm under: y = 0.01, ring radius sqrt(0.0016 − 0.0001) =
  0.0387298; point (0.0387298, 0.01, 0) → depth 0.02 → 1.0.
- B5 stone_a bottom (0, −0.02, 0): the water's top face is 0.05 away, its
  floor 0.55 away; the bed is touched (`f = 0`, uncounted) → depth 0.05 →
  1.0 (saturated).
- B6 stone_d in sand (sand top 0.03, centre 0.035, r 0.03): equator point
  (0.29, 0.035, 0.09) → 5 mm above the sand → 0; the point at y = 0.02,
  horizontal radius sqrt(0.03² − 0.015²) = 0.0259808 → (0.1659808, 0.02,
  0.09) → depth 0.01 → 0.5; the bottom (0.26, 0.005, 0.09) → depth 0.025 →
  1.0 (the strip is 0.10 deep, its top is nearest).
- B7 stone_b: its minor-axis bottom point rotated back to world (the test
  builds it from the chunk's radii and orientation) → depth 0.03 − (−0.02)
  = 0.05 → 1.0; a point on its waterline ring → 0. Exact (container is the
  box).
- B8 stone_c: the test finds a surface point by bisecting the pebble's
  field along −y from above at x = −0.08, z = 0.04 (the Map is available
  through the geometry), then asks `interior` there → depth = 0.03 − y_hit
  → clamp(depth/0.02) exact to 1e-9 (container is the box); the bisection
  tolerance is stated.
- Painter stations at B1, B3, B4 on stone_a (constant-1 control): 0 / 0.5 /
  1.0 within 0.08; footprint at 0.4 m stated (a 1 cm step must span ≥ 8 px).
- Beauty: a crisp waterline on every wet stone, a sheen saturating 2 cm
  down, dry grain above, and on stone_d the same rule as a burial line in
  dry sand. Judged honestly.

**Cost.** Live vs `def buried 0` (both chunks) at the shipped spp; the box
containers are the cheapest family, so this gate bounds `interior`'s own
overhead (the running maximum over the TLAS's containing leaves).

## 3. `shelf_bunny` — mesh-to-mesh and mesh-to-plank contact (Phase 2)

New scene `scenes/FeatureBased/Textures/shelf_bunny.RISEscene`, a 0.5 m
close-up of a painted wooden shelf (a `box_geometry` plank, its top derived
from its chunk) against a wall, the Stanford bunny on it and the small
dragon perched on the bunny's back, exactly scene D's placement: the bunny
positioned so its LOWEST VERTEX touches the shelf top (scene D: y =
−0.0329874 above a plane at 0 — here offset by the plank's top), the dragon
at scene D's offset (−0.0318315, 0.135862595, −0.014760295) relative to the
bunny, `scale 0.35 0.35 0.35` (three components — one number derives to a
DEGENERATE transform silently), its lowest vertex on the bunny's highest.
The test re-derives both placements from the mesh vertex arrays exactly as
`MeshClosestPointTest` (g) does and asserts them at 1e-6. A raking key from
the left; `pathtracing_pel_rasterizer`.

**What it shows.** Mesh neighbours as SHEETS: the shelf's painted
`expression_painter` reads `proximity(0.02)` and draws a dust ring around
the bunny's single contact vertex (the bunny touches at ONE vertex; scene
D's "four" were four probe directions from it); the BUNNY reads
`proximity(0.02)` from the dragon's feet (mesh ↔ mesh, the case scene D
pins at 1.0 with the bunny as self) and darkens there; the dragon reads
nothing.

**Stations** (r = 0.02; a `NearestOtherSurface` point query does not depend
on the receiver's family, so scene D's measured values transfer to the
plank at the same relative offsets).
- M1 shelf top, 1 mm outside the contact vertex's footprint in each of ±x,
  ±z → ≥ 0.9 (scene D: 0.9519 / 0.9740 / 0.9865 / 0.9913 — re-measured
  here, not copied).
- M2 shelf top, 2 mm outside, worst direction → ≥ 0.9 (scene D: 0.9037 —
  the margin is stated).
- M3 shelf top, 5 cm from the contact TOWARD THE CAMERA (−z, away from the
  wall, which is > 2 cm from every station) → 0.
- M4 the bunny's highest vertex with the bunny as `self` → 1.0; 5 mm below
  it → 0.75 closed form.
- Painter stations at M1 (the −z direction) and M3 (constant-1 control):
  ≥ 0.85 and 0 within 0.08; the bunny at M4 is a grazing sliver from the
  camera — the query adjudicates, the pixel gets a band of 0.3 and the
  header says so.
- Beauty: the dust ring around the foot, the darkening under the dragon's
  claws, nothing on the dragon. Judged honestly.

**Cost.** Live vs `def dust 0` on both receivers; both meshes answer on their
own BVHs, so this is the mesh family's cost figure on camera-visible hits.

## 4. Deliverables per showcase (one worker each, parallel, separate worktrees)

1. The scene, header with the doctrine paragraph, the station table with
   predicted AND measured numbers, the framing arithmetic, the cost table,
   and every deviation from this spec named.
2. `tests/<Showcase>Test.cpp`: query stations derived from the scene's
   chunks, painter stations from the in-process probe render with the
   constant-1 control, the assertions above; linked with
   `make -C build/make/rise build-test/<Name>` (never `make tests`).
3. Renders are not tracked; the header describes the crops the test writes
   under `rendered/`.
4. `CstDeriveGoldenTest` row; README entry; design doc §8.5 paragraph.
5. Gate suites: the new test, ProximitySignalTest, CstDeriveGoldenTest,
   SourceHygieneTest, plus `grep -l` for any class the worker touches.

Each worker's branch goes through the implementation-review-loop to zero
P1 before merge; the three merge in the order they finish, each rebased on
the previous, with the README and golden-file conflicts resolved by the
supervisor.
