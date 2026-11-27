# M7j results: thin layers located, the free edge's kicks traced, the impulse exchange exact (`liquid.mac` m7j.1)

2026-11-27. Follows [M7i](SIMULATION_M7I_RESULTS.md), [M7h](SIMULATION_M7H_RESULTS.md), [M7g](SIMULATION_M7G_RESULTS.md).
Contract: no Appendix C row (no port or member added or changed; the changes are behavioural, inside the two
solvers, and one more reading in the liquid's progress detail).

**Status.** Two models changed. Versions: `liquid.mac` `m7i.1` → `m7j.1`, `cloth.shell` `m3a.6` → `m3a.7`;
`rigid.bodies` unchanged (`m6.2`). Sheets stay `preview`.

| Task | Outcome |
|---|---|
| 1. Thin layers on a sheet | **Not fixed; located** (§1). Minimal tests built: a held sheet (flat, sagged, or a spherical bowl) with a sloshing layer. A flat held sheet loses what a rigid floor loses (2.4 cells: −0.7% against −2.3% in 1.5 s); a held spherical bowl with a 1.3-cell rim loses 44% in 2 s where the same bowl as a rigid solid (cut cells, or closed to a staircase) loses 1–2%. Ruled out by measurement: the velocity field (divergence 1e-10 of max\|u\| h), the sheet's velocity on its faces in either extrapolation, the wet-side pin (removed, or held through the advection), marker particles, films drying beside the sheet (0.3% of V0), the kinematics of the advection along a sloped staircase (a pool turning rigidly in the held bowl: +3.3% in 2 s), the projection's staircase. What is left is the level set's representation at a curved sheet: its ghost values across the sheet's wall faces decide the result (−42% to +13% for the cup by the choice alone), i.e. one value per cell where the sheet needs two. Target (2.4 cells within 2% without correction) not met; the stated minimum stays 5 cells |
| 2. Wave warning; free-edge kicks | **Cause found, not fixed** (§2). (a): the indicator reads real error. At the wave's peak (t = 1.052 s, the flap's bottom corner, 6.25e-5 kg) one more step with the rotations refitted would stretch an edge by 5.5%: the corotational step is inaccurate there, the estimate δ²/2 (1.3% in that run) understates it. cloth.shell alone in air does not show it (a flap thrown at 6 m/s: at most 0.09 m/s velocity change per step, nonlinear and corotational agree). The kicks are the coupling at the waterline: the face liquid on a light edge vertex jumps between steps by 10–1000 times the vertex's mass (0 → 3.6 g → 0 on a 34 mg vertex), and the vertex is dragged or released (4.6 m/s in one step with no liquid impulse, the step after its face liquid detached). All 7 traced runs: heavy or fine flaps (24 cells 100 kg/m², 64 cells) are liquid-driven with the linearisation accurate (correction ≤ 0.4%); light coarse flaps kick at the waterline with correction strains of 0.5 to 60. A new progress reading reports that correction |
| 3. Test F's residual | **Fixed** (§3): the binary32-rounded element blocks were not invariant to an element's translation, and the corotational d_e holds the element's rigid motion about the world origin, so a free sheet's elastic forces did not sum to zero. Balanced on both sides (`CorotatedElements::Balance`): F 2.79e-5 → **1.7e-9** of H m g (the gate tightened 1e-4 → 1e-6) |
| 4. Viscosity with sheets; 3-D momentum audit | Not done |
| 5. Second-order cut-cell velocity | Not done |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Cloth/ClothLinearAlgebra.{h,cpp}` | `CorotatedElements::Balance` (g_e less its mean, K_e ← P K_e P) |
| `src/Library/Simulation/Cloth/ClothModel.cpp`, `ClothSolver.cpp` | `BeginLinearAt` balances its elements; version m3a.7 |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | The accepted elements are balanced; version m7j.1; the progress detail's "linearisation correction per step" (warning above 1%) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}`, `LiquidSheets.h` | The correction (a diagnostic, after each coupled step); `Barrier()` (read access for diagnostics) |
| `tests/LiquidClothCouplingTest.cpp` | F's gate 1e-6; `--exchange`; `--cup` options `frozen`, `offset`, `freeze`, `u0`, `bowl`, `coupled`; `--spin`, `--air`; `--layer bowl`; stage traces (`M7J_SHARP`, `M7J_TRACE`, `M7J_COL`, `M7J_SLICE`, `M7J_VMAX`) |
| `tests/LiquidSheetTest.cpp`, `tests/LiquidRigidTest.cpp`, `tests/ClothSolverTest.cpp` | Version pins |
| `docs/SIMULATION_SOLVER_CONTRACT.md` | §6 note on the layer depth points here |

