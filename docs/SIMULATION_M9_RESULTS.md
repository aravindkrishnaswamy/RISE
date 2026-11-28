# M9 results: inflow sources and drains; the barrier-hole check; analytic bodies on cloth with body-body contact

2026-11-27. Branch `simulation-liquid`, from `9f746162e` (uncommitted, in the working tree). Requested by the
showcase round's "Engine requests" ([SIMULATION_SHOWCASE.md](SIMULATION_SHOWCASE.md)). Contract: one Appendix C row
(additive; no member added or changed). Versions unchanged: `liquid.mac` m7m.1, `rigid.bodies` m6.2, `cloth.shell`
untouched. Every configuration without the new parameters keeps its effective parameters, key and frames (§6).

| Task | Outcome |
|---|---|
| 1. Liquid inflow sources (`liquid.mac`) | **Done** (§1): `sources` / `drains` (chunk fields `source`, `drain`): box, cylinder nozzle or sphere, velocity, window, schedule, rain drops; volume accounting in the progress detail and the correction's target; deterministic, thread-count invariant, checkpointed; works with viscosity (honey pour), rigid bodies and sheets (rain on cloth). New fast test `LiquidSourceTest`, scene `liquid_inflow`, two demo renders each |
| 2. `monsoon` barrier holes | **Done** (§2): not holes. The check misjudged faces whose lattice segment a sagging sheet crosses twice; it now judges only faces crossed once; `monsoon` proof bake: 862 → **0** (20 faces not judged); frames unchanged |
| 3. Engine request 1: analytic bodies on cloth with body-body contact | **Done** (§3): body option `surface <object>` (`surface_input`, or inline `surface_vertices`/`surface_triangles`); new fast test `ClothRigidPrimitiveTest`; a scene trampoline with a sphere and a box bakes |
| 4. Engine request 5: cloth-body contact in a liquid group | **Assessed, not built** (§4): what is missing is listed |
| 5. Fire requests 3, 4 | Not touched (fire agent's) |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | `LiquidSource` (shape, distance, schedule, exit area, support, drops, nominal volume); `LiquidSetup::sources`; `ApplySources` (end of each step's level-set stage), `ImposeSources` (velocity at the step's start; a drop's before its step's projections); `MaxStep` takes the sources' speed; state `sourceVolume`, `sourceMeasured`, `sourceActive` (`SourceState`/`SetSourceState`); `SheetBarrierHoles(unjudged)` judges only faces crossed once |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | `ParseSource`; parameters `sources`, `drains` (written back only when present); evidence condition `sources`, scope sentence, preview limit, notes (nominal flux, regions thinner than two cells); checkpoint member `sources` (only with sources); state token; progress detail `SourceDetail`; barrier detail wording and the unjudged count |
| `src/Library/Simulation/Liquid/LiquidParameterSchema.cpp` | chunk fields `source`, `drain` and their translation |
| `src/Library/Simulation/Rigid/RigidSolver.cpp`, `RigidParameterSchema.cpp` | body `surface_input` / `surface_vertices` + `surface_triangles` (analytic shapes); chunk body option `surface <object>` |
| `src/Library/Simulation/SimulationWorker.cpp` | a single-solver bake prints the solver's last progress detail when it finishes (`rise-sim: last progress: ...`) |
| `scenes/Tests/Simulation/liquid_inflow.RISEscene` (new) | a ramped tap, a rain shower and an overflow drain over a basin; 40 cells, 46 frames |
| `tests/LiquidSourceTest.cpp` (new), `tests/ClothRigidPrimitiveTest.cpp` (new), `tests/test_schedule.tsv` | the two tests and their rows |
| `tests/data/cst_derive_golden.txt` | the new scene's line (only that line) |
| `src/Library/Parsers/README.md`, `docs/SIMULATION_SOLVER_CONTRACT.md` (Appendix C), `docs/SIMULATION_SHOWCASE.md` (Engine requests: status lines) | documentation |

No library source file added (build lists and the Xcode project unchanged); no chunk keyword added (191); the new
tests are found by the test build's glob.

## 1. Inflow sources and drains

### 1.1 Syntax

```text
liquid_simulation
{
    ...
    source  tap cylinder -0.08 0.25 0  0 -1 0  0.02 0.04  speed 0.8 end 1.2 schedule 0 0.1 0.2 1
    source  shower box 0.09 0.27 0  0.18 0.03 0.26  speed 1.2 start 0.2 end 0.8 drops 3 4 0.022 0.15
    drain   overflow box 0.18 0.08 -0.13  0.04 0.04 0.04
}
```

- `source <name> box cx cy cz sx sy sz [orientation w x y z] | cylinder cx cy cz ax ay az r h | sphere cx cy cz r`
  (centre and size; a cylinder is a nozzle: centre, axis, radius, length along the axis), then `speed s` (m/s,
  default 1), `direction dx dy dz` (default the cylinder's axis, else `0 -1 0`), `start t`, `end t` (the window
  `[start, end)`), `schedule t f [t f ...]` (a piecewise-linear factor on the speed, held outside the table) and,
  for a box, `drops nx nz r period` (rain).
- `drain <name> <shape> [orientation ..] [start t] [end t]`.
- Solver parameters `sources` and `drains`: lists of maps (`name`, `shape`, `center`, `size`, `orientation`,
  `axis`, `radius`, `height`, `direction`, `speed`, `start`, `end`, `schedule`, `drops`); refused: unknown keys, a
  drain with a speed/direction/schedule/drops, drops on a non-box, a schedule whose times do not increase or whose
  factors are negative, `end <= start`, duplicate names.

### 1.2 The model

- **Continuous source.** At the step's start the open faces inside the region take speed × factor(t) × direction
  (and count as known for the extrapolation), so the level set leaves the region at that velocity; at the end of the
  step's level-set stage (after the reinitialisation, the particles, the volume correction and the sheets' guards) the
  region is refilled: φ ← min(φ, d_region) in the region's cells and around it, except beyond its exit plane (the
  jet that left; refilling there widened the accelerating, thinning jet each step and pumped liquid through the
  exit). Air marker particles inside the region are removed. A step is active when its midpoint is in the window and
  the factor is positive. Holding the region's velocity through the projections as well was tried and dropped: it
  held back the stretching jet below the nozzle (axis speed 1.5 cells below the exit 6.5% under free fall, 3.8% when
  set at the step's start only).
- **Drops.** A box with `drops nx nz r period` is split along its local x and z into nx × nz columns; each column
  releases one sphere of radius r once per period at phase `hash(source, column) × period`, at a position hashed from
  (source, column, period index) inside its column (and the box's height). The schedule's factor is the chance of a
  release (a hash below the factor). A released drop fills its sphere and takes the velocity on the faces inside it
  before the step's projections. No random state: every drop is a function of the indices and times.
- **Drain.** φ ← max(φ, −d_region); liquid marker particles inside are removed.
- **Volume accounting.** The volume correction keeps `targetVolume`. A source's first active step (and the first
  after an inactive one) adds its measured fill (the region's volume in the correction's smoothed measure); later
  steps add the nominal flux, exit area × speed × ∫ factor over the step (the integral of the piecewise-linear
  schedule is exact, so the sum telescopes to the nominal volume); a drop adds 4/3 π r³; a drain removes what it
  measured. The level set's own refills are reported too: they are larger than the nominal flux because the region's
  refill also replaces what the transport erodes inside it, which the volume correction would put back anyway
  (counting it would double it). Measured (A, nozzle of radius 2.5 cm): refills over the added volume +41% at 24
  cells (2 cells of radius), +33% at 32, +21% at 48 (§1.4); the correction removes that excess globally.
- **Time step.** With sources `MaxStep` takes each source's speed × its largest factor from the start (a
  conservative, time-independent bound).
