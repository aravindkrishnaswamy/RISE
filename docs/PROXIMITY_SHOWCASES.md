# Cross-object signal showcases — three composed scenes for `proximity(r)` and `interior(r)`

Status: SPEC, 2026-09-10, revised five times the same day after adversarial
rounds (round 5, 4 P1s: an appended probe material derives AFTER the
object that names it, which then silently fails to add; M4's 5 mm station
IS a closed form, 0.75, because the contact plane bounds the dragon from
below; the M3 direction rule was a vertical-column test while two
candidates read 0.41 and 0.77; a raking key is TIR-blocked from every
submerged station regardless of `transparent_shadows`. Round 4, 5 P1s: the probe ratio is not the signal unless the
receiver is rebound to a Lambertian for the probe; `transparent_shadows`
defaults FALSE, so every submerged NEE ray was blocked; `pinhole_camera`
has no focal length and its `fov` is vertical; the hygiene test's literal
is a contiguous substring the setter call does not contain; a "closed
form" on a mesh neighbour. Round 3, 8 P1s: an unscaled teapot as the mid-ground prop would have
engulfed the camera; the flagstone's centre, rise direction and low corner;
its painter stations crossed the water at 51°; the probe-copy setter named
no-ops on an absent parameter; the probe must swap the `expr`, not the `def`;
the shelf camera sat at 22°. Round 1, 8 P1s: the pavilion's shipped camera frames none of its caps; a
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
  station's TRUE distance has no closed form → the geometry's band,
  stated — unless a geometric bound pins it, as the contact plane does at
  §3's M4.
- **Painter stations**: the same test renders a PROBE copy of the scene and
  a CONTROL copy in-process and reads the pixel ratio under each station.
  Mechanism (all existing API): `Cst::ParseToCst` the tracked file →
  `Cst::DocSetOrAddParamValue(doc, chunkId, …)` (NOT `DocSetParamValue`,
  which silently no-ops when the parameter is absent from the chunk) to
  replace the receiver painter's `expr` (probe: `vec3(dust)` — the RAW
  signal, exactly as design §8.2's albedo protocol; control: `vec3(1)`;
  swapping only the `def` would leave the beauty `mix`/`clamp`/`fbm` in
  both copies and the ratio would not be the signal) and to set
  `oidn_denoise FALSE` on the rasterizer chunk (MANDATORY: the default is
  TRUE and a denoised probe smears the band) → `Cst::DeriveToJob` (the
  in-memory half is `MeshClosestPointTest` (g)'s `LoadSceneD` on a
  `new Job()`) → `Job::RemoveRasterizerOutputs()` +
  `GetRasterizer()->AddRasterizerOutput(capturing output)` +
  `Job::Rasterize()` (that triple is `EnvLightBalanceTest`'s; values are
  linear doubles, no sRGB). The ratio is the signal ONLY when the receiver
  is a pure albedo: §8.2's protocol is an ALBEDO probe. So a receiver
  whose shipped material is anything but a `lambertian_material` with the
  expression painter as its `reflectance` (showcase 2's stones are
  `coated_material` over GGX, whose `rs` F0 and coat add the same term to
  both copies and pull the ratio toward 1) is REBOUND in both copies: the
  test inserts a `lambertian_material` chunk whose `reflectance` names
  the receiver's expression painter and points the receiver object's
  `material` at it with `DocSetOrAddParamValue`. The insertion INDEX
  matters: `DeriveToJob` applies chunks in document order and
  `Job::AddObject` resolves the material name at apply time (an unknown
  name fails that one chunk, the derive continues, and the receiver is
  silently MISSING from the probe), while every in-tree `DocInsertItem`
  call site appends at the end — so the test builds the item the Job.cpp
  way (`ParseToCst(chunkText)`, first `NodeKind::Chunk` item via
  `DocResolveNodeId(DocNodeIdAt(d, i))`) and inserts it AT the receiver
  object's own index (`DocFindByName` → `DocIndexOfNodeId`), which is
  after the expression painter the object already depends on. Both
  derives assert `diagnostics.empty()`, as `MeshClosestPointTest`'s
  `LoadSceneD` does. The black-out is scoped per showcase: showcase 1
  renders under the direct-only `pixelpel_rasterizer`, where the ratio is
  the albedo with no other change, so its probe is the `expr` swap alone;
  showcases 2 and 3 are path-traced, where interreflection off the other
  surfaces is not linear in the receiver's albedo, so BOTH copies also
  rebind every non-receiver material to a black `lambertian_material`
  (showcase 2's water keeps its dielectric) with relief modifiers
  stripped. The probe `expr` is `vec3(<the signal def's name>)` —
  `dust` in showcases 1 and 3, `buried` in showcase 2. `SourceHygieneTest` hard-fails
  any test that defines an `IRasterizerOutput` unless the FILE contains
  the contiguous text `oidn_denoise false` (case-insensitive); the setter
  call `DocSetOrAddParamValue(doc, id, "oidn_denoise", 0, "FALSE")` does
  NOT contain it, so the test carries a comment holding the literal
  `oidn_denoise FALSE` next to that call, saying that the probe copies
  force it off. A probe copy re-points the shipped `thinlens_camera`
  (`location`, `lookat`, `up`, `focus_distance` via the same setter — no
  chunk swap; `pinhole_camera` has no `focal_length` and its `fov` is the
  VERTICAL angle, so it is not used) where the beauty camera cannot read a
  station (showcase 2). 512 spp on both copies: §8.2
  measured that a 16-spp divide-by-control probe produced ratios above 1
  because the two renders seed independently from the wall clock, and 512
  was needed for every ratio to land in range, worst where the control is
  dark; the ±0.08 band is stated on top of that. Budget: a probe pair
  (two 800 × 600 renders at 512 spp) must finish inside 3 minutes of wall
  clock on the gate machine, measured and written in the header; if a
  path-traced showcase exceeds it, the probe copies use a `film` of
  400 × 300 with every footprint below doubled and restated in the header
  (the bands do not change — they are stated on top of the noise the
  §8.2 precedent measured). The pixel footprint (mm) is stated; where
  the footprint is coarser than the station spacing the query station
  adjudicates and the pixel is a sanity check with a stated wide band. A
  painter station is read ONLY where the sightline from the probe camera to
  the station is unoccluded and, if it crosses a refractive interface, is
  within 10° of that interface's normal (so the refractive displacement is
  under a pixel — the header shows the angle).