No source file, scene, test or chunk keyword added: build lists, keyword count, `RewireConnectionTest`, the cst
golden and the test schedule are unchanged. The `sheet` help and the parser README keep the 5-cell statement.

## 1. Thin layers on a sheet (task 1)

All volumes are the sharp count clamp(1/2 − φ/h, 0, 1) h³ over the cells on the liquid's side of the sheet (the
flood fill from the top of the tank). M7i's stage sums used the smoothed volume over every cell, which counted the
dry side's cells beside the sheet: the pin's +6.4 V0 and the reinitialisation's −6.2 V0 of M7i §3 are those cells
(on the liquid's side the two stages move −0.5% and −14%), not liquid.

### 1.1 Minimal tests

`LiquidClothCouplingTest --cup` (24 cells, 0.3 m tank, cotton, 1/120 s, no volume correction, 2 s unless stated):

| Case | Liquid side, final |
|---|---|
| M7i's bag (3 cm = 2.4 cells, sags 3 cm) | −41.4% |
| the same, held from 0.5 s (`freeze 0.5`; sagged, at rest, the liquid settling) | −38.3% (−28% after 0.5 s); the loss stops when the liquid's speed falls below 0.1 m/s (t ≈ 1.6 s) |
| held flat from the start, at rest (`frozen 1`), on the face plane or 0.3 or 0.5 cell above it | 0 (1e-11) |
| held flat, sloshing (`frozen 1 u0 0.2`), 2.4 cells / 1.3 cells, 1.5 s | **−0.7% / −12.6%** |
| a rigid floor, same (`--layer`), 2.4 / 1.3 cells, 1.5 s | −2.3% / −8.8% |
| held spherical bowl (`bowl 1`: sagitta 3 cm, rim at the tank's walls, level 1.6 cm above the rim), at rest | +0.6% |
| the same, sloshing (`u0 0.2`) | **−43.8%** |
| the same at 48 cells | −14.9% |
| the same bowl as a rigid solid (`--layer bowl 1`, cut cells), 1.5 s | **−1.2%** |
| the rigid bowl closed to a staircase (faces less than 99% open closed) | −2.1% |

A flat sheet is a rigid floor; a curved one is not. The loss needs the curve and motion, and converges at about
second order (44% → 15%). It is where the layer is thin and sloped (the advection's loss by ring from the bowl's
centre: rings 6–9 of 12 hold two thirds of it).

### 1.2 Ruled out (measured, each against the bowl or the bag)

| Candidate | Test | Result |
|---|---|---|
| The velocity field | discrete divergence of the step-start field in the wet cells | ≤ 1.8e-10 of max\|u\|/h, net flux 1e-13 m³/s |
| The sheet's velocity on its faces (cell velocity averages a 0 from the sheet) | the faces left unknown in the interface extrapolation (bag); in both extrapolations (bowl) | −41.7%; −44.2% |
| The wet-side pin (Dirichlet zero at the sheet) | no wet cell pinned, every face a wall; pinned cells not advected | −40.0% / −43.8%; −40.2% / −43.8% |
| Marker particles | off | −40.9% (bowl) |
| Films beside the sheet drying | sign changes of the cells beside the sheet in the advection | 549 dried, 401 wetted: −0.3% of V0 |
| The advection's kinematics along a sloped staircase | `--spin`: the bowl's pool (contact circle 0.137 m) turning rigidly at 2 rad/s, advection alone | +3.3% in 2 s (+0.3% with linear wall ghosts) |
| The projection's staircase | the rigid bowl as a staircase (above) | −2.1% |
| Reinitialisation alone | off (and particles off), bowl / rigid bowl, 1.5 s | −33% / −11% (the rigid bowl's reinitialisation restores it to −1.2%; the sheet's makes it −41%) |

### 1.3 Where it is

The level set's walls at the sheet decide the result. With the reinitialisation's ghost values across a sheet's wall
face changed from constant (m7f–m7i) to the linear extension of the liquid's side, the bag went −41% → −16% and the
bowl −44% → −18%; with the linear extension at the domain's walls too (the liquid touches the tank's walls), the bag
gained +13% (+16% with the sheet's walls left constant, the dry side below the sheet then taking 8.8% of V0, M7g's
false interface); a linear extension that may not change sign changed nothing (−42%). So the result swings by tens of per cent with the ghost values alone:
in a layer within a stencil's reach of the sheet on both sides, the single value per cell has no information that
says where the liquid ends at a curved sheet. The rigid bowl has it (the solid's cells hold the liquid's extension,
read by every stencil, and are not anyone's liquid); the sheet's other side holds its own values.

Tried and reverted: the advection's wall ghosts made linear (as at the domain's edge). It reduced the spin test's
error tenfold but left the bag at −40.5% and raised the 64-cell catch's held crossings from 449 to 3,303 (0.19 → 1.6
litres held at the barrier), so it was not kept.