- **State.** Per source the added (or drained) volume, the refills and the active flag, in the checkpoint's header
  member `sources` (only with sources) and the rollback token.
- **Progress detail** (and, new, printed by a single-solver `rise-sim` bake when it finishes): `source <name> added
  X L (the level set's refills Y L)`, `drain <name> removed Z L`, `liquid volume V L (target T L: ...)`.
- **Label.** Always preview: evidence condition `sources` (`drain`, `drops`, `inflow`), a scope sentence, the limit
  "sources: inflow and drains are not certified (preview)". Notes give each source's exit area and nominal flux and
  flag a region thinner than two cells.

### 1.3 Stated limits

- A source should emit into air: inside liquid it adds only what flows out of it.
- A jet narrower than about three cells thins with its fall and its tip erodes (B: the front of a jet thinning to 1.5
  cells trails the free fall of its first liquid by 5 cells after 0.16 s, while the axis speed follows free fall);
  drops below about two cells evaporate into the volume correction.
- Keep a region two cells or more from sheets and bodies (the refill does not know their sides).
- Drains remove everything inside them, so a basin's level is set by where the drain sits (an overflow).

### 1.4 Tests (`LiquidSourceTest`, fast tier, about 40 s)

| Case | Result |
|---|---|
| A: nozzle 2.5 cm radius, 1 m/s, 0.3 s over a pool, drain from 0.2 s, 24 cells | added = first fill 0.0633 L (region 0.0589 L) + nominal flux 0.5825 L exactly (gate 1e-9); target = initial + added − drained to 1.7e-15; liquid volume within 0.01% of the target; drain removed 0.205 L; refills +41% (24 cells; +33% at 32, +21% at 48, `LST_CELLS`) |
| A: the same ramped (`schedule 0 0 0.1 1 0.2 1 0.3 0.5`) | nominal 0.4417 L, exact; refills +53% |
| A: a box source turned 0.3 rad, 0.6 m/s, window 0.05-0.3 s | nominal 0.3700 L, exact; first fill 0.0859 L for a 0.075 L region (smoothed measure of a 2.4-cell-thick box) |
| B: downward jet, 1 m/s, nozzle radius 3 cm (2.4 cells), 0.16 s | axis speed against free fall from the exit at 0.5 / 3 / 6 / 10 / 15 cm below: 1.053/1.048, 1.203/1.260, 1.386/1.442, 1.707/1.749, 1.976/2.010 m/s (gate 6%); radius against continuity r0 √(v0/v): 29.8/29.3, 28.2/26.7, 25.5/25.0, 23.3/22.7, 21.4/21.2 mm (gate half a cell); front 0.206 m (free fall of the first liquid 0.144 m; gate 70% of the fall) |
| B: horizontal jet, 0.8 m/s, radius 3.5 cm, 0.3 s | centre height against the parabola at x + 0.2 = 0.03 … 0.12 m: −0.35, −0.57, −1.04, −1.34 cells (gate 1.5) |
| C: rain, 4 × 4 columns, drops of 2 cells, period 0.1 s, window 0.3 s | 48 drops released, as the window gives; the target gains 48 drop volumes exactly; the level set's fills +21% (smoothed measure of 2-cell spheres) |
| D: a 500 kg/m³ box (7.5 cm, vertical motion free) floating in a basin a nozzle fills with 1.14 L | the box rises 11.7 mm with the level's 13.6 mm (gate 30%) |
| D: rain (49 drops, 0.56 L) on a held, implicitly coupled sheet across the box | nothing more than a cell below the sheet (0.0000 L; gate 1%). (A prescribed m7d sheet leaked 8% of a dropped block in the same setup without any source: m7d's preview leak, not the sources') |
| E: the solver | 1 and 3 threads: identical frames; a checkpoint at frame 4 restored: identical; the detail reports the volumes; without sources the effective parameters are unchanged (an empty list is the absent one; their digest pinned to 9f746162e's, and the key of the same configuration baked by the start and final `rise_sim` is identical); refusals; the chunk syntax |

### 1.5 Scenes and renders

- `scenes/Tests/Simulation/liquid_inflow.RISEscene`: 40 cells, 46 frames; baked in 66 s (an intermediate build of this round, before the nominal accounting; not rebaked). Golden line
  added.
- **Honey pour** (scratch scene, `rendered/simulation_m9/m9_honey_pour{,_denoised}000{0,1}.png`, t = 0.8 and 1.8 s):
  10 Pa s, 1420 kg/m³, nozzle radius 1.6 cm (2.6 cells at 56 cells, 6.25 mm), 0.25 m/s, ramped in over 0.15 s, shut
  at 1.6 s, falling 0.3 m onto the floor. The rope falls and thins (its radius about halves) and piles into a mound
  with a raised centre where it lands; no coiling at this resolution (a coiling rope needs the rope several cells
  across where it buckles). Bake 337 s, 0.38 GB; added 0.3345 L, liquid 0.3345 L.
- **Rain on cloth** (scratch scene from `liquid_cloth_catch`, `rendered/simulation_m9/m9_rain_cloth{,_denoised}000{0,1}.png`,
  t = 0.6 and 1.4 s): 48 cells, a cotton square (0.5 kg/m², pinned all round, implicit coupling) under 4 × 4 columns
  of 2.2 cm drops every 0.1 s for 1.2 s. The drops land and pool on the cloth, which bellies; the water then runs
  off its edges. Group bake 90 s; detail: rotation stretch up to 0.81%, linearisation correction up to 0.72% (no
  warning), barrier holes 0 (32 faces not judged), cells wet across a sheet 26 (held), added 8.56 L.

## 2. `monsoon`'s barrier holes (task 2)

Reproduced on a scratch proof copy (64 cells, awning 21 × 18) with a temporary trace of each hole's path (removed):
862 is the same count as the showcase's. Every printed path was two or three cells long beside the awning's low
pinned edge, where the sagging cloth's lowest row lies within a millimetre of a row of cell centres (vertices at
y = 0.3726-0.3728 m, the cells' centres at 0.3727 m). A lattice segment along x there crosses the sheet twice (down
and up again within the cell); FindSheetCrossings closes that face (it is crossed) and the check labelled its two
cells front and back by the crossed triangle's normal, although both lie below the sheet. The fill then ran from a
falsely "front" cell to genuinely back cells through ordinary open faces: not a path through the sheet. So the check
misfired; the barrier was watertight (and no leak is visible in any frame; the barrier itself is unchanged).