- **Framing rule**: a receiver's 2 cm ring must image at ≥ 8 px across in
  the RADIAL direction (tangential size × sin(elevation); thin-lens
  magnification f/(d − f)), its face seen at ≥ 25° elevation, and every
  STATION inside the depth of field at a 1-pixel circle of confusion (a
  receiver's far parts may fall soft and the header says which); the
  header shows the arithmetic (distance,
  focal length, sensor — `sensor_size` is the HORIZONTAL extent — f-stop,
  focus distance, DoF limits: near `s·f²/(f² + N·c·(s − f))`, far
  `s·f²/(f² − N·c·(s − f))`). Every scene
  ships a `film` chunk of `width 800`, `height 600` (cameras are
  imaging-only); the circle of confusion c is one pixel of the 36 mm
  sensor, 0.045 mm, and every pixel count below is at 800 px wide. The
  camera must not lie inside any solid.
- **Beauty**: the scene renders at its shipped settings; a crop around the
  contact is written under `rendered/` (gitignored) and judged in the header
  honestly (what the signal drew, what it did not).
- **Costs**: the scene's wall clock with the signal live vs the same scene
  with the signal's `def` replaced by the constant 0 in EVERY chunk that
  declares it (the expression VM has no constant folding, so the rest of the
  program still executes and the delta isolates the signal call and its L1
  memo lookup), every core busy (`render_thread_reserve_count` is an
  OPTIONS-file setting read by `CPUTopology::ResolveReserveE`, not a scene
  parameter — `./bench.sh` sets it; the worker uses the same route and
  says which), 2 warm-ups then ≥3 interleaved pairs, every run listed. Target ≤ 1.15× (the plank's
  1.11–1.13× is the reference). The header names the CALL RATE regime: a
  `pixelpel_rasterizer` scene evaluates the painter about once per camera
  sample, a path-traced scene many times more — the three figures are not
  comparable with each other and §8.5 says so.
- **Ledgers**: `CstDeriveGoldenTest` gains one row per NEW scene (no tracked
  scene is edited by this spec, so no existing row drifts); the
  `scenes/FeatureBased/README.md` entry follows the plank entry's shape; the
  scene header records every measured number and every deviation from this
  spec; a NEW §8.5 "Showcases" in the design doc (it ends at §8.4 today)
  gets one paragraph per showcase pointing here.

## 1. `pavilion_colonnade` — contact dust at the foot of a fluted column (Phase 3, CSG neighbour)

**Why a new scene.** `glass_pavilion`'s shipped camera (85 mm at
(−3.5, 2, 6), f/1.4 focused at 7 m) frames NONE of the four caps (cap1/2/3
are outside even the 14.8° half-diagonal; cap4 at 10.6 m is defocused to
5.6 px while its 2 cm ring images at 3.6 px, under a pixel at 10° grazing).
The pavilion keeps its subject and its `capped TRUE` pin; the signal gets a
foot study of its own.

