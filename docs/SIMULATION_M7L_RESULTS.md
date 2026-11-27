# M7l results: the level set two-valued at a sheet; thin layers on curved sheets (`liquid.mac` m7l.1)

2026-11-27. Follows [M7k](SIMULATION_M7K_RESULTS.md), [M7j](SIMULATION_M7J_RESULTS.md), [M7i](SIMULATION_M7I_RESULTS.md).
Contract: no Appendix C row (no port, member or parameter added or changed; the changes are behavioural, inside the
liquid solver; the checkpoint and cache layouts are unchanged).

**Status.** One model changed. Versions: `liquid.mac` `m7k.1` → `m7l.1`; `cloth.shell` unchanged (`m3a.7`);
`rigid.bodies` unchanged (`m6.2`). Sheets stay `preview`.

| Task | Outcome |
|---|---|
| 1. Thin layers on curved sheets | **Built; largely fixed** (§1). The level set is two-valued at implicitly coupled sheets: each cell beside a sheet keeps its own side's value and reads, across the sheet, the other side's level set extended to it along the sheet's normal (as a rigid solid's cells hold the liquid's), evaluated from the current field at every Runge-Kutta and reinitialisation stage; m7f's boundary values (the distance to the sheet) are gone. Without the volume correction, 24 cells, 2 s: held bowl's 1.3-cell rim sloshing **−43.8% → −2.5%** (−2.8% with the sheet's part, §1.3; 48 cells −14.9% → −3.3%, not converging, with up to 0.4% of the volume below the sheet beyond its cells); two-sided bowl (liquid below too) −1.0%; the M7i/M7j bag's 2.4 cells **−41.4% → −4.3%** (target ~2% **not met**: the remainder is not the layer's depth, §1.4); catch at 64 cells 0 holes, held crossings **393 → 0**, but its corotational indicators rose (0.54%/0.58% → 1.4%/2.6%, warnings; §1.6); at 128 cells 17,105 → 888 held crossings and, measured, no leak and the same held water within 2% (§1.7); nothing below the bag's sheet beyond the cells beside it. Stated depth 5 cells → about 2.5 cells |
| 2. Viscosity with sheets | **Not done** (§2): still refused |
| 3. 3-D momentum audit | **Not done** (§3) |
| 4. Second-order cut-cell velocity | Not done |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidLevelSet.{h,cpp}` | `LevelSetBarrier::Second` (the second values: slots, sides, linear recipes, the dry-side test; `Evaluate`, `Value`); `Build` takes them and its stencil samples read them across a closed face (`Sample::second`); the boundary kind (2) and the reinitialisation's `fixed` cells removed; `OneSidedDerivatives`, `UpwindAdvectionRhs`, `Reinitialize` evaluate the second values per stage; `InterfaceArea` takes the barrier (one-sided gradient) |
| `src/Library/Simulation/Liquid/LiquidFlow.{h,cpp}` | `FaceTheta`, `PressureOptions::faceTheta`, `GhostTheta`: a caller's free-surface fraction on given faces (projection, its update, the pressure gradient) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | `BuildBarrier` builds the second values (`ExtensionRecipe`, `TrilinearWeights`, `CrossingPoint`); `PinSheetInterface`, `Redistance`, `OwnInterface` removed; `SweepSheetCells` gives a swept cell its new side's extension (and finds cells whose side flipped); `PrepareTwoValued`; `Across`; `SheetFaceTheta` (the face liquid, shared by `SheetFaceMass`); `SheetFreeSurface` (the projection's free surface on a sheet face's open part); `SheetVolume` (in `Volume`); `OpenUnwetted` and `ClearSheets` read the wet side's own level set; the volume correction with coupled sheets uses `Volume` and the barrier's area; a `held` stage hook |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | version m7l.1 |
| `src/Library/Simulation/Liquid/LiquidParameterSchema.cpp`, `src/Library/Parsers/README.md`, `docs/SIMULATION_SOLVER_CONTRACT.md` §6 | the stated depth: about 2.5 cells |
| `tests/LiquidClothCouplingTest.cpp` | N (thin layers: the held bowl and the bag, 1 s each); K and the cup probe measure with the sheet's part (`CupSample::topSheet`); the rig's momentum takes the solver's face liquid (`SheetFaceTheta`); `--cup under 1` (the bowl wet on both sides); `LCC_ONLY`; diagnostics `M7L_SIDE` (stage sums over the current side), `M7L_GUARDDUMP` (second values, their memory, the dry-side test); `--measure` (a baked catch's volumes above, below and outside the sheet's footprint, `M7L_AIR`); m7j's pinned-cell switch count removed |
| `tests/LiquidRigidTest.cpp`, `tests/LiquidSheetTest.cpp` | version pins |
| `tests/test_schedule.tsv` | `LiquidClothCouplingTest` seconds (N added) |

No source file, scene, chunk keyword or parameter added: build lists, keyword count, `RewireConnectionTest` and the cst
golden are unchanged.

## 1. Thin layers on curved sheets (task 1)

### 1.1 What was built

M7j §1.4's design, with M7k §2's correction (the other side's values recomputed per stage):

- **Second values.** The band is every cell within a stencil's reach of a closed face along its axis (offsets −3..2
  of the face). Each band cell is on one side (in front of the crossed triangle or behind it) of every crossing whose
  lattice line holds it; a cell given two sides (a fold, two sheets) has none. A band cell's second value is the
  other side's level set at its centre, extended along the normal of its nearest crossing: the trilinear value at
  x_o, 1.5 h beyond the crossing's plane on the other side at the foot of the cell's centre, continued linearly to the
  cell with the normal difference to x_o + h n (slope clamped to 1). The recipe (16 weights) is refused when any of
  its cells is of the wrong side or of none. The recipes are fixed by the geometry (built with the barrier); the
  values are evaluated from the field passed in, at every SSP-RK3 stage of the advection and every stage of the
  reinitialisation.
- **The stencils.** A WENO sample that crosses a closed face reads the second values of the cells beyond it (of the
  stencil cell's opposite side), until the domain's edge, a second closed face or a cell without one (then the last
  value reached). Where no second value can be read, m7f's constant extension stays.
- **The dry-side test.** Without it, a side whose scene level set is the distance to the liquid on the other side
  (a bag's air below the sheet, the bowl's) extends to a zero at the sheet, and its cells beside the sheet hovered
  about zero (the held bowl's dry side turned 1,234 cell-steps wet in 2 s, the bag held 1.2% of its volume wet below
  the sheet). When the level set at x_o is ≥ 0 and the other side's cells at the foot of the cell are all at least
  h/4, the value is at least their least value. Without the h/4 margin, a liquid side whose cells at the sheet had
  dried to about zero (the bag's corners at its pinned rim) stayed dry (bag −7.8%, against −4.0%). A version ramped
  in over h/4..h/2 (continuous) was tried on the catch: it did not lower the catch's kicks (§1.6) and held 446
  crossings; not kept.
- **Sweeps.** A cell the sheet moved over takes its new side's level set extended to it from 2 h beyond the new
  plane (which no cell swept in a step reaches); the cells whose side flipped between the old and new barrier are
  swept as well (no case here had one the line pairing missed). m7f-m7k's pin at the sheet's new position and the
  re-distancing of the cells it left are removed.
- **The start.** A scene's level set is one-valued (its liquid ending at a sheet holds the distance to the sheet on
  both sides). Once, before the first step, the band cells take their own side's extension from 2 h (deeper only)
  where wet, and at least 1.5 h where dry with no wet cell within two steps on their side; the target volume is
  measured after it (`PrepareTwoValued`). Without it the two sides relaxed over the first steps, which the volume
  measure read as a change (a piston's acceleration 1.8% low).
- **What reads the side's value.** The face liquid moving with the sheet (`SheetFaceTheta`: each side's own level
  set up to the crossing), the contact rule for a free surface below a sheet (`OpenUnwetted`), the projection's free
  surface on the open part of a sheet's face between cells of its two sides (`SheetFreeSurface`, `PressureOptions::
  faceTheta`: the wet side's liquid ends there, at the crossing at most), the surface output's vertices on edges
  across a sheet (`ClearSheets`: where the wet side's own level set ends, at the sheet at most) and the volume
  measure (`SheetVolume`: the liquid between the centres of each closed face, θh, for the two half cells the cell
  measure gives them, the faces' shares n_a² of the sheet's area). With the face theta and free surface from the
  own values (as m7k) the membrane frequency (E) moved from −5.5% to −6.7%; with the volume measure on cells only,
  the piston's acceleration was 0.24-0.54% off and its momentum audit 0.4-0.7% (gate 0.5%): both back with these.
- **Checkpoints and caches.** The second values are not state; the checkpoint holds the level set as before (an
  m7k.1 checkpoint is read: its pinned values are a valid level set, which relaxes as at a scene's start but without
  the start's preparation). Frames are unchanged in layout.

### 1.2 Memory

Per band cell (slot): 8 (cell) + 4 (side) + 1 (valid) + 4 (start) + 8 (reach) + 4 (dry start) + 4 (reference
crossing) bytes, its recipe's terms (up to 16 of 20 bytes) and its dry-test cells (up to 8 of 4 bytes), about 0.4 KB.
Measured (`M7L_GUARDDUMP`): the 24-cell bag, 576 closed faces, 3,456 slots, 9,408 terms, 4,704 dry-test cells:
0.32 MB; at 48 cells (2,304 faces) 1.2 MB, about 0.5 KB per closed face; a tilted or curved sheet's recipes have more
terms (up to 16), at most about 2.4 KB per closed face. Per stage, one array of doubles per slot is evaluated
(transient). The barrier's samples are the same size as before (the flag fits the padding). The 128-cell catch's
peak resident set was 2.15 GB against 3.5 GB in M7k (both include the other agents' changes of the moment; the
difference is not this change's).

### 1.3 The minimal tests (24 cells, 0.3 m tank, cotton, 1/120 s, no volume correction, 2 s)

`LiquidClothCouplingTest --cup` (m7k.1: the binary built from the tree at the start; m7l.1: final):

| Case | m7k.1 | m7l.1 |
|---|---|---|
| held bowl, 1.3-cell rim, sloshing (`bowl 1 u0 0.2 depth 0.016`) | −43.8% | **−2.5%** (with the sheet's part −2.8%) |
| the same at 48 cells | −14.9% (M7j) | −3.3% (−4.0%); up to 0.40% of V0 below the sheet beyond the cells beside it (24 cells: none) |
| the same bowl wet on both sides (`under 1`) | | −0.8% (−1.0%) |
| rigid bowl, same (`--layer bowl 1 depth 0.016`) | −1.5% | (unchanged path) |
| bag (2.4 cells, sags 3 cm; `depth 0.03`) | −41.4% | **−4.8%** (−4.3%) |
| held flat sheet, 2.4 cells, sloshing (`frozen 1 u0 0.2`) | −3.8% | −6.1% |
| rigid floor, 2.4 cells, sloshing (`--layer u0 0.2`) | −4.2% | (unchanged path) |

The sharp count is clamp(1/2 − φ/h, 0, 1) h³ over the cells on the liquid's side; "with the sheet's part" adds
`SheetVolume`'s face terms with the sharp count (the cell count alone moves by the sheet's motion inside the cells
beside it: ±1.5% as the bag sags and rebounds, which m7f's pinned values made part of the count).

At 48 cells the bowl loses more than at 24 (−3.3% against −2.5%) and some of its liquid reaches below the bowl
beyond the cells beside the sheet (0.40% of V0 at most; none at 24 cells): the result does not converge with the
grid in this case. Not analysed (a 48-cell run before the start's preparation and the projection's sheet free surface
were added gave −1.8% with 0.07% below).

### 1.4 Where the bag's 4% goes

Stage sums over the current side (`M7L_SIDE`) of the bag: the loss is in the first 0.6 s, while the bag sags at up to
0.16 m/s (from 0.6 s to 2 s the count changes by −0.1%). It does not shrink with the layer's depth in cells: at 48
cells (4.8 cells deep) the bag loses 4.4% (m7k.1 at 4.8 cells: −5.2%, M7j §1.5), and a rigid floor's 2.4 cells
sloshing loses 4.2%. So the remaining loss is not the thin-layer representation this task addresses but the
sagging sheet's motion through the cells (the sweeps and the advection beside a moving staircase); the target of
about 2% is not met.

### 1.5 The flat held sheet

Sloshing 2.4 cells on a held flat sheet loses 6.1% (m7k.1 3.8%; a rigid floor 4.2%). The flat case is where m7f's
boundary values were exact (the sheet a face plane); the reinitialisation's constant ghosts at the tank's walls,
which m7g set for coupled sheets, remain (with linear ones the flat case lost 4.7% but the bowl leaked 11% below
its sheet and the bag lost 4.7%). Not pursued further.

### 1.6 The demos

64 cells, 49 frames (1.6 s), final binary:

| Demo | m7k.1 | m7l.1 |
|---|---|---|
| `liquid_cloth_catch`: barrier holes / held crossings | 0 / 393 | **0 / 0** |
| the same: rotation stretch / linearisation correction | 0.54% / 0.58% | **1.4% / 2.6% (warnings)** |
| a thin-layer variant (the catch's cloth with the water a 3 cm layer on it from the start, 3.2 cells): held crossings / indicators | 385 / 0.076%, 0.045% | **38** / 0.12%, 0.043% |

The catch's corotational indicators rose above their warning level: the sheet is kicked harder as the water
arrives. Without the dry-side test they were 0.94% / 1.1% but 2,649 crossings were held; with a continuous test,
1.4% / 2.6% and 446. Not resolved. 

**128 cells** (the committed catch scene at 128 cells, 17 × 17 cloth, 49 frames, 1.6 s; final binary; 964 s beside
other work, 2.15 GB peak): barrier holes 0, held crossings **17,105 → 888** (0.89 → 0.046 litre), rotation stretch
0.14% (m7k.1 0.085%), linearisation correction 0.12% (0.059%), no warnings.

**Renders** (`rendered/simulation_m7/`, outside git; path tracing 640 × 400, denoised copies beside them):
`m7l_catch_128_0000.png` (scene camera, t = 0.8 s) and `m7l_catch_128_below_0000.png` (camera 0.95 0.04 1.05 looking
at 0.3 0.25 0.3); `m7l_layer_64_0000.png` and `m7l_layer_64_below_0000.png` (the thin-layer variant at 64 cells,
t = 0.8 s: the layer stays on the sagging cloth, nothing below it).

### 1.7 Is the 128-cell catch leaking? (measured: no)

The m7l render at 128 cells was read as a regression (a nearly empty cloth, a pile under its centre, streams from under
its interior). Measured instead: the catch baked again with `publish_fields TRUE` (25 frames, 0.8 s) by the m7k.1
binary (`05eab7170`) and the final m7l.1 binary, and `LiquidClothCouplingTest --measure <pool cache> <cloth cache>`
reading each frame's level set and cloth: the sharp volume clamp(1/2 − φ/h, 0, 1) h³ above the cloth, below it inside
the footprint of its pinned rim (x, z in 0.1..0.5 m), the part of that more than 1.5 h below the cloth (the cells
within 1.5 h hold the dry side's values beside the sheet; m7k's pinned distances count there in part), the part of
that below y = 0.15 m (the floor), outside the footprint, the total, and the cloth's lowest point. Litres:

| Cells, binary | t (s) | above | below in footprint | > 1.5 h below | of it on the floor | outside | total | cloth lowest y (m) |
|---|---|---|---|---|---|---|---|---|
| 64, m7k.1 | 0.2 | 4.672 | 0.136 | 0 | 0 | 0 | 4.809 | 0.2109 |
| | 0.4 | 3.616 | 0.160 | 0 | 0 | 0.889 | 4.665 | 0.2608 |
| | 0.6 | 4.122 | 0.190 | 0 | 0 | 0.369 | 4.682 | 0.2545 |
| | 0.8 | 4.471 | 0.216 | 0 | 0 | 0.060 | 4.748 | 0.2479 |
| 64, m7l.1 | 0.2 | 4.865 | 0.009 | 0 | 0 | 0 | 4.874 | 0.2067 |
| | 0.4 | 3.715 | 0 | 0 | 0 | 0.961 | 4.676 | 0.2597 |
| | 0.6 | 4.278 | 0 | 0 | 0 | 0.400 | 4.678 | 0.2544 |
| | 0.8 | 4.686 | 0.018 | 0.018 | 0.018 | 0.073 | 4.776 | 0.2477 |
| 128, m7k.1 | 0.2 | 4.736 | 0.069 | 0 | 0 | 0 | 4.805 | 0.2101 |
| | 0.4 | 3.155 | 0.096 | 0 | 0 | 1.525 | 4.775 | 0.2616 |
| | 0.6 | 2.807 | 0.197 | 0.095 | 0.095 | 1.667 | 4.672 | 0.2607 |
| | 0.8 | 3.563 | 0.887 | 0.791 | 0.791 | 0.246 | 4.696 | 0.2526 |
| 128, m7l.1 | 0.2 | 4.819 | 0.002 | 0 | 0 | 0 | 4.821 | 0.2076 |
| | 0.4 | 3.210 | 0.002 | 0 | 0 | 1.553 | 4.765 | 0.2605 |
| | 0.6 | 2.788 | 0.118 | 0.117 | 0.116 | 1.772 | 4.678 | 0.2608 |
| | 0.8 | 3.489 | 0.997 | 0.995 | 0.981 | 0.197 | 4.683 | 0.2533 |

(Start: 4.828 L at 64 cells, 4.806 L at 128.) At both resolutions and in both binaries the impact throws water over the
rim (outside the footprint, up to 1.8 L at 128 cells by 0.5 s), which falls to the floor and spreads under the cloth
from 0.57 s; the water below the cloth inside the footprint is on the floor (y < 0.15 m) except for 0-14 mL in m7l.1
from 0.57 s, and every such cell (frame by frame, `M7L_AIR`) lies within two cells of the footprint's edge: streams
falling from the rim, none under the cloth's interior. At 0.8 s m7l.1 holds 3.489 L on the cloth against m7k.1's
3.563 (−2%) at 128 cells and 4.686 against 4.471 at 64; the cloth's lowest point is the same within 0.7 mm. m7k.1's
"below" of 0.07-0.22 L before any water reaches the floor is its pinned dry-side values beside the sheet, not water.
So there is no leak through the cloth's interior in either, and m7l.1 does not hold materially less: the render's
impression does not survive measurement (a re-render of the m7k.1 bake at 0.8 s reproduces M7k's image; from the
camera below, the far rim's streams project under the cloth's middle). The flood-fill watertightness check is
geometric (closed faces and open fractions, not the level set) and still reports what it says: 0 holes in all four.
Not checked: 0.8-1.6 s.

## 2. Viscosity with sheets (task 2): not done

Task 1 took the session. Viscosity with sheets stays refused ("sheets with viscosity are not supported"). What it
needs (not built): the faces a sheet closes fixed at the sheet's velocity in the viscous solve (a no-slip wall, the
ghost-fluid theta to the crossing), the sheet's vertex velocities unknowns of that solve with its step matrix S
added to the energy as the rigid bodies' mass is (symmetric positive definite), and the viscous impulse added to
the one the cloth receives.

## 3. 3-D momentum audit (task 3): not done

## 4. Second-order cut-cell velocity (task 4): not done

## 5. Bytes: which reference scenes changed

The m7k.1 binary (built from the clean tree at the start) and the final binary baked reduced copies (M7i's scripts), each in
its own cache directory; content-addressed frame names compared:

| Scene (reduced) | Result |
|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | identical (39) |
| `liquid_cloth_flap` (staggered; explicit sheet) | identical (27) |
| `liquid_cloth_flap`, one-way | identical (27) |
| `liquid_honey_water` (24 cells, 7 frames) | identical (7) |
| `cloth_flag_scene` (31 frames) | identical (32) |
| `rigid_cloth_trampoline` (31 frames) | identical (63) |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | **12 of 27 differ** |
| `liquid_cloth_catch` (24 cells, 13 frames; implicit) | **21 of 27 differ** |
| `liquid_cloth_catch` (full, 64 and 128 cells) | **differ** (§1.6) |

Changed: only scenes with implicitly coupled sheets, as the version note says (bakes without them publish m7k.1's
frames). The old binary was built from the clean tree at `05eab7170`; the final one also holds the performance agent's
uncommitted cloth, cache and CBOR changes of the moment, and the cloth and rigid scenes are identical nonetheless.

## 6. Tests, pins

- **Pins.** `liquid.mac` m7k.1 → m7l.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` m3a.7 (`ClothSolverTest`
  unchanged).