Fix (diagnostic only): a face's cells take sides only when the segment between their centres crosses the sheet once,
counted against the crossed triangle's one-ring (triangles that close no face skipped); rim triangles' singly crossed
faces mark a cell they contradict as "both" (left out). The progress detail now reads `barrier holes (paths of open
faces through the sheet pinned all round) N (faces crossed twice by the sheet, not judged: up to M)`. Results:

| Run | holes | not judged | pressure iterations |
|---|---|---|---|
| `monsoon` proof copy, start binary | 862 | (not counted) | 63,007 |
| the same, fixed check | **0** | 20 | 63,007 (identical) |
| rain on cloth demo | 0 | 32 | |
| `LiquidClothCouplingTest` M (intact pinned sheet: 0; a quad removed: found) | passes | | |

## 3. Analytic bodies on cloth with body-body contact (engine request 1)

`rigid.bodies` bodies of analytic shape (sphere, box, cylinder) take an optional coupling surface:
`surface_input <scene input>` (chunk: `body <name> sphere r ... surface <object>`) or inline `surface_vertices` +
`surface_triangles`, world space at t = 0 with the body at its initial pose. The record's surface (what
`rigid_surface` places) is then that mesh in the body frame (R_bᵀ (y − x_b)); the cloth's `collider_port <object>`
samples the same object, so the topologies match and the adapter drives it. The body keeps its analytic shape for
its mass properties, plane contact and body-body contact. A mesh body with a surface is refused. Written back only
when given; the scene input enters the key then.