**New scene** `scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene`,
built from the pavilion's chunks (the same `colcylgeom`, `flutegeom`,
`capgeom`, all four columns and caps at (±2.5, ·, ±2.5) with the pavilion's
orientations, the floor, the ceiling and back wall for bounce, the two omni
lights), framed on cap1's foot. NO prop is relocated into the frame: the
pavilion's `vasegeom` is an unscaled Bezier teapot 6.5 world units wide
(`AddBezierPatchGeometry` recentres, never normalises), and the glass
sphere's pedestal is out of frame — this is a foot study of marble, floor
checker and the columns behind. Camera, verified in review against the
framing rule:
```
thinlens_camera
  location        -2.5    0.9452  3.8407
  lookat          -2.4111 0.175   2.7443      # the S1 station
  up              0 1 0
  sensor_size     36
  focal_length    50
  fstop           16                          # DoF [0.979, 2.140] m holds the whole cap
  focus_distance  1.343
```
Arithmetic: camera→S1 horizontal 1.100 m, Δy 0.770 → elevation 35.0°;
distance 1.343 m → a 2 cm ring images at 17.2 px tangential, 9.9 px radial
(thin-lens f/(d − f); 22.22 px/mm on the 36 mm/800 px sensor); at f/16 with a 1-px (0.045 mm)
circle of confusion the DoF is [0.979, 2.140] m, holding S1 (1.343), S7
(1.377), S6 (1.578) and the cap's far corner (1.891). One pixel at S1 is
1.16 mm tangential and 2.03 mm RADIAL (÷ sin 35°), so a pixel spans about
0.10 of signal across the 1 cm ramp: the query station adjudicates S1 and
S7, and their painter bands are the wide ±0.15 of §0's coarse-footprint
clause. The camera is 1.341 m
from column1's axis, above the floor, outside cap1's footprint. `pixelpel_rasterizer`
as the pavilion, 64 spp, `oidn_denoise` at the pavilion's setting for the
beauty render (the probe copies force it off).

**Receiver.** One new material `marble_cap` on all four caps (the frame
holds cap1; the others carry it harmlessly): a
`lambertian_material` whose reflectance is an `expression_painter` —
`def dust proximity(0.02)`; `expr mix(vec3(marble white), vec3(0.42, 0.36,
0.30), clamp(dust*0.85 + 0.08*fbm(P*180.0, 3, 0.5, 2.0), 0, 1))` with the
pavilion's marble white (no `jitter` — it is not a context variable; a
`seed` line plus `def jitter vec3(seed, seed*1.7, seed*2.3)` is the wear
prelude's way if breakup jitter is wanted). No relief, no roughness. A
`def` is per chunk.