- **Coupling suite** (`LiquidClothCouplingTest`, all pass): A, B, C unchanged; D piston error 9.8e-6 / −1.2e-6, audit
  5.5e-4 / 1.4e-4 (m7k.1 −3.4e-5 / −1.3e-5, 5.8e-4 / 1.5e-4); E membrane −5.48% (−5.48%); F, G, H, I pass; J −0.35%
  (m7k.1 −0.34%); K the bag holds (with the sheet's part +5e-5 of V0; the cell count alone −1.4%); N (new) bowl
  −0.8% in 1 s (gate 3%; m7k.1 −24%), bag −4.9% (gate 6%; m7k.1 −18%), nothing below the bag's sheet beyond the
  cells beside it (two cells beside it wet for one step at 0.18 s, 1.5e-3 of V0); L, M unchanged. 47 s (m7k.1 about
  24 s). `LiquidSheetTest` passes.
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the performance agent's
  uncommitted changes in the tree): 452 built, **452 passed** (`SimulationGoldenCacheTest` among them; 269 s wall).
  Fire tests were not run.
- **Build.** `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free (Homebrew linker
  notices only).

## 7. Verified, inferred, not done

- **Verified (measured).** §1.7's volume table (both binaries, 64 and 128 cells), §1.3's table (both binaries), §1.4's stage sums and the 48-cell bag, §1.5, §1.6's bakes
  and their variants, the suite's readings, the byte comparison.
- **Inferred.** §1.4's reading that the bag's remaining loss is the sagging sheet's motion through the cells (from
  its timing and its independence of the depth in cells; the mechanism not isolated); §1.6's attribution of the
  catch's kicks to the arriving water (the dry-side test changes them; the path not isolated).
- **Not done.** Tasks 2-4; the bag's 2% target; the bowl's convergence at 48 cells and its liquid below the sheet there; the catch's indicators; the catch after 0.8 s; the VS2022, Android and Xcode builds (no file
  added); the GUI; fire tests.