```text
rigid_simulation { name drop  body ball sphere 0.1 mass 0.1 at 0 0.36 0 surface ball
                   body crate box 0.12 0.12 0.12 mass 0.1 at 0.03 0.62 0.01 surface crate }
standard_object { name ball  geometry geo_ball  position 0 0.36 0  transform_source drop.bodies[ball] }   # likewise crate
cloth_simulation { ...  collider_port ball collider_ball  collider_port crate collider_crate }
simulation_group { ... connect drop.body_state -> sheet.collider_ball via rigid_surface body=ball
                   connect drop.body_state -> sheet.collider_crate via rigid_surface body=crate
                   connect sheet.collider_reaction -> drop.body_load via contact_reaction targets=ball,crate
                   policy implicit  on_non_convergence halve  minimum_macro_step 0.002 ... }
```

`ClothRigidPrimitiveTest` (fast tier, about 50 s; surfaces a latitude-longitude sphere and an 8-corner box, unlike
the record's own icosphere):

| Case | Result |
|---|---|
| A: zero gravity, free cotton sheet: a box (0.15 kg, 1.2 m/s up) strikes a sphere at rest, which is thrown into the sheet | total momentum of sheet and both bodies conserved to **4.6e-6** of the box's (gate 1e-3); sphere up to 0.864 m/s, box slowed to 0.624 m/s (body-body); sheet momentum up to 0.023 N s (cloth contact) |
| B: trampoline (pinned corners, gravity): a box dropped on a sphere | closest centres 0.1302 m (touching at 0.13: the box rests on the sphere); both centres stay at least 0.063 m above the sheet's lowest point; the sheet sags to −0.137 m |
| C: the record | carries the given surface in the body frame (radius error < 1e-12); without one, the icosphere; version m6.2; a mesh body with a surface refused |

Scene check (scratch copy of the trampoline with `surface` on a sphere and a box, 31 frames): the group bakes; with
`policy implicit` alone one step at 0.35 s (box on sphere on cloth) did not converge in 30 sub-iterations (residual
4.3e-4 N against 1e-4), with `on_non_convergence halve` it completes (121 s). The committed trampoline scene is
unchanged and bakes identically (§6).

## 4. Cloth-body contact inside a liquid group (engine request 5): assessed, not built

Task 3's change gives the bodies a cloth-collider surface, but inside a liquid group the rest is missing:

1. **The body's load.** The liquid already feeds `rigid.bodies`' `body_load` (owned exchange); the group refuses two
   connections into one port, so the cloth's `contact_reaction` cannot reach the body. Needs a second accepted load
   port on `rigid.bodies` (summed) or a summing adapter.
2. **The liquid's copy of the cloth.** With `coupling implicit` the liquid solves a linearised copy of the cloth and
   the cloth replays the liquid's step impulses (`surface_load_steps`; m7k's replay keeps the copies equal). A collider
   contact acting in the cloth's own step is not in the copy, so the copies part wherever the body touches the
   cloth; the contact impulse would have to enter the liquid's sheet solve (or the copy).