### 1.4 What a fix needs (not contained)

Two level-set values in the cells a sheet's barrier faces bound (one per side), each advected and reinitialised
with its own side's stencils, a ghost extension of each side across the sheet (normal, multi-dimensional, as for
the rigid solids) for the other side's cells, the volume measure, the sweeps and the pin on the owning side's value,
and the surface extraction choosing by side. Touches `LevelSetBarrier`, `AdvectLevelSet`, `Reinitialize`,
`PinSheetInterface`, `SweepSheetCells`, `BlockSheetLeaks`, the volume and the output. Estimate: 3–4 days with the
M7e–M7i cases rerun. The bowl (`--cup bowl 1 u0 0.2`) and the rigid bowl are the acceptance pair.

### 1.5 Supported depth (unchanged: 5 cells)

The bag, 24 cells, 2 s, liquid side: 2.4 cells −41.4% without the correction (−8.4% with); 4.8 cells −5.2%
(−2.7%); 7.2 cells −0.1%. "Above + below" (the half-cells beside the sheet) with the correction: 4.8 cells +0.07%.
Not lowered.

## 2. The wave's indicator and the free edge's kicks (task 2)

### 2.1 The indicator reads real error

The indicator is δ²/2, δ the largest angle by which an element turned beyond its held rotation in a step. A direct
measure was added: at each coupled step's end the elastic gradient with the rotations refitted at the end positions
less the one with the held rotations is the force the held rotations left out; one more backward-Euler correction
moves the vertices by dx = −dt²(S + F)⁻¹(g_fit − g_held), and its largest edge stretch (|(dx_b − dx_a)·e|/|e|²) is
reported ("linearisation correction per step", warning above 1%).

A temporary per-kind split (one run) showed membranes carrying the largest δ (1.3% against hinges 0.6%, rods 0.4%), so the
reading is not a bending hinge's fold. At the wave's peak step in a run of an intermediate binary (t = 1.052 s, vertex 238, the flap's bottom corner,
6.25e-5 kg, at the waterline): δ = 0.16 rad (1.3%), correction 5.5%; the corner picked up face liquid in that step
(0 → 0.14 g) and a liquid impulse of 1.7e-4 N s took it from 0.14 to 1.78 m/s. In that run (an intermediate binary of this milestone) only 2
of about 980 liquid steps had a correction above 1%. So (a): real, rare error where a light corner is hit; δ²/2 understates it there.

| Demo (64 cells, committed) | m7i.1 indicator | m7j.1 indicator | m7j.1 correction | wall, memory |
|---|---|---|---|---|
| `liquid_cloth_wave` | 4.5% (warning) | 6.4% (warning) | 14% (warning) | 68 s, 0.54 GB |
| `liquid_cloth_catch` | 0.54% | 0.54% | 0.58% | 79 s, 0.51 GB |

The wave's peak moves with any change at rounding level (three nearby binaries of this milestone read 1.3%, 1.7%
and 6.4%): a statistic of one rare event.

