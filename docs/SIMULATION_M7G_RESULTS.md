# M7g results: second round of sheet defects; rigid contact in liquid (`liquid.mac` m7g.1)

2026-11-26. Follows [M7f](SIMULATION_M7F_RESULTS.md), [M7e](SIMULATION_M7E_RESULTS.md) and
[M7b](SIMULATION_M7B_RESULTS.md); design [M7_DESIGN](SIMULATION_M7_DESIGN.md) §2. References checked by the
references agent: [SIMULATION_M7_REFERENCES.md](SIMULATION_M7_REFERENCES.md). Contract: one Appendix C row (solver
side) and one host-side note (the group keeps each member's progress detail).

**Status.** Three causes found and fixed, all in the implicit sheet coupling; bakes without implicitly coupled sheets
publish the frames of m7f.1 (§9). `liquid.mac` is now `m7g.1`. Sheets stay `preview`.

| Task | Outcome |
|---|---|
| 1. Near-zero values in dry corners below a sagging bag | **Cause found and fixed**: the reinitialisation's linear ghost values at the domain walls are unstable on the dry side (§1). Cotton bag, 6 cm, 3 s: liquid beyond the sheet's cells 0 at 24/40/64 cells (m7f.1: 0, 0.27%, 1.2%); sharp volume −2.7%, −1.7%, −1.1% (m7f.1: −2.5%, −3.3%, −2.3%); correction work +5.8%, +6.0%, +5.6% (m7f.1: −16%, −7.0%, −2.3%) |
| 2. Thin layers on a sheet | **Sheet-specific, mechanism located, not fixed** (§2). On a rigid floor a layer of 1.2–4.8 cells holds its sharp volume to 0.4% over 2 s; on the sheet the 2.4-cell layer still loses 8.4% (unchanged), and without the correction now loses 33% (m7f.1 gained 99%, mostly the wall artefact of §1). Limit: about 4.8 cells on a sheet |
| 3. Fast motion of light flaps | **Two coupling artefacts, both fixed** (§3): the face liquid drove the sheet at 1/n_a times its tangential speed, and a closed face loaded the sheet across an air gap. Still water, 40 cells, 1/120 s, 0.2 kg/m²: 1.15 → 0.089 m/s (every light flap 0.075–0.10); dam break at 40–64 cells 1.0–4.3 m/s (m7f.1 1.2–5.4). At 24 cells the dam-break peaks stay (3.8–14.9 m/s): a third mechanism, the closed face's flux constraint at the coarse edge, located, not fixed |
| 4. Through-sheet measure | **Built** (§4), reported in the progress detail and, for group bakes, by `rise-sim`. Catch (64 cells): 459 cell wettings across the cloth attempted and held, none passed; wave: 0 |
| 5. 128 cells | **Run** (§5): the catch at 128 cells with a 33 × 33 cloth, 0.8 s |
| 6. Cut-cell velocity order | **Not changed** (§6): cause analysed, no contained improvement built, no new table |
| 7. Rigid contact in liquid | **Described and measured** (§7): a sphere settling on the floor rests exactly at contact, no jitter, momentum closes; body–body contact does not exist; two smaller defects stated, not fixed |
| 8. 3-D momentum audit | Not built (§8) |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidLevelSet.{h,cpp}` | `Reinitialize(…, wall)`: the ghost values beyond the domain's walls, linear (m7a) or constant; barrier samples beyond the edge marked |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Constant wall ghosts in the reinitialisation with coupled sheets; the face mass along the sheet's normal (`SheetFaceMass`); `OpenUnwetted`/`CloseUnwetted`; `CountSheetCrossings` and the run totals `sheetCrossedCells`, `sheetCrossedVolume`, `sheetUnfedCells`; capillary-limit comment (Denner and van Wachem 2015) |
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `SheetCoupling` skips the opened faces; Guendelman, Selle, Losasso and Fedkiw 2005 (name corrected) |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | Version m7g.1; the progress detail adds the PCG iteration total and the through-sheet counts |
| `src/Library/Simulation/SimulationGroup.{h,cpp}` | `MemberDetail`: a member's last progress detail inside a group (the group passed no progress object before) |
| `src/Library/Simulation/SceneSimulationWorker.cpp` | A group bake prints each member's last detail at the end |
| `tests/LiquidClothCouplingTest.cpp` | Probes `--layer` (§2), `M7G_DEEP` / `M7G_COL` diagnostics in `--cup`, the through-sheet line in `--cup` |
| `tests/LiquidRigidTest.cpp` | Probe `--settle` (§7); version pin |
| `tests/LiquidSheetTest.cpp` | Version pin |
| `docs/SIMULATION_M7E_RESULTS.md`, `docs/SIMULATION_M7F_RESULTS.md` | The thin-shell paper's authors corrected |

No source file added: the build lists are unchanged. No chunk keyword changed (count 191).

## 1. Dry corners below a sagging bag (task 1)

### 1.1 Diagnosis (measured)

The cup of M7f §4.2 (cotton, 6 cm of water, 21 × 21 vertices, rim pinned, 1/120 s), with two diagnostics in
`--cup`: `M7G_DEEP` (the smoothed volume of the region well below the sheet, y < y0 − 0.055 m, per stage of the step,
and the marker particles there) and a per-stage record of the largest fall of φ in the lowest ten rows.

- **What grows.** With m7f.1 at 40 cells the region below stayed at its distance values until about 2.3 s, then in
  0.25 s it filled with values near zero: least φ 2.3h → 0.003h, 7,750 then 92,000 positive marker particles reseeded
  into the band that appeared. The cells were not in a corner only: a whole layer of cells against a side wall, at
  the tank floor, held φ ≈ 0.45–0.5h with the next layer inward at about 1.5h. That is a false interface lying on the
  wall plane.
- **Which stage.** The fall was made by the reinitialisation. In one run (40 cells) a front moved down the corner
  column (x = 0.004, z = 0.296) from below the rim to the floor in 0.05 s, about 1.2h per step, while the
  reinitialisation's pseudo-time step is 0.3h: |∇φ| there was about 5.
- **The mechanism (derived, then confirmed).** Beyond a domain wall the level set's stencils use a linear ghost,
  φ_ghost = φ_0 + (φ_0 − φ_1). On the dry side (φ > 0) the Godunov Hamiltonian takes max((D⁻)₊², (D⁺)₋²) per axis. If
  a wall cell is lower than its inner neighbour (φ_0 < φ_1), the ghost is lower still, so D⁻ = (φ_1 − φ_0)/h > 0 is
  counted as an upwind slope coming from outside the wall, beside the true slope from the interface (here the sheet
  above). Then |∇φ| > 1, φ_0 falls, φ_1 − φ_0 grows: a feedback that ends only where φ_0 is near zero (the sign
  function vanishes), with the zero on the wall plane. Nothing in the dry region restores it, because a distance to
  a plane on the wall is itself a valid distance field. Below a sagging sheet the level set beside the walls has
  exactly that shape (the pinned rim on the wall, the sag inward), so the dry side starts it. With the particles off
  or the reinitialisation off it did not appear (M7f §4.2); the particles' share was not isolated.
- **The test.** Constant ghosts at the walls in the reinitialisation only (zero normal slope, a 90° contact angle),
  everything else unchanged:

| 40 cells, 6 cm, 3 s | Beyond the sheet's cells (max) | Deep region at 3 s | Sharp volume | Correction work |
|---|---|---|---|---|
| m7f.1 | 2.7e-3 of V0 | least φ 0.000h, 1.5e-2 of V0 | −5.3% | −7.0% |
| m7f.1 + constant wall ghosts | 0 | least φ 2.2h, 0 | −1.7% | +33% |

(The m7f.1 run here differs from M7f §4.2's 0.27%: the onset time is sensitive; the 3 s run of §4.2 caught its start.)

### 1.2 Fix

With implicitly coupled sheets, `Reinitialize` uses constant ghosts at the domain walls (`LiquidLevelSet.h`). Runs
without them keep m7a's linear ghosts and their bytes. The advection keeps linear ghosts in both. Whether the same
instability appears without sheets was not measured; m7a chose linear ghosts by measuring free surfaces at walls, and
changing them everywhere would need M7a's tables rerun.

### 1.3 Result (m7g.1, all three fixes)

| Cotton bag, 6 cm, 3 s | Sag | Beyond the sheet's cells (max) | Wet cells below | Sharp volume above | Correction work | Barrier |
|---|---|---|---|---|---|---|
| 24 cells | 39 mm | 0 | 0 | −2.7% | +5.8% | intact |
| 40 cells | 39 mm | 0 | 7.8e-5 (transient) | −1.7% | +6.0% | intact |
| 64 cells | 39 mm | 0 | 0 | −1.1% | +5.6% | intact |
| 24 cells, no correction | | 0 | 0 | −5.5% (smoothed −2.9%) | — | intact |
| 40 cells, no correction | | 0 | 0 | −4.7% (smoothed −3.0%) | — | intact |

m7f.1 for the same rows: 0 / 0.27% / 1.2% beyond; sharp −2.5%, −3.3%, −2.3%; correction −16%, −7.0%, −2.3%; without
correction the smoothed volume grew 8.1% and 1.3%. The sharp volume above plus the dry side's half-cell count is
within 0.07% of the start in all three corrected runs. The 64-cell run took 558 s and 0.8 GB.

## 2. Thin layers on a sheet (task 2)

### 2.1 The control: the same depth in cells on a rigid floor (measured)

`--layer`: the cup's tank without a sheet, a layer of depth d on the floor, set sloshing by a uniform initial velocity
u0 along x, 24 cells, 2 s, the same numerics. Sharp volume change (with the correction):

| Depth (cells) | u0 0.1 m/s | u0 0.2 m/s |
|---|---|---|
| 1.2 | +0.34% | +0.22% |
| 2.4 | −0.07% | +0.003% |
| 3.6 | +0.006% | −4.1% |
| 4.8 | −0.15% | −0.05% |

Without the correction, 2.4 cells at 0.1 m/s: −2.1% (smoothed). At 0.3 m/s (Froude number about 0.4 at 3 cm) the
2.4-cell layer loses 11% and the 4.8-cell one 6%, and the 3.6-cell row at 0.2 m/s loses 4%: steep sloshing fronts in
a layer a few cells deep lose sharp volume on a rigid floor too. The bag moves more slowly than that after its first
0.25 s (vertex speeds below 0.05 m/s).

### 2.2 On the sheet (measured)

| Cotton bag, 3 cm (2.4 cells), 24 cells, 2 s | Sharp volume above | Smoothed |
|---|---|---|
| m7f.1, correction | −8.4% | held |
| m7g.1, correction | −8.4% | held (correction +27% of V0) |
| m7f.1, no correction | +76% | +99% |
| m7g.1, no correction | −41% | −33% |

So the layer on the sheet is not resolution alone: at the same depth in cells and slower motion, the rigid floor
holds it. Without the correction, m7f.1's growth was mostly §1's wall artefact; with it gone, the layer loses volume.

### 2.3 Where (measured; not fixed)

Stage sums over the run (m7g.1, no correction): the pin added +6.4 V0, the reinitialisation removed −6.2 V0, the
advection −0.70 V0, the sweeps +0.22 V0. The two first nearly cancel: in a layer 2.4 cells deep, the cells beside the
sheet have the free surface within two open steps ("an interface of their own", `OwnInterface`), so they are pinned
in some steps and not in others as the surface moves. When pinned their value is the sheet's distance; when not,
the reinitialisation moves them towards the free surface's distance. The net loss is in the advection, which sees
alternately the two values.

Tried and reverted: pinning those cells to the nearer of the two boundaries (|φ| = min(|φ_own|, distance to the
sheet)). The 3 cm layer then lost 90% without the correction: shrinking |φ| on the wet side every step drains it.

**Limit.** A layer on a sheet needs about 4.8 cells (the 6 cm bag at 24 cells holds −2.7% sharp, correction +5.8% over
3 s); 2.4 cells is under-resolved there, with the correction doing a quarter of the volume's work in 2 s. Stated in
the solver's preview scope; not fixed.

## 3. Fast motion of light flaps (task 3)

### 3.1 Diagnosis (measured)

A temporary trace in the solver printed, per liquid step, every sheet vertex faster than a threshold with its velocity
before and after, its impulse, the predictor V*, and every closed face of its triangles (axis, weights, φ, q, the face's
predicted velocity u*, the face mass).

**(a) Above still water (M7e §4 flap, 0.2 kg/m², 40 cells, 1/120 s).** The 1.15 m/s is one liquid step at t = 1.783 s:
edge vertex 47 (3 cm above the water, mass 1.3e-4 kg) went from −0.08 to 1.13 m/s with an impulse of 2.2e-4 N s. The
only loaded face of its triangles was a *y* face closed at y = 0.412 m between a wet cell (centre 0.3875, φ = −0.54h)
and the air cell above. The free surface lies at θ = 0.48 of the way, y = 0.3995: the sheet is 1.2 cm above the water,
in air. Closed, the face took away the wet cell's free surface, and the cell's pressure (q = 0.066) pushed on the
sheet across the gap. The next step the vertex was back at the flap's speed. This is an artefact: liquid cannot load a
sheet it does not touch.

**(b) Dam break (0.05 kg/m², 24 cells, 1/30 s).** At t = 0.917 s vertex 60 went from −0.20 to −9.65 m/s and back to
+9.72 the next step with no impulse (elastic recoil). Its predictor V* was already −10.1 m/s: the face mass. One y face
with n_a = 0.16 (the flap nearly vertical, the staircase's face beside it) carried liquid of mass 0.062 kg (1,800 times
the vertex's) with u* = −1.7 m/s. M7e's face-mass energy ρh³w(G_fV − u*)²/2, G_fV = (n_w·V) n_a, makes the face's
liquid move with the sheet *along the axis*: a light sheet's normal velocity becomes u*/n_a, here 10.8 m/s. Liquid
sliding along a sheet (a tangential velocity on a face that the staircase assigns to the sheet) drove it across.

### 3.2 Fixes

- **Face mass along the normal** (`SheetFaceMass`). The face's liquid moves with the sheet in the sheet's normal
  direction, with the normal part of its axis velocity: energy ρh³w(n_w·V − n_a u*)²/2. The matrix becomes
  ρh³w (w n)(w n)ᵀ (m7e: ρh³w n_a² (w n)(w n)ᵀ); the right-hand side, the momentum the liquid brings, is m7e's
  unchanged, so the weight the face mass carries (M7e's reason for it) is unchanged. The two agree for an
  axis-aligned sheet. Robinson-Mosher et al. 2008 lump the fluid's full mass onto the solid's nodes (per the references
  document), so the full mass is closer to the paper than m7e's n_a².
- **No load across an air gap** (`OpenUnwetted`). A face a coupled sheet closes between a wet cell and a dry cell is
  opened for the step's projections (as a free-surface face, p = 0 at θ) when the free surface lies short of the
  crossing (θh < distance to the crossing − 0.01h), the wet cell has an interface of its own (not a pinned boundary
  value), and the dry cell lies above the wet one against gravity. It stays closed for the level set (barrier, sweeps,
  pin) and is closed again after the projections. Both restrictions were measured:
  - Without the pinned-cell exclusion, a bag's cells beside several closed faces (holding their least distance, which
    is not θ along the others) were opened every step, and the correction put back 1.4 V0 per second.
  - Without the gravity condition, a 3 cm layer resting on the sheet (its cells beside the sheet holding the free
    surface's values, zero short of the sheet by 0.02–0.3h) drained into the opened faces; the correction put back a
    third of the volume per second. Liquid above a sheet with a sub-cell gap falls onto it within the step and its
    weight must reach the sheet.

### 3.3 Result (M7e §4 sweep, m7g.1)

Each entry: max |v| m/s (submerged, when different); roughness. 2 s, liquid 8 threads, cloth 4.

Still water:

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | 0.100 (0.046); 0.014 | 0.099 (0.047); 0.015 | 0.102 (0.002); 0.009 | 0.078 (0.029); 0.0007 | 0.085 (0.034); 0.001 | 0.075 (0.004); 0.0004 |
| 0.2 | 0.097 (0.076); 0.016 | 0.097 (0.080); 0.015 | 0.098 (0.006); 0.009 | 0.081 (0.045); 0.0007 | 0.089 (0.058); 0.001 | 0.087 (0.009); 0.0009 |
| 1 | 0.096 (0.090); 0.017 | 0.096 (0.093); 0.016 | 0.096 (0.029); 0.018 | 0.097; 0.002 | 0.091 (0.083); 0.002 | 0.093 (0.054); 0.002 |
| 10 | 0.20; 0.020 | 0.22; 0.036 | 0.22; 0.037 | 0.24; 0.013 | 0.22; 0.008 | 0.22; 0.006 |
| 100 | 0.70; 0.22 | 0.75; 0.17 | 0.77; 0.19 | 0.69; 0.17 | 0.75; 0.13 | 0.76; 0.12 |

Dam break:

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | **14.9**; 1.96 | 4.32; 1.34 | 3.73 (2.04); 1.03 | **6.82**; 1.95 | 2.42 (2.04); 0.78 | 2.78 (2.18); 0.80 |
| 0.2 | 3.84; 1.08 | 2.35; 0.81 | 1.80; 0.49 | **8.64**; 1.57 | 3.26; 0.84 | 2.10; 0.51 |
| 1 | 5.01; 2.56 | 2.41; 2.31 | 1.62; 0.59 | 2.58; 1.59 | 2.12; 1.35 | 1.96; 0.24 |
| 10 | 2.79; 1.95 | 1.70; 0.69 | 1.85; 0.87 | 2.54; 1.75 | 1.66; 0.88 | 1.73; 0.68 |
| 100 | 1.07; 1.62 | 1.08; 1.27 | 1.04; 1.40 | 1.00; 1.24 | 1.01; 0.77 | 1.08; 0.66 |

All 60 runs are stable (no vertex above 20 m/s, no failed step).

Reading:

- **Above still water** the light flaps' 0.32–1.15 m/s at 40 cells are gone: every still-water entry from 0.05 to 1 kg/m² is now 0.075–0.10 m/s (m7f.1: up to 1.15),
  the part in water within 0.01 m/s of m7f.1's or lower, and the roughness 2–50 times lower at 1/120 s (0.0004–0.002
  against 0.001–0.07). The 10 and 100 kg/m² rows are as in m7f.1 (the heavy flap bouncing on its own stretch).
- **Dam break** at 40 and 64 cells every peak is 1.0–4.3 m/s (m7f.1: 1.2–5.4), within the surge's range, and the
  roughness is lower in 24 of 30 entries. At 24 cells the peaks stay large and three got larger (bold: 14.9, 6.82 and
  8.64 against m7f.1's 5.66, 5.31 and 3.90), while 0.2/1 kg/m² at 1/30 fell from 7.98 and 6.87 to 3.84 and 5.01. The
  trace of the 14.9: the flap's free bottom edge (0.05 kg/m², 3.4e-5 kg per edge vertex, crumpled, normals with
  n_a ≈ 0.3–0.5) in the surge front, wet on one side (q ≈ 0.5–0.7) and dry on the other, received pressure impulses of
  about 5e-4 N s per step and moved at 10–17 m/s for two steps. The closed face's velocity constraint
  G_f V = (n_w·V) n_a lets the face's flux move the sheet at 1/n_a times the flux speed, the same staircase factor as
  the face mass in the projection's constraint; at 24 cells (4.2 cm, coarser than the 3.3 cm vertex spacing) one
  cell's flux lands on one edge vertex. Not fixed; at 40 cells and finer it does not appear in this sweep.
- **Physical or artefact.** The two mechanisms of §3.1 were artefacts (fixed). What remains in the surge: the fastest
  vertices are on the flap's free bottom edge, with pressure impulses from wet faces on both sides (q ≠ 0), in a
  surge of 2–3 m/s; a free edge of a 0.05–1 kg/m² cloth in that flow can move at a few times the surge speed. Inferred,
  not shown: there is no reference for a cloth edge in a dam break.

## 4. Did water pass through the cloth? A through-sheet measure (task 4)

### 4.1 Definition

An open sheet has no inside, so "the liquid on each side" is not defined. What is defined: liquid reaches a cell
through its faces. In each step (`CountSheetCrossings`), a cell that turned wet (φ ≥ 0 at the step's start, < 0
after the level-set stages, before the guard) is

- *fed* if a neighbour across an open face was wet at the step's start, or
- *swept* if a coupled sheet moved over its centre in the step (it changed sides with the sheet), or else
- *crossed* if a neighbour across a face that a coupled sheet closes (now or at the step's start) was wet: liquid that
  would have passed the sheet, or
- *unfed*: no wet neighbour at all (an artefact of the level set's corrections).

Crossed and unfed cells are then held by the guard (M7d: an air cell that turns wet without a wet open neighbour keeps
its value), so the measure counts attempts; none passes as a sign change. The run totals (cells, and the crossed
cells' sharp fill clamp(½ − φ/h, 0, 1)h³ summed over steps, so a cell held in several steps is counted in each) are
in the liquid's progress detail. Flux through a closed face is zero by construction (its fraction is 0 in the
projection); the faces of §3.2 that are opened lie between a free surface and the sheet, below the sheet.

What the measure does not see: values on the dry side that fall towards zero without changing sign (the smoothed
volume counts them); §1's artefact was of that kind.

### 4.2 The scenes (m7g.1, the committed scenes in a private cache directory)

| Scene | Steps | Crossed (held) | Unfed (held) | Rotation stretch reported |
|---|---|---|---|---|
| `liquid_cloth_catch` (64 cells, 1.6 s) | 779 | 459 cells (2.0e-4 m³ summed over steps, 4% of the block) | 21,280 | 5.2% (warning) |
| `liquid_cloth_wave` (64 cells, 2 s) | 980 | 0 | 3,888 | 62% (warning) |
| bag, 6 cm, 24 / 40 / 64 cells, 3 s (`--cup`) | | 112 / 781 / 2,885 cells (2.1%, 3.1%, 2.8% of V0 summed) | 0 / 0 / 14 | |

- **Catch.** No water passed the cloth as liquid: every attempt was held, and in the bags, where the space below is
  closed, the same order of attempts (2–3% of the volume summed over 3 s) left no wet cell below the sheet. The
  attempts are cells beside the cloth on its dry side whose level set the stencils pushed below zero for a step.
  The 21,280 unfed wettings are spray: liquid in flight whose level set reappears a cell away (the guard holds those
  too, which removes some spray; not measured further).
- **Wave.** The cloth is submerged on both sides wherever it is wet: nothing to cross.
- **The rotation estimate** warns in both scenes: 62% in the wave means an estimated ωH ≈ 1.1 rad per macro step, from
  edge vertices whipping in the surge (§3.3). The estimate is a largest edge turning rate, so one fast edge vertex
  sets it; the linear step's stretch is real only where that rate is sustained. Not investigated further.

## 5. Cloth coupling at 128 cells (task 5)

`liquid_cloth_catch` with the cloth refined to 33 × 33 vertices (1.25 cm, against 17 × 17), the liquid at 128 cells
along x (4.7 mm; 128 × 107 × 128), macro step 1/120 s, shortened to 0.8 s (25 frames), 16 threads, one process, with
other jobs running on the machine. The committed 64-cell scene with m7g.1 alongside:

| | 64 cells, 17 × 17, 1.6 s | 128 cells, 33 × 33, 0.8 s |
|---|---|---|
| Stability | stable, no clamped face | stable, no clamped face |
| Liquid steps | 779 | 923 |
| Wall time per liquid step (both members) | 0.09 s | 0.56 s |
| PCG iterations per step (mean) | 31.8 | 35.7 |
| Peak resident memory | 0.74 GB (the wave at 64; the catch not recorded) | 2.1 GB |
| Cloth | 779 Newton steps, direct solves | 1,069 Newton steps, 4,421 CG iterations (the iterative path at 1,089 vertices) |
| Through-sheet (§4) | 459 crossings held | 12,394 crossings held (6.5e-4 m³ summed over steps) |
| Rotation stretch estimate | 5.2% | 25% |

The whole run took 516 s. The cost per step grows 6× for 8× the cells; the PCG count barely grows (multigrid).

**Frames** (path traced, 640 × 400, `rendered/simulation_m7/catch_m7g_128000{0,1}.png`, t = 0.4 and 0.8 s), looked at:

- **t = 0.4.** The cloth has sprung back nearly flat; water runs over all four edges in thin sheets and broken
  streams, finer than at 64 cells. No step-like artefact on the cloth, no visible jitter.
- **t = 0.8.** The cloth is flat with a thin film; streams fall from its front edge to the floor, where a splash has
  spread. From this camera the streams start at the front rim; whether any comes from the underside cannot be told
  from this view. The cell-level measure says no crossing passed as a sign change; it does not see a hole in the
  barrier (a path of open faces through the sheet), which only a flood fill can show in a closed geometry (the bags
  of §1 show none at 24–64 cells). Not checked at 128 cells.
- The rotation estimate's warning (25%) says the cloth's linear step within a macro step is stretched by the fastest
  edges; a 1/240 s macro step was not tried.


## 6. Velocity at cut-cell solids (task 6): analysed, not changed

The face velocity is u = u* − (q_R − q_L): the pressure difference between two cell centres a full cell apart, for any
open fraction A. A's role is the face's weight in the divergence and in the kinetic energy (the variational form);
it does not enter the gradient. Beside a small cut cell the centre lies inside the solid or far from the liquid's
part of the cell, and the difference is a first-order approximation of the normal gradient at the face. M7a §13.4
measured exactly that pattern: the largest velocity errors on faces with open fractions down to 0.005, order about 1,
the pressure about 2.

- **Face-fraction definition.** Area (static solids) or control-volume (bodies) fractions change the weights only; no
  choice of weights changes the gradient's order. M7b's control-volume fractions fixed a staircase in the *force*,
  not the velocity order.
- **Boundary velocity extrapolation.** Already in place for transport (`min_transport_fraction` re-extrapolates faces
  below 0.25); it hides the worst faces from the advection, not from the measure.
- **A contained improvement** would need a second-order gradient at cut faces: a ghost value of q at the solid with a
  Neumann condition along the solid's normal (in the spirit of Gibou-type sharp methods), which makes the system
  non-symmetric or needs a different variational weight per face. That is a new pressure operator, with M7a's case 5s
  and M7b's added-mass and Archimedes tables to rerun (the validation module is another agent's). Not attempted in
  this session; no refinement table before/after. The references document notes that the rigid coupling inherits
  Robinson-Mosher et al.'s critique of Batty's volume-fraction impulse split when a dual cell mixes materials of very
  different density; no case in the suite shows it.

## 7. Rigid contact inside the liquid (task 7)

### 7.1 What happens today (read from the code)

- **Who integrates.** In an owned group (`owned <liquid> <rigid>`) the liquid integrates its own copy of each coupled
  body inside the macro step, the body's six velocities in the pressure solve. rigid.bodies integrates the body
  over the macro step with the liquid's mean force and torque (its impulse / H) and gravity, then applies its own
  plane contacts (sequential impulses, the configured restitution and friction) in each of its substeps. At the next
  macro start the liquid takes rigid.bodies' pose and velocity.
- **Who owns the contact impulse.** rigid.bodies. The liquid's copy is stopped at the domain walls after each liquid
  step (`LiquidSolver::WallContact`: restitution 0, friction 0.3, hard-coded), so it does not squeeze the liquid
  between body and wall; that correction is kinematic and enters no impulse account.
- **Does the pressure solve see the post-contact velocity in the same step?** No. The liquid's wall stop runs after
  the step's projections; the next liquid step's solve sees it. rigid.bodies' contact is seen at the next macro start.
  At rest each liquid step predicts w* = w − g dt into the floor and the projection resists it.
- **Two bodies colliding.** There is no body–body contact anywhere: rigid.bodies has planes only, and the liquid's
  geometry takes the smaller of the bodies' distances, so two submerged bodies pass into each other (the liquid between
  them is squeezed out by the pressure solve, which pushes them apart only as far as incompressibility does).

### 7.2 Measured: a dense sphere settling on the floor (`LiquidRigidTest --settle`)

Tank 0.32 m, water to 0.24 m, sphere R = 0.04 m (5 cells at 40 cells), density 2,500 (0.670 kg), released at rest at
y = 0.15 m, rigid.bodies with a floor plane (restitution 0, friction 0.3), owned exchange, macro 1/30 s, 2 s, 49 s.

| Quantity | Value |
|---|---|
| Landing | t ≈ 0.23 s, from about 0.7 m/s |
| Rest height y − R (last third) | +9e-18 m (range 0): exactly at contact, no sinking |
| rms vertical velocity at rest | 1.3e-18 m/s: no jitter |
| Liquid force at rest | 2.647 N against buoyancy ρVg = 2.630 N (+0.65%) |
| Floor force at rest (the momentum residual m Δv − H(F − mg) per H) | 3.928 N against (m − ρV)g = 3.945 N |
| Sum | 6.575 N = mg (closes to rounding by construction) |
| Impact macro step | liquid force −1.19 N (the decelerated liquid's momentum), floor impulse 0.87 N s |

With restitution 0.5 on the plane the sphere rebounds at 0.36 m/s and settles likewise (same rest values). The
liquid's copy still stops with restitution 0 inside that macro step: the two copies differ for one macro step and the
liquid takes rigid.bodies' state at the next start.

Reading: the rest state is clean. The +0.65% in the liquid force is either the 5-cell sphere's discrete buoyancy or the
resisted w* = −g dt at the floor; the two were not separated (a held sphere at the same place would separate them).

### 7.3 Defects stated, not fixed

- **No body–body contact** (in either solver). A clear gap, not a contained fix: it needs contact in rigid.bodies first.
- **The liquid's wall stop does not follow rigid.bodies' planes**: fixed restitution 0 and friction 0.3, and all
  domain walls whether or not rigid.bodies has a plane there. A body rigid.bodies lets through a wall is stopped by the
  liquid within each macro step. Passing the partner's contact settings through `body_mass_properties` would fix it;
  that is a contract change, not made here.

Rigid contact inside liquid stays `preview`.

## 8. 3-D momentum audit (task 8)

Not built (as M7f §10). The face-mass change of §3.2 does not change what the audit would sum (the momentum the face
liquid brings is unchanged); the piston's 2-D audit is unchanged (5.75e-4 and 1.46e-4 of p0 L t, test D).

## 9. Bakes without implicit sheets

The m7f.1 binary (built from the clean tree before any change) and the m7g.1 binary baked reduced copies of the
scenes, each in its own private directory; the content-addressed frame names were compared:

| Scene (reduced) | Members | |
|---|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | pool | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | pool, drop | identical (39) |
| `liquid_cloth_flap` (24 cells, 13 frames; staggered) | pool, flap | identical (27) |
| `liquid_honey_water` (24 cells, 7 frames) | honey | identical (7) |
| `cloth_flag_scene` (31 frames) | flag | identical (32) |
| `rigid_cloth_trampoline` (31 frames) | drop, sheet | identical (63) |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | pool: 12 of 13 frames differ; cloth: identical | changed, as intended |

Changed scenes: `liquid_cloth_wave` and `liquid_cloth_catch` (implicit sheets; all three fixes act there). Every
scene with an implicitly coupled sheet changes bytes; nothing else does.

## 10. Tests, pins, change log

- **LiquidClothCouplingTest** (all checks pass, 22.8 s): B flap max 0.097 m/s (submerged 0.076), roughness 0.032
  (m7f.1: 0.095, 0.076, 0.027); C 134.9 and 138.2 kg/m² (unchanged); D −4.4e-5 and −1.3e-5, audits 5.75e-4 and
  1.46e-4 (unchanged); E −5.54% (unchanged); F, G (twins, threads, checkpoint), H, I unchanged; J −0.29% (m7f.1
  −0.32%); K +0.033% (+0.03%); L unchanged. Gates unchanged.
- **Pins.** `liquid.mac` `m7f.1` → `m7g.1` in `LiquidRigidTest` and `LiquidSheetTest`. `cloth.shell` and
  `rigid.bodies` unchanged. Keyword count 191, `RewireConnectionTest` 6, the cst golden file and the schedule unchanged
  (no scene and no test added).
- **Contract.** Appendix C row (2026-11-26, M7g).
- **Test runs.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 452 built warning-free, 452 pass
  (with the other agents' uncommitted changes in the tree). Fire tests were not run.

## 11. Verified, inferred, preview, not run

- **Verified (measured).** §1's cause (stage, front speed, the constant-ghost test) and the bag tables; the two flap
  artefacts (the trace) and the sweep; the rigid-floor control and the sheet's stage sums of §2; the through-sheet
  counts; the settle case; byte identity without implicit sheets; thread and checkpoint invariance (test G).
- **Inferred.** That the wall instability needs a sheet to start (not measured without sheets); that the remaining
  dam-break speeds are physical; the source of the catch's held crossings (cells beside the cloth on its dry side).
- **Preview.** Sheets (thin layers on a sheet, the rotation estimate, no references for cloth in a surge); rigid
  contact inside liquid (no body–body contact, the liquid's own wall stop).
- **Not run / not done.** Task 6's improvement and table; task 8; the VS2022, Android and Xcode builds (no file added);
  the GUI; the 3 cm layer at 40 or 64 cells; the wall-ghost question without sheets; a rendered frame of the catch at 64
  cells with m7g.1.