3. **The liquid's copy of the body.** The liquid integrates its own copy of each port body (with `body_contact` for
   planes and bodies); it would also need the cloth as a contact partner.
4. **Implicit iteration.** Cloth-body contact needs sub-iterated coupling (the conjurer history: staggered contact
   with a resting body fails); the group refuses sub-iterations beside an owned exchange unless every member is in
   one, so cloth-rigid contact would run staggered, one macro step lagged.

Not contained in the coupling, cloth or rigid code: it changes the liquid's implicit sheet solve and the group's
policy. Left open.

## 5. Not done, inferred, verified

- **Verified (run):** every table above; the builds; the tier (§6); the byte-identity bakes and keys (§6).
- **Inferred:** that the refill excess is erosion inside the region (from its convergence with the grid and the jet
  speeds following free fall); that the scene trampoline's non-convergence at 0.35 s is the resting-contact case the
  conjurer history describes (halving converges).
- **Not done:** coiling honey (resolution); a source inside liquid adding a set flux (it needs a mass source in the
  pressure solve); drains with a rate; engine request 5 (§4); the Xcode project (no file added); fire tests (not run).

## 6. Bytes, keys, tests

- **Keys.** `rise_sim status` on every committed simulation scene (tests and showcase, a snapshot taken at the
  start): the start binary (built from the unchanged tree) and the final one print the same keys for every non-fire
  simulation (42 of 44); the two fire keys differ by the fire agent's uncommitted changes in the shared tree (not mine).
- **Bytes.** Reduced bakes (13 frames; liquid 24 cells, showcase 32; cloth at most 13 × 13; showcase scenes at their
  proof values) of every committed non-fire simulation scene, every group and every producer no group lists, with the
  start binary (built from the unchanged tree before any change) and the final one, each into its own cache
  directory: **all 28 member caches identical** (`manifest.cbor` byte for byte and the content-addressed frame and
  static names): `cloth_chain_scene_inputs` (cover, drape), `cloth_flag_scene`, `conjurer` (props, silk), `honey`
  (honey, linen), `liquid_cloth_catch` (cloth, pool), `liquid_cloth_flap` (flap, pool), `liquid_cloth_wave` (cloth,
  pool), `liquid_dam_break`, `liquid_honey_cloth` (cloth, pool), `liquid_honey_water` (honey, water),
  `liquid_rigid_bodies` (drop, pool), `monsoon` (canvas, floats, rain), `rigid_cloth_trampoline` (drop, sheet),
  `tide` (blocks, water), `unveiling` (silk). The showcase scenes were a snapshot taken at the start (the showcase
  agent edits them meanwhile).
- **Tests.** Non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 455 built (453 + `LiquidSourceTest` +
  `ClothRigidPrimitiveTest`); first run 454 passed, `CstDeriveGoldenTest` failed only for the new scene's missing
  line (UNCOVERED 1), added (only that line: the regeneration also rewrote three showcase lines the showcase agent's
  edits move; those were restored, theirs to regenerate); final run: **455 passed, 0 failed** (417 s wall). Under the tier's load
  `LiquidSourceTest` took 45 s, `ClothRigidPrimitiveTest` 91 s.
- **Build.** `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free (Homebrew linker
  notices only).