### 2.2 cloth.shell alone in air does not show it

`--air`: the flap of `--probe` (0.36 × 0.45 m, 12 × 12, pinned top, 17°) thrown with a normal velocity V (s/L)²,
backward Euler at 1/1440 s, 0.5 s:

| Model | V | max \|v\| | largest one-step \|Δv\| | δ | edge strain |
|---|---|---|---|---|---|
| nonlinear, 0.05 kg/m² | 3 / 6 m/s | 3 / 6 | 0.026 / 0.091 m/s | — | 2.4e-4 / 5.7e-4 |
| corotational (macro 1/30 s) | 3 / 6 | 3 / 6 | 0.025 / 0.090 | 1.1e-4 / 5.4e-4 rad | 2.4e-4 / 5.6e-4 |
| corotational, 1/360 s steps | 6 | 6 | 0.30 | 7e-3 rad | 4.3e-4 |
| 0.2 kg/m², either | 3 | 3 | 0.029 | 1e-4 | 1.1e-3 |

A light edge whipping in air is smooth, and the corotational model equals the nonlinear one.

### 2.3 The kicks in the dam-break sweep (traced)

A temporary per-step trace (the vertex whose velocity changed most, its liquid impulse, its face liquid this step
and the step before, δ, the correction) on M7i's rows that rose by more than 10% and the still-water row. Peaks are
of the m7j binaries (they move with every change; final binary in the last column):

| Case (dam, cells, macro, kg/m²) | m7i.1 peak | kicks > 0.5 m/s per step | largest correction | nature | m7j.1 peak |
|---|---|---|---|---|---|
| dam, 24, 1/120, 100 | 1.44 | 4 | 0.2% | liquid impulse = m\|Δv\| | 1.44 |
| dam, 24, 1/30, 0.05 | 4.58 | 118 | 62 (6,200%) | waterline: face liquid 0 ↔ 3.6 g on 34–67 mg vertices; 34 kicks with no liquid impulse on the vertex | 8.94 |
| dam, 40, 1/120, 0.05 | 4.11 | 155 | 2.4 | same | 4.00 |
| dam, 40, 1/30, 0.2 | 2.28 | 86 | 0.45 | same, milder | 1.80 |
| dam, 64, 1/120, 0.2 | 2.46 | 55 | 0.4% | liquid-driven | 2.67 |
| dam, 64, 1/30, 1 | 1.76 | 35 | 0.3% | liquid-driven | 1.88 |
| still, 40, 1/120, 1 | 0.160 | 0 | — | no kick above 0.5 m/s | 0.160 |

(M7i's seventh row that rose by more than 10% is not in its printed table; not traced.) A typical sequence (24 cells,
1/30 s, 0.05 kg/m², vertex 137 on the bottom edge at the water line, 34 mg): t = 0.3556 s face liquid 3.6 g; 0.3611 s
0 g, no liquid impulse, 0.56 → 4.58 m/s, δ 0.53 rad, correction 55; 0.3667 s face liquid 0.5 g, 4.58 → 2.9 m/s.
The stiff membrane (wave speed √(E/σ) ≈ 300 m/s at 0.05 kg/m²) snaps a vertex its neighbours' face liquid dragged
when its own face liquid leaves; the next steps alternate. The still-water rise of M7i (0.091 → 0.160 m/s at 40 cells)
has no kick: the flap's steady response, not a whip.

Moving the rotations' fit point (x, x + dt v/2 instead of x + dt v, both sides) did not change the kicks (peaks 5.25,
6.14 m/s on the 24-cell row; reverted): the rotations' prediction is not the cause.

### 2.4 What a fix needs (not done)