**What the picture shows.** The band follows the COMPOSITE's cross-section
(the design's §8.4 crop): an outer ring hugging the round wall, BROKEN in a
wedge ~8 cm wide at EACH of the two slot mouths (`flutegeom` is 0.5 deep,
the column's diameter, so the slot cuts clean through: mouths at local ±z;
from a point 1 cm outside a mouth the nearest real surface is the
slot-wall/cylinder corner, 4.21 cm away), and two thin bands INSIDE each
mouth hugging the slot walls (at |x| = 0.03 the wall is 1 cm away → 0.5).
The camera at +z sees the near mouth; the far one is hidden by the column. A cylinder neighbour would draw an unbroken ring;
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
  90° station's sightline passes the cylinder with 5.2 mm clearance, a
  contaminated pixel): local (0.26 sin 20°, y, 0.26 cos 20°) = (0.088925, y,
  0.244320), 1 cm outside the round wall (the station's |x| = 0.0889 and
  the wall point's 0.0855 are both > 0.04, so the nearest surface is the
  wall; corner 4.9 cm away) → world (−2.411075, 0.175, 2.744320) → 0.5
  within the composite band; the sightline never comes nearer the axis
  than the station itself (unoccluded).
- S2 2 cm out on the same azimuth (radius 0.27) → 0; S3 4 cm out (0.29) → 0.
- S4 flute mouth local +z, 1 cm out: (−2.5, 0.175, 2.76) → 0 on the scene
  query at r = 0.02; the per-object `column1->DistanceToSurface` at r = 0.1
  REFUSES (the scene-wide query at 0.1 answers 0.075 from the floor top at
  y = 0.1 — the cross-check (l) uses).
- S5 on the rotated `column2` (`orientation 0 45 0`, present in the
  scene): local (0.088925, y, 0.244320) and local (0, y, 0.26) pushed
  through `column2->GetFinalTransformMatrix()` → 0.5 (band) and 0.
- S6 cap1 top, 6.5 cm from the wall = 0.315 from the axis along local +x:
  (−2.185, 0.175, 2.5) → 0 (nothing within 2 cm; the cap's half-width 0.35
  keeps it on the cap; the floor top is 7.5 cm below); sightline clears the
  column at 0.307 m.
