# Cross-object signal showcases — three composed scenes for `proximity(r)` and `interior(r)`

Status: SPEC, 2026-09-10. Companion to `docs/CROSS_OBJECT_PROXIMITY_DESIGN.md`
(the engine; §8.2 has the station protocol these gates copy, §8.4 what Phase 3
built). Phase 1 shipped two showcases (`plank_closeup`, `weathered_workbench`);
Phases 2 and 3 shipped fully tested machinery with no beauty scene an author can
open. This document specifies the three scenes that fill that gap, each with
stations predicted from the geometry BEFORE the scene is composed, measured by a
harness test against the TRACKED scene, and recorded in the scene's header.

Every rule below inherits the doctrine in `skills/agent/procedural-textures.md`
(camera-first feature sizes, structure before noise, one field driving colour and
roughness, raking light) and the standing conventions: never over-read contact,
world-length radii, no `DynR` twin for either signal, `PT`-family rasterizers
(the signal is stamped in `ObjectManager::IntersectRay`; BDPT/VCM/MLT rebuilt
records read 0 — a showcase MUST use a path-tracing rasterizer or the legacy
`pixelpel_rasterizer`, and the test asserts the painter actually fires by
probing the rendered albedo, not only the query).

## 0. Shared gate protocol (from design §8.2, verbatim in spirit)

- **Query stations**: a harness test (`tests/<Showcase>Test.cpp`) loads the
  TRACKED scene through the normal CST path, takes the `IObjectManager`, and
  asks `NearestOtherSurface` / `DeepestOtherContainment` at world points it
  DERIVES from the scene's own chunks (positions, radii) — never hard-coded
  answers copied from a previous run. Each station states its prediction and
  the closed form it comes from; tolerance 1e-9 for exact families, the
  design's stated bound where a family is bounded (mesh sheets, SDF brackets).
- **Painter stations**: the same test renders a probe copy of the scene
  (relief modifiers stripped, every material a black Lambertian except the
  receiver, whose painter is the RAW signal, divided by a `clamp(N.y,0,1)`
  white control rendered identically, EXR in `Rec709RGB_Linear`, 16 spp)
  and reads the pixel under each station; this proves the signal reaches the
  painter under the scene's rasterizer. Pixel footprint is stated; where it
  is coarser than the station spacing the query station adjudicates and the
  pixel is a sanity check with a stated wide band.
- **Beauty**: the scene renders at its shipped settings; a crop around the
  contact is saved under `rendered/` and judged in the header honestly
  (what the signal drew, what it did not).