The face liquid's mass on a vertex must change continuously as the waterline passes: a face's θ is clamped at
`thetaMin` and drops to 0 when its wet cell dries, and faces appear and vanish as the sheet's triangles cross lattice
lines; at 24 cells one face's liquid (ρh³ ≈ 2 g) is 60 times a 0.05 kg/m² vertex. Options: blend a face's mass in over
a band of φ (the face's wetness as a smooth function), or spread it over the triangle's area instead of the crossing's
weights; and iterate a step's rotations when its correction exceeds a bound (both solvers, from the same values).
Neither was built.

## 3. Test F (task 3): fixed

The temporary sum of the cloth's own elastic force over its free vertices in each step, summed per macro step,
matched F's residual step for step (−5.0e-8 vs −5.6e-8, −1.22e-8 vs −1.22e-8, +9.8e-8 vs +9.2e-8 N s); the liquid's
copy had different values of the same size. Mechanism: each element's g_e and K_e cross the port in binary32, after
which K_e·1 ≠ 0 at the rounding level; the corotational d_e = R_eᵀp_e − x0_e holds the element's rigid motion about
the world origin (millimetres), so R_e K_e d_e leaves a net force of ε32 |K_e| |d_e| per element (M7i's first candidate).

Fix (`CorotatedElements::Balance`, called by the cloth when it takes its linear model and by the liquid on the
elements it accepts, on the same binary32 values): g_a ← g_a − mean_b g_b, K ← P K P (P removes the element's mean
displacement; upper blocks computed, lower mirrored). Exact energies are unchanged (rigid translations are in K's null
space); the port carries the values before it (its bytes unchanged).

| | m7h.1 | m7i.1 | m7j.1 |
|---|---|---|---|
| F: \|Δp − (H m g + liquid impulses)\| / (H m g) | 4.83e-5 | 2.79e-5 | **1.68e-9** |

What is left is the binary32 rounding of the exchanged state. The gate is now 1e-6.

## 4. Viscosity with sheets; 3-D momentum audit (task 4)

Not done (time went to tasks 1–3). Viscosity with sheets stays refused.

## 5. Second-order cut-cell velocity (task 5)

Not done.

## 6. Bytes: which reference scenes changed

The m7i.1 / m3a.6 binary (built from the tree at the start) and the final binary baked reduced copies (M7i's
scripts), each in its own cache directory; content-addressed frame names compared:

| Scene (reduced) | Result |
|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | identical (39) |
| `liquid_cloth_flap` (staggered) | identical (27) |
| `liquid_cloth_flap`, one-way | identical (27) |
| `liquid_honey_water` (24 cells, 7 frames) | identical (7) |
| `cloth_flag_scene` (31 frames) | identical (32) |
| `rigid_cloth_trampoline` (31 frames) | identical (63) |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | **24 of 27 differ** (12 cloth, 12 pool) |
| `liquid_cloth_catch` (24 cells, 13 frames; implicit) | **24 of 27 differ** (12 cloth, 12 pool) |

Changed: exactly the scenes with implicitly coupled sheets (the balanced elements). The full catch (64 cells) holds
401 crossings (m7i.1 449), 0 barrier holes.

## 7. Tests, pins

- **Pins.** `liquid.mac` m7i.1 → m7j.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` m3a.6 → m3a.7
  (`ClothSolverTest`); port counts unchanged.
- **Coupling suite.** `LiquidClothCouplingTest`: all checks pass; A–E, G–M as in M7i (J −0.00344, K +3.30e-4); F
  1.68e-9.
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the other agents'
  uncommitted changes in the tree): 452 built, 451 passed (248 s); 1 failed: `CstDeriveGoldenTest`, one DRIFT on
  `scenes/Tests/Simulation/fire_campfire_scene.RISEscene`, a fire scene the fire agent has modified in the tree (not
  touched here). Fire tests were not run.
- **Build.** `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free.

## 8. Verified, inferred, not done

- **Verified (measured).** §1.1–1.3's tables; §1.5; §2.1's correction and the wave's peak trace; §2.2; §2.3's traces
  (with the m7j binaries as stated); §3's match and fix; the byte comparison; the demos' readings.
- **Inferred.** §1.3's reading that a single value per cell is what is missing (the ghost-value swing and the rigid
  bowl support it; a two-valued level set was not built); §1.4's and §2.4's designs and estimates; the stiff-membrane
  snap as the mechanism of the no-impulse kicks (the face-liquid jumps and the timing were measured, the force path
  inside the step was not split).
- **Not done.** Tasks 4 and 5; a fix of §1 and §2; M7i's unprinted seventh sweep row; the VS2022, Android and Xcode
  builds (no file added); the GUI; fire tests.