- S7 inside the slot mouth on the exposed cap top: local (0.03, y, 0.20) →
  world (−2.47, 0.175, 2.70): inside the cylinder radius (0.2022 < 0.25),
  inside the slab (|x| = 0.03 < 0.04), so the composed field is
  `max(f_A, −f_B)` = +0.01 (outside the composite) and the nearest composite
  surface is the +x slot wall 1 cm away → 0.5 within the composite band
  (the boundary arm fires on the box's exact face; verified in review). The
  sightline enters the cylinder's disc at local x = 0.029 ≤ 0.04, through
  the mouth.
- Painter stations at S1, S6, S7 from the shipped camera (the cap is
  already a Lambertian; the `expr` swap alone is the probe): 0.5 / 0 / 0.5
  within 0.15 at S1 and S7 (2.03 mm/px radial, stated) and 0.08 at S6.
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
- `water`: `dielectric_material` (`ior 1.33`, `tau` WHITE — the
  descriptor's default `tau` is 0, a fully absorbing black block —
  `scattering 1000000`, delta pass-through) box 0.36 × 0.60 × 0.36 at
  (0, −0.27, 0) → x, z ∈ [−0.18, 0.18], y ∈ [−0.57, 0.03]; the WATERLINE
  is y = 0.03. Deep so that on every stone's side the TOP face is the
  nearest face. The camera is outside the box, so no IOR-stack seeding
  question arises.
- `pool_bed`: opaque sand box 0.34 × 0.53 × 0.34 at (0, −0.285, 0) → y ∈
  [−0.55, −0.02], inside the water; the stones rest on its top (y = −0.02),
  visible through the water as the pool floor. Its top computes to
  −0.019999999999999962 in double while a stone bottom at 0.02 − 0.04 is
  −0.02 exactly, so a resting point is inside the bed by 3.8e-17 and
  `DeepestOtherContainment` DOES count it — at depth 3.8e-17, dominated by
  the water's depth under the running MAXIMUM. The header says this; the
  test asserts the water's value.
- `sand`: four opaque strips, top at y = 0.03, inner faces 0.5 mm outside
  the water (no coincident face with the dielectric; the 0.5 mm seam images
  at about a pixel — 1.24 px face-on at 800 px, 0.86 px after the beauty
  camera's foreshortening — and reads as the pool's rim): `sand_px` 0.2195 × 0.10 × 0.80
  at (0.29025, −0.02, 0) → x ∈ [0.1805, 0.40], z ∈ [−0.40, 0.40];
  `sand_nx` mirrored at x = −0.29025; `sand_pz` 0.361 × 0.10 × 0.2195 at
  (0, −0.02, 0.29025) → x ∈ [−0.1805, 0.1805], z ∈ [0.1805, 0.40]; `sand_nz`
  mirrored. The z strips' end faces at x = ±0.1805 are coplanar with the x
  strips' inner faces: two opaque boxes of one material sharing a plane
  along a 0.22 m seam, stated here; it is not a receiver and no station is
  near it. None enters the pool footprint.
- `stone_a`: `sphere_geometry` r = 0.04 at (0, 0.02, 0): bottom −0.02 on the
  bed, top 0.06 (3 cm in air).
- `stone_b`: `ellipsoid_geometry` radii (0.05, 0.03, 0.035) at (0.09, 0.01,
  −0.05), `orientation 0 20 0` (a yaw leaves the y semi-axis vertical; bottom
  at 0.01 − 0.03 = −0.02 on the bed; its footprint x ∈ [0.04, 0.14] stays
  inside the bed). Visual variety; its container is the water box (exact).
- `stone_c`: `sdf_geometry` pebble — `sphere` r 0.03 at part origin and
  `sphere` r 0.022 offset (0.02, 0, 0), joined by `smin` k 0.007 (≈ a third
  of the small radius, the descriptor's guidance; spelled in the `part`
  grammar as `part sphere smin 0.007 <px py pz> <ex ey ez> <sx sy sz>
  <a b c> <round>` with `a` the radius) — at (−0.08, 0.01, 0.04)
  so its lowest point (0.01 − 0.03 = −0.02) rests on the bed. Container: the
  water box (exact).
- `stone_e`: a tilted FLAGSTONE — `box_geometry` 0.10 × 0.02 × 0.06 at
  (−0.02, 0.0111940, −0.10), `orientation 0 0 −25` (the scene language's
  Euler triple is applied about the box's own centre; a −25° rotation about
  z tilts the top normal to n = (sin 25°, cos 25°, 0) and the top face
  RISES toward −x along t = (−cos 25°, sin 25°, 0)). The top face's centre
  is the box centre + 0.01·n = (−0.015774, 0.0202571, −0.10); the rotated
  corners lie at y = −0.019 (1 mm above the bed, no interpenetration),
  −0.0009, 0.0233 and 0.0414, so the face crosses the waterline. A station
  at signed distance s along t from the face centre has y = 0.0202571 +
  s·sin 25°; the test SOLVES s for each target y (below) and asserts
  |s| ≤ 0.05. This is the receiver whose wet ramp is readable — from the
  OVERHEAD probe camera (§2 painter stations), not from the beauty camera,
  whose sightlines cross the water at ~51° to its normal.
- `stone_d`: `sphere_geometry` r = 0.03 at (0.26, 0.035, 0.09), on `sand_px`
  (x ∈ [0.23, 0.29] inside the strip's [0.1805, 0.40]): equator ring 5 mm
  above the sand, bottom 2.5 cm under. Its burial line is visible AT the
  sand surface; the buried ramp is not visible (opaque sand), so it carries
  no painter station.
Camera (beauty): `thinlens_camera` at (0.10, 0.36, 0.34) looking at
(0, 0.0, −0.02), 50 mm, f/22, focus 0.50 m (DoF [0.424, 0.608] m at a 1-px
circle of confusion, which holds stone_d at 0.44 m and stone_a's top at
0.46 m as well as the flagstone at 0.57 m); the view axis is at 43.9°, the
flagstone is seen at 36.8° elevation and its 2 cm ramp images at ≥ 8 px
radial (the header shows the arithmetic; the flagstone's top face is
seen at 38.9° above its own tilted face, 36.8° above horizontal, and the
sightline crosses the water at 53.2° to the WATER's normal — far outside
§0's 10° rule, hence the overhead probe). Lighting: the key is an
`omni_light` PINNED at (−0.35, 0.80, 0.45), camera-left and above; a dim
sky fills. `pathtracing_pel_rasterizer` with `transparent_shadows TRUE` —
MANDATORY and recorded in the header: the default is FALSE and
`RayCaster::CastShadowRayAuto` then runs the binary occlusion test, so
every NEE shadow ray from a submerged surface to any light is blocked by
the water box. With it on, `CastShadowRayTransmittance` carries the
water's Fresnel transmittance — but it returns 0 on total internal
reflection, so the key must lie within asin(1/1.33) = 48.75° of the
water's normal as seen from every submerged station; a RAKING key would
be blocked with the flag on exactly as off. From the pinned key the
deepest B9 station sees it 39.8° from vertical, B4 37.0°, stone_b's
bottom 39.1°, B8 31.3° (all exiting the water inside the pool footprint,
clear of the sand rim), so the submerged half is lit by NEE and the probe
controls are not dark. The key is 0.67 m from the overhead probe camera
and off its axis, so it occludes no probe sightline.

**Why `interior` and not `proximity`.** A point on a stone below the
waterline is 1–5 cm from the water's TOP face; `proximity(r)` would paint
everything under water uniformly (or nothing, past r). `interior(0.02)` =
clamp(depth/2 cm): 0 above the waterline, a ramp over the first 2 cm, 1
below — a crisp line AT the waterline and a sheen that saturates 2 cm down.

**Receivers' recipe.** Each stone is a `coated_material` whose `base` is a
`ggx_material` (`fresnel_mode schlick_f0`, a dielectric) with `rd` bound to
an `expression_painter` (`def buried interior(0.02)`; `expr mix(dry, wet,
buried)`; a fine `fbm` grain scaled by `(1 − buried)` so grain shows only
on the dry part) and `alphax`/`alphay` bound to a `scalar_painter`
(`expression mix(0.55, 0.12, buried)` with its OWN `def buried
interior(0.02)` — a `def` does not cross chunks; the second call is an L1
memo hit), and whose `coat_weight` is a third `scalar_painter` reading
`buried` — coverage is the physically right wetness slot and the one the
`add_wetness` verb drives. (`cooktorrance_material` is conductor-only;
`coated_material` has no reflectance slot of its own, hence the `base`.)
One field drives colour, roughness and coat; three chunks declare it. All
five stones carry the recipe; two (stone_a, stone_e) carry painter
stations, and for the probe those two are REBOUND to a Lambertian per §0
(the GGX `rs` and the coat would otherwise add the same term to probe and
control).

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
  bed counted at 3.8e-17 → running maximum 0.05 → 1.0.
- B6 stone_d in sand_px (sand top 0.03, centre (0.26, 0.035, 0.09), r 0.03):
  equator point (0.29, 0.035, 0.09) → 5 mm above the sand → 0; the point at
  y = 0.02 at horizontal radius sqrt(0.03² − 0.015²) = 0.0259808 toward +x,
  (0.2859808, 0.02, 0.09) → depth 0.01 → 0.5 (the strip's top is nearest:
  its inner face is 0.1055 away, its outer 0.114); the bottom (0.26, 0.005,
  0.09) → depth 0.025 → 1.0.
- B7 stone_b: the bottom point (0.09, −0.02, −0.05) → depth 0.05 → 1.0; a
  waterline point on the ellipsoid: solve the ellipsoid at y = 0.03 in its
  local frame (y_local = 0.02 = 2/3 of the 0.03 semi-axis, so the local xz
  ellipse has semi-axes 0.05·sqrt(5)/3 and 0.035·sqrt(5)/3), take the point
  at local +x, rotate by the 20° yaw, translate → 0.
- B8 stone_c: the test finds a surface point by bisecting the pebble's
  field in OBJECT space (`SDFGeometry::EvaluateParts(geo->GetParts(),
  ptObject)`, reached by `dynamic_cast<const SDFGeometry*>(
  obj->GetGeometry())` as `ProximitySignalTest` does) along −x at object
  y = 0.01, object z = 0, from object x = −0.07 toward 0 (bisection
  tolerance 1e-10 stated), then translates the root by the chunk's
  position (−0.08, 0.01, 0.04) → a world point at y = 0.02 on the pebble's
  −x side → depth 0.01 → 0.5 exact (container is the box). Feeding the
  WORLD coordinates to the object-space evaluator would land the station on
  the waterline and read 0.
- B9 stone_e (the flagstone): four points on its top face at the TARGET
  heights y = 0.0, 0.01, 0.02, 0.035 (s = −0.04793, −0.02427, −0.00061,
  +0.03488 along t, all within the 0.05 half-length) → depths 0.03, 0.02,
  0.01, −0.005 → 1.0, 1.0, 0.5, 0. The test builds the points from the
  chunk's centre, dimensions and orientation and asserts they lie on the
  face.
- Painter stations from an OVERHEAD PROBE CAMERA (the probe and control
  copies re-point the shipped `thinlens_camera`: `location −0.016 0.60
  −0.10`, `lookat −0.016 0.0 −0.10`, `up 0 0 −1` — the default `up 0 1 0`
  is parallel to the view and `SafeUnitUp_` would pick an arbitrary axis —
  `focus_distance 0.58`, the f/22 stays): the sightline to the y = 0.02
  ramp station is on the camera axis (0° to the water's normal, no
  refractive displacement), the deepest B9 station is 4.2° off axis (a
  0.5 mm ≈ 1 px lateral shift, harmless where the value is saturated at
  1.0), and the flagstone's top face is seen at 65° elevation; the angles
  are stated. Stations: B1 on stone_a's dry top (0) and the four B9 points
  (1.0 / 1.0 / 0.5 / 0) within 0.08. B3/B4 (a sphere's flank under water)
  are QUERY-only.
- Beauty: a crisp waterline on every wet stone, a sheen saturating 2 cm
  down on the flagstone and the sphere, dry grain above, the ellipsoid and
  pebble carrying the same line, stone_d's burial line in dry sand. Judged
  honestly.

**Cost.** Live vs `def buried 0` in ALL THREE chunks of every stone at
the shipped spp; the box containers are the cheapest family, so this gate bounds
`interior`'s own overhead (the running maximum over the TLAS's containing
leaves) in the path-traced regime.

## 3. `shelf_bunny` — mesh-to-mesh and mesh-to-plank contact (Phase 2)

New scene `scenes/FeatureBased/Textures/shelf_bunny.RISEscene`, a 0.5 m
close-up. Layout:
- `shelf`: `box_geometry` 0.60 × 0.025 × 0.24 at (0, −0.0125, 0) → top face
  y = 0, x ∈ [−0.30, 0.30], z ∈ [−0.12, 0.12]; a `lambertian_material`
  whose reflectance is a painted, slightly worn `expression_painter` (the
  plank's wear recipe at the author's discretion, plus `def dust
  proximity(0.02)` mixed into a warm grey) — Lambertian so the §0 probe is
  the `expr` swap alone; the bunny's material is the same kind.
- `wall`: `box_geometry` 0.60 × 0.40 × 0.02 at (0, 0.19, −0.13) → its front
  face at z = −0.12 is flush with the shelf's back edge (the wall's
  y ∈ [−0.01, 0.39] and the shelf's back face share a 0.60 × 0.01 m
  coplanar patch, hidden below and behind the shelf top; stated, not
  moved); a matte plaster painter; it reads no signal.
- `bunny`: `risemesh_geometry` (`file models/risemesh/bunny.risemesh`), `position 0
  Y_b 0` with Y_b = −(lowest vertex y) so the lowest vertex touches y = 0
  (scene D: −0.0329874 for a plane at 0 — the same number here, re-derived
  by the test from the vertex array and asserted at 1e-6). The bunny's
  footprint must stay inside the shelf and > 2 cm from the wall: the test
  asserts the bunny's bounding box against the plank's extents.
- `dragon`: `risemesh_geometry` (`file models/risemesh/dragon_small.risemesh`) at
  scene D's ABSOLUTE position (−0.0318315, 0.135862595, −0.014760295),
  which is relative-to-bunny once corrected by (0, Y_b + 0.0329874, 0) —
  identically zero here since Y_b = −0.0329874 — `scale 0.35 0.35 0.35` (three
  components — a single number derives to a DEGENERATE (0.35, 0, 0)
  transform silently), its lowest vertex on the bunny's highest, re-derived
  and asserted at 1e-6.
Camera: `thinlens_camera` at (0.22, 0.25, 0.42) looking at (0, 0.06, 0),
50 mm, f/16, focus 0.52 m (DoF [0.458, 0.601] m at a 1-px circle of
confusion: M1 at 0.547 m and every M3 candidate — +z 0.511, +x 0.524, −x
0.574, −z 0.585 — are inside, so the direction the test derives cannot
fall soft; the bunny's near flank at 0.40 m and the shelf's near edge fall
soft, stated in the header); the shelf top at the contact vertex is seen
at 27.2° elevation from 0.547 m, the 2 cm ring at 20 px radial (44.7 px
tangential × sin 27.2° at 800 px). A raking key from the left; `pathtracing_pel_rasterizer`.

**What it shows.** Mesh neighbours as SHEETS: the shelf reads
`proximity(0.02)` and draws a dust ring around the bunny's single contact
vertex (the bunny touches at ONE vertex; scene D's "four" were four probe
directions from it); the BUNNY reads `proximity(0.02)` from the dragon's
feet (mesh ↔ mesh, the case scene D pins at 1.0 with the bunny as self) and
darkens there; the dragon reads nothing. The shelf ALSO reads the wall: its
back edge is flush with the wall's front face, so a 2 cm dust band runs the
full length of the shelf along the wall, reading 1.0 at the joint and 0 by
2 cm out — the box neighbour's exact closed form, predicted here and
pinned by M5, and the reason the bunny must sit > 2 cm clear of it
(asserted by the test from the bunny's bounding box).

**Stations** (r = 0.02; a `NearestOtherSurface` point query does not depend
on the receiver's family, so scene D's measured values transfer to the plank
at the same relative offsets).
- M1 shelf top (y = 0), 1 mm outside the contact vertex's xz in each of ±x,
  ±z → ≥ 0.9 (scene D: 0.9519 / 0.9740 / 0.9865 / 0.9913 — re-measured
  here, not copied).
- M2 shelf top, 2 mm outside, worst direction → ≥ 0.9 (scene D: 0.9037 —
  the margin is stated).
- M3 shelf top, 5 cm from the contact vertex in the direction the test
  DERIVES as clear by the quantity it asserts — the 3D distance, not a
  column test: among ±x, ±z it picks the direction whose minimum distance
  from the station to any bunny vertex exceeds 0.025 (a triangle interior
  can be nearer than any vertex, so the margin is the guard) AND whose
  sightline from the camera passes ≥ 2 mm from every bunny vertex → 0.
  Predicted from the vertex array: −x is clear by 33.1 mm with a sightline
  ≥ 20.5 mm from the bunny; +z is only 21.5 mm clear; +x (4.6 mm) and −z
  (11.9 mm) would read ~0.77 and ~0.41 and their sightlines graze the
  bunny at 0.2 mm. The chosen direction and the clearances found are
  printed and recorded in the header.
- M4 the bunny's highest vertex with the bunny as `self` → 1.0 (the
  dragon's lowest vertex sits on it, distance 0); 5 mm below it (inside
  the bunny — a query-only station) → 0.75 EXACTLY, the one mesh station
  with a closed form: the dragon's lowest world vertex IS the contact
  point and no dragon point lies below that plane, so every dragon point
  is ≥ 5 mm from a station 5 mm straight below it, with equality at the
  shared vertex (`MeshClosestPointTest` 6b makes the same argument and
  measures 0.75). Asserted at 1e-4 (absorbing the 1e-6 placement
  tolerance); the test re-derives the vertex from the dragon's array.
- M5 shelf top at z = −0.11 (1 cm from the wall's front face at z = −0.12),
  x = 0.20 (away from the bunny) → 0.5 exact (box neighbour, 1e-9); at
  z = −0.09 → 0.
- Painter stations (both copies black-out everything but the shelf per
  §0; footprint at the contact 0.45 mm/px tangential, 0.98 mm/px radial
  at 800 px): NOT at M1 — the sightline to each 1 mm station passes
  within 0.1–0.6 mm of a bunny vertex, so that pixel straddles the bunny's
  own silhouette (§8.2's nail failure) — but at M1p, 5 mm from the contact
  vertex in M3's derived clear direction: predicted ~0.75 (the query
  measures it first and adjudicates; the sightline must clear every bunny
  vertex by ≥ 2 mm, checked) → the query's value within 0.15; and M3 → 0
  within 0.08. No painter station on the bunny at M4: the pixel there
  shows the dragon's foot, not the bunny.
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