- **Costs**: the scene's wall clock with the signal live vs the same scene
  with the signal's `def` replaced by the constant 0 (the §8.2 "no-prox"
  control), 2 warm-ups then ≥3 interleaved pairs, every run listed, gate
  ≤ 1.15× (composites and meshes are dearer than the closed forms; the
  plank's 1.11–1.13× is the reference).
- **Ledgers**: `CstDeriveGoldenTest` gains one row per new scene; the
  `scenes/FeatureBased/README.md` entry follows the plank entry's shape; the
  scene header records every measured number and every deviation from this
  spec; §8.5 of the design doc gets one paragraph per showcase pointing here.

## 1. `glass_pavilion` — contact dust at the feet of fluted columns (Phase 3, CSG receiver-neighbour)

**What it shows.** `proximity()` answered by a CSG composite: the four
`subtraction` columns (a capped cylinder r = 0.25 minus a 0.08 × 5.2 × 0.5
slot) stand on the caps `cap1..cap4` (`capgeom` 0.7 × 0.15 × 0.7 at y = 0.1,
top face y = 0.175). The cap tops collect a dust ring where the column wall
meets them — and the ring is INTERRUPTED at each flute mouth, because the
composite's recessed wall is farther away there. That interruption is the
tell that the query walked the composite, not the cylinder.

**Edit (tracked scene).** One new material `marble_cap` bound to
`cap1..cap4` ONLY (`marble_col` keeps its other 14 objects, so the probe-albedo
copy paints exactly four receivers): a `lambertian_material` whose
reflectance is an `expression_painter` — `def dust proximity(0.02)`;
`expr mix(marble_white, dust_brown, clamp(dust*0.85 + breakup, 0, 1))` with a
small `fbm` breakup so the ring is not a hard band; marble white and dust
brown are the scene's existing `pnt_marble_white` colour and a warm grey-brown
`vec3(0.42, 0.36, 0.30)`. No relief, no roughness (Lambertian caps). The
`capped TRUE` pin stays. Nothing else in the scene moves; the beauty render
keeps `pixelpel_rasterizer` if the probe render proves the signal fires under
it, else the scene switches to `pathtracing_pel_rasterizer` at matched spp and
the header says so.

**Stations (cap1's top face, y = 0.175; column1 axis at (−2.5, ·, 2.5),
un-rotated, slot faces at local z = ±0.25, slot mouth wedge half-angle
asin(0.04/0.25) = 9.21° about ±z).** Query radius 0.02.
- S1 wall +x, 1 cm out: world (−2.5 + 0.26, 0.175, 2.5) → predict 0.5
  (`d(s) = s`, upright wall). Exact-operand landing: closed form to 1e-9.
- S2 wall +x, 2 cm out: (−2.5 + 0.27, …) → 0 (the cut-off is exclusive).
- S3 wall +x, 4 cm out: (−2.5 + 0.29, …) → 0.
- S4 flute mouth +z, 1 cm out: (−2.5, 0.175, 2.5 + 0.26) → 0; the
  per-object query REFUSES (design §8 flute station; true corner distance
  4.21 cm). Asserted as a refusal on `column1` and 0 on the scene query.
- S5 same on cap2 for the ROTATED column2 (`orientation 0 45 0`): the wall
  station is placed along the rotated local +x, world direction
  (cos 45°, 0, −sin 45°) from (2.5, ·, 2.5): 1 cm out → 0.5; the flute
  station along the rotated local +z → 0. This is the transformed-composite
  gate on the tracked scene.
- S6 a point on the cap top 9 cm from the axis, i.e. 6.5 cm from the wall
  (the cap's half-width 0.35 leaves 0.10 of exposed top beyond the 0.25
  wall) → 0 (nothing within 2 cm).
- Painter station: the probe render's pixel at S1 reads 0.5 within 0.08
  (the cap top is seen at a grazing angle from the camera at (−3.5, 2, 6),
  85 mm; the footprint is stated; if grazing makes the pixel unreadable the
  probe copy is rendered from a top-down camera and the header says so).
- Beauty: the ring is visible on cap1 and cap2 (the front pair) and breaks
  at the flute mouths; the back caps may be out of frame — say which.

**Cost.** Two runs of the tracked scene vs the no-prox control at the
shipped 64 spp; composite queries are the dearest family, so the gate here
is the one that measures the CSG bracket in a real scene.

## 2. `tidal_stones` — a burial line from `interior(r)` (Phase 3, signed half)

New scene `scenes/FeatureBased/Textures/tidal_stones.RISEscene`, 0.4 m
close-up. A shallow clear pool (a `dielectric_material` box, DEEP per the
skill's rule: 0.60 tall so the top face is the nearest face over every
receiver, 0.36 wide) sits in a sand bed (an opaque box). Three stones rest on
the pool floor and break the surface; a fourth lies on the sand outside the
water, half-buried in it. A key light rakes across the water from behind the
camera-left; a dim sky fills.

**Why `interior` and not `proximity`.** The water TRANSMITS: a point on a
stone below the waterline is 1–4 cm from the water's top face, so a
`proximity` grime term would paint everything under water uniformly; what
the eye expects is a depth ramp (darkening and a wet sheen that saturate a
couple of centimetres down) and a crisp line AT the waterline. That is
exactly the signed half's contract: `interior(0.02)` = clamp(depth/2 cm),
0 above the waterline, 1 from 2 cm down.

**Receivers.**
- `stone_a`: `sphere_geometry` r = 0.04 at (0, 0.02, 0); water top at
  y = 0.03 → the stone's top is 3 cm in air, its bottom 5 cm under.
- `stone_b`: `ellipsoid_geometry` (0.05, 0.03, 0.035) at (0.09, 0.015, −0.04),
  rotated 20° about y — the bounded family; its stations use the design's
  `dUnit × min` lower bound, tight along the minor axis.
- `stone_c`: an `sdf_geometry` pebble (smooth-union of two spheres) at
  (−0.08, 0.01, 0.03) — the bracketed family.
- `stone_d`: `sphere_geometry` r = 0.03 at (0.14, 0.035, 0.09), sitting in
  the SAND box (top y = 0.03 there, sand is opaque) — the same signal on an
  opaque container, so the burial line reads on dry ground too.
Each stone's material: the skill fence's recipe — `expression_painter` with
`def buried interior(0.02)`, colour `mix(dry, wet, buried)` and, on a
`cooktorrance_material`, roughness through a `scalar_painter`
`expression` `mix(0.55, 0.12, buried)` so the sheen follows the depth, and
a fine `fbm` breakup scaled by (1 − buried) so grain shows only on the dry
part. One field drives colour and roughness.

**Stations** (all containers are boxes: exact signed distance; the receiver's
own surface point is what a shading sample sees, so the stations are ON the
stones' surfaces).
- B1 stone_a top (0, 0.06, 0): outside the water → 0.
- B2 stone_a at the waterline (0.04·cos θ, 0.03, 0.04·sin θ) for θ = 0:
  world (0.04, 0.03, 0) → depth 0 → 0 (exclusive: on the face).
- B3 stone_a 1 cm under: point on the sphere at y = 0.02 → (0.04, 0.02, 0)
  → depth 0.01 → 0.5 to 1e-9.
- B4 stone_a 2 cm under: y = 0.01 → (sqrt(0.0016 − 0.0001), 0.01, 0) →
  depth 0.02 → 1.0.
- B5 stone_a bottom (0, −0.02, 0): `interior` measures the nearest face of
  the CONTAINER, and the stone rests on the pool floor, so the floor is the
  nearest face here (the pool's 0.60 depth is what keeps the TOP face nearest
  everywhere on the stone's sides; the floor clearance at the bottom is the
  stone's own resting gap). With the stones resting on a 1 cm bed of pebbles
  (`stone_bed`, a thin opaque box at the pool floor, y ∈ [−0.04, −0.03]) the
  water box's floor is 1 cm below the stone's bottom, so B5 reads depth 0.01
  → 0.5, not 1.0. The header states this: the underside of a resting stone
  reads the floor gap, the sides read the waterline, and B2–B4 are the
  stations that pin the visible ramp. The test asserts B5 = 0.5 to 1e-9.
- B6 stone_d (in sand, sand top y = 0.03, sphere centre y = 0.035, r 0.03):
  the sphere's equator ring at y = 0.035 is 0.5 cm above the sand → 0; the
  point at y = 0.02 on the sphere → depth 0.01 → 0.5; the bottom
  (0.14, 0.005, 0.09) → depth 0.025 → 1.0 (the sand box is 0.10 deep, so
  the top face is nearest).
- B7 stone_b (ellipsoid) at its minor-axis bottom point rotated back to
  world: the bound is tight on the minor axis, so the prediction is the
  exact depth; state it and assert within 1e-9; at an off-axis point assert
  only `≤ true depth` (the bound direction).
- B8 stone_c (SDF): assert the bracket's answer against a grid search of the
  pebble's own field, per design §8's SDF rule, with `gap_max` recorded.
- Painter stations: probe-albedo pixels at B1, B3, B4 on stone_a from the
  shipped camera: 0 / 0.5 / 1.0 within 0.08; the footprint at 0.4 m is
  stated.
- Beauty: a crisp waterline on every wet stone, a sheen that saturates
  2 cm down, dry grain above; on stone_d a burial line in the sand at the
  same rule. Judged honestly.

**Cost.** Live vs no-interior control at the shipped spp; gate ≤ 1.15×.

## 3. `shelf_bunny` — mesh-to-mesh and mesh-to-plane contact (Phase 2)

New scene `scenes/FeatureBased/Textures/shelf_bunny.RISEscene`, a 0.5 m
close-up of a painted wooden shelf with the Stanford bunny on it and the
small dragon perched on the bunny's back, exactly scene D's placement
(bunny `position 0 −0.0329874 0` so its lowest vertex touches the shelf;
dragon at (−0.0318315, 0.135862595, −0.014760295), `scale 0.35 0.35 0.35`,
its lowest vertex on the bunny's highest — the numbers scene D's test
derives and asserts, reused verbatim and re-asserted here). A wall behind,
a raking key from the left.

**What it shows.** Mesh neighbours as SHEETS: the shelf (a `box_geometry`
plank with a painted, slightly worn `expression_painter`) reads
`proximity(0.02)` and draws a dust ring around the bunny's four contact
patches; the BUNNY reads `proximity(0.02)` from the dragon's feet (mesh ↔
mesh, the case scene D pins at 1.0 with the bunny as self) and darkens
there; the dragon reads nothing (it is the top of the stack).

**Stations** (r = 0.02; the test derives the footprint from
`GenerateBoundingBox` and the mesh's lowest vertices exactly as
`MeshClosestPointTest` (g) does).
- M1 shelf top, 1 mm outside the bunny's lowest-vertex footprint → ≥ 0.9
  (scene D measured 0.9519 at this station; the mesh answer is exact, the
  ≥ 0.9 band is the geometry's).
- M2 shelf top, 2 mm outside (worst of four directions) → ≥ 0.9 (scene D:
  0.9037, narrowly; state the margin).
- M3 shelf top, 5 cm from the footprint → 0.
- M4 the bunny's highest vertex with the bunny as `self` → 1.0 (the dragon's
  lowest vertex is there); 5 mm below it → 0.75 closed form.
- M5 a shelf point under the dragon's overhang, 3 cm below the dragon's
  belly → 0 (the dragon is > 2 cm away and the bunny is not nearer).
- Painter stations: probe-albedo pixels at M1 and M3: ≥ 0.85 and 0 within
  0.08; on the bunny at M4 the pixel is a grazing sliver — the query station
  adjudicates, the pixel gets a wide band and the header says so.
- Beauty: the dust ring around the feet, the darkening under the dragon's
  claws, nothing on the dragon. Judged honestly.

**Cost.** Live vs no-prox control; both meshes are answered on their own
BVHs, so this is the mesh-family cost gate in a composed scene; ≤ 1.15×.

## 4. Deliverables per showcase (one worker each, parallel, separate worktrees)

1. The scene (edit or new), header with the doctrine paragraph, the station
   table with predicted AND measured numbers, the cost table, and every
   deviation from this spec named.
2. `tests/<Showcase>Test.cpp`: query stations derived from the scene's
   chunks, painter stations from a probe render, the assertions above; linked
   with `make -C build/make/rise build-test/<Name>` (never `make tests`).
3. `rendered/` crops are NOT committed (the repo does not track renders);
   the header describes them and the test writes them to the scratch cwd.
4. `CstDeriveGoldenTest` row; README entry; design doc §8.5 paragraph.
5. Gate suites: the new test, ProximitySignalTest, CstDeriveGoldenTest,
   SourceHygieneTest, AgentSkillsTest (if the skill is touched), plus
   `grep -l` for any class the worker touches.

Each worker's branch goes through the implementation-review-loop to zero
P1 before merge; the three merge in the order they finish, each rebased on
the previous, with the README and golden-file conflicts resolved by the
supervisor.
