# M7k results: the waterline's contact made continuous, the rotation iteration built and measured (`liquid.mac` m7k.1)

2026-11-27. Follows [M7j](SIMULATION_M7J_RESULTS.md), [M7i](SIMULATION_M7I_RESULTS.md), [M7h](SIMULATION_M7H_RESULTS.md).
Contract: no Appendix C row (no port, member or parameter added or changed; the changes are behavioural, inside the
liquid solver).

**Status.** One model changed. Versions: `liquid.mac` `m7j.1` → `m7k.1`; `cloth.shell` unchanged (`m3a.7`; a cloth
change was built for the rotation iteration and removed with it, §1.3); `rigid.bodies` unchanged (`m6.2`). Sheets
stay `preview`.

| Task | Outcome |
|---|---|
| 1. Waterline kick | **Partly** (§1). Built: the contact of a sheet with a free surface below it fades over a quarter cell of the gap instead of switching (the face split as a free edge's partly covered face is), and the liquid's copy of the cloth takes the velocities the cloth's own step gives with the binary32 impulse. Light coarse flap (24 cells, 0.05 kg/m², dam break, 10 perturbed runs each): macro 1/30 s median 4.67 → **3.83** m/s, max 8.94 → **6.14**; macro 1/120 s median 4.87 → **4.34**, max 9.59 → **8.74**. Sweep 60/60 stable (largest dam-break peak 4.54 m/s). A, D, F hold (F 3.0e-9, gate 1e-6). The rotation iteration was built (bounded passes, SPD at every pass, the cloth repeating the same passes) and measured: it brings the wave demo's correction 14% → 0.2% and its indicator 6.4% → 0.28%, but it makes the light flap blow up (4 of 5, 3 of 10 and 1 of 10 runs in its three variants); not kept. **Not met:** the wave demo's warnings (final: indicator 6.9%, correction 39%, a single rare event, §1.5) |
| 2. Thin layers on curved sheets | **Not done** (§2): no time left after task 1; the 5-cell statement stays |
| 3. Viscosity with sheets; 3-D momentum audit | Not done |
| 4. Second-order cut-cell velocity | Not done |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | `OpenUnwetted`/`CloseUnwetted`: the contact's band (`kContactBand`, the faces' saved covers); the copy's velocities replayed after each step; the m7j correction moved into `LinearisationCorrection` (unchanged arithmetic, evaluated before the replay) |
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `SheetDynamics::Replay`; `SheetCoupling` skips only fully opened faces (`skip` 1; 2 is a face in the band) |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | version m7k.1 |
| `tests/LiquidClothCouplingTest.cpp` | `--probe … jitter k`: the dam's height raised by k × 0.1 mm (the ensembles) |
| `tests/LiquidRigidTest.cpp`, `tests/LiquidSheetTest.cpp` | version pins (`ClothSolverTest` unchanged: cloth.shell is m3a.7) |
| `docs/SIMULATION_SOLVER_CONTRACT.md` | §6 note on the layer depth points here too |

No source file, scene, test, chunk keyword or parameter added: build lists, keyword count, `RewireConnectionTest`, the
cst golden and the test schedule are unchanged. The `sheet` help and the parser README keep the 5-cell statement.

## 1. The waterline kick (task 1)

### 1.1 What the face liquid's changes are made of

A temporary per-step decomposition (removed) of every vertex's face liquid (the mass `SheetFaceMass` attaches,
Σ_f w_f c.w_m) between consecutive steps, summed as |Δ| over vertices and steps, in the 24-cell, 1/120 s, 0.05 kg/m²
dam break (m7j.1 arithmetic, 2 s):

| Part | Σ\|Δ\| (kg) | vertex-steps with \|Δ\| > 10 m_vertex |
|---|---|---|
| θ of a face present in both steps | 13.8 | 2,689 |
| the m7g contact switch (a face opened or closed for the step) | 8.0 | 673 |
| a face that appears or vanishes (the crossing moves to the next face of its lattice line, a free edge) | 43.5 | 2,419 |
| the crossing's weights (it moves within or between triangles) | 37.6 | 4,640 |

The wet/dry switch §2.4 of M7j pointed at is the smallest of the four. Most of the change is the lattice: a crossing on
a one-sided wet sheet carries θ = reach/h, which goes 1 → 0 when the sheet passes a cell centre and the closed face moves
to the next face, and the face's liquid sits on the three vertices of the crossed triangle, which a whipping light
sheet sweeps across in a few steps (at 24 cells a face, 4.2 cm, is wider than a cloth triangle). A blended θ (θ near 0
and 1 reflected towards 1/2 on faces whose two cells are both pinned) was tried: it applied in too few steps to change
any run (the ensemble's peaks equal to the digit in 7 of 10 runs); removed.

### 1.2 Built and kept

- **The contact fades.** A face a coupled sheet closes between a wet cell below and a dry cell above (m7g's candidate)
  whose free surface lies short of the crossing by the gap g = reach − θh: contact weight r = 1 for g ≤ 0.01 h (as
  m7g), r = 1 − (g − 0.01h)/(0.25h) in the band, 0 beyond (opened, as m7g). In the band the face is split as m7i's
  partly covered face is: the part r·c moves with the sheet (J and face mass scaled), A(1 − r·c) stays open with its
  ghost-fluid θ. `SheetCoupling` and `SheetFaceMass` skip only opened faces; the covers are restored after the step's
  projections. The operator is unchanged in form (a cut face), symmetric positive semidefinite (test A).
- **The copy follows the cloth.** The cloth takes each step's impulse in binary32 and solves its own step without the
  face liquid; the copy now replays that step (`SheetDynamics::Replay`: the same rotations, S V = rhs + I₃₂, pins
  keeping theirs) and keeps its velocities, so the two stay equal to the rounding of their own arithmetic. The
  measured drift at macro starts (cloth's served state against the copy's end state, a temporary trace) stayed at
  1e-6 m/s in the light flap's first 0.3 s, as before; the change matters when a step's impulse must carry large
  internal forces (§1.3). The step's diagnostics are taken before the replay (with the face liquid's operator).

### 1.3 The rotation iteration (built, measured, not kept)

Built as M7j §2.4 stated: after a step's projections, the correction (m7j's measure) at the step's end; above 0.2%,
the step's stage state (the predicted field, the pressure, the valid faces) restored and the projections repeated with
the rotations refitted at the step's end positions, at most 4 more passes; S + F stays SPD at any rotations (every pass
factorised). Pass logs (24 cells, 1/30 s, 0.05 kg/m²): it converges in most steps (40.1 → 0.146 → 5e-4 in two passes;
1.53 → 2.8e-4), diverges in others (1.81 → 0.32 → 0.86 → 0.58; 7.3 → 37.8 → 6.5 → 4.9).

**The cloth must take the same step.** The cloth fits its own rotations from its state (x + h v) and receives only the
impulse. Three ways of keeping the two in step were built:

| Scheme | Result |
|---|---|
| the impulse as liquid.mac computes it (iterated rotations), cloth unchanged | the cloth's step at the predicted rotations is another step: blew up |
| the impulse taken with the cloth's rotations (fitted at x, cloth changed to fit there), so the cloth's step reproduces the iterated velocities | exact in arithmetic, but the impulse then carries the change of the elastic force between the two rotations (0.035 N s on a 16.7 mg vertex); the binary32 rounding and the cloth's Newton tolerance (1e-6 m/s) are amplified about 40× per step through the rotations fitted from the states: 1.8e-6 → 7e-5 → 7.6e-3 → 0.44 → 25 m/s apart in one macro step |
| the cloth repeating its own step with rotations refitted at its end (the same rule, cloth.shell m3a.8 draft), the copy replaying the cloth's passes | in step (drift 8e-4 m/s), but see below |

With the third (all ensembles 5 runs × 2 macro steps unless stated; peak max \|v\| m/s, median / max, runs above
20 m/s):

| Variant | 1/30 s | 1/120 s |
|---|---|---|
| m7j.1 (10 runs) | 4.67 / 8.94, 0 | 4.87 / 9.59, 0 |
| iteration, best pass kept, cloth refitting | 36.1 / 64.6, **4** | 20.6 / 27.9, **3** |
| the same with the contact band | 20.8 / 281, **3** | 9.7 / 17.9, 0 |
| converged passes only (else the first), both sides, with the band (10 runs) | 5.17 / 23.8, **1** | 5.04 / 7.79, 0 |
| converged only in the liquid, best in the cloth | 7.2 / 691, **3** | 9.37 / 521, **1** |
| the cloth's refit alone (liquid one pass) | single runs 14.9 and 18.2 | |

On the wave demo (64 cells, 0.2 kg/m²) the iteration works: best pass kept, correction 14% → **0.2%**, indicator
6.4% → **0.28%** (90 s against 66 s); converged-only: 5.2% / 3.1%. A light coarse edge with face liquid a thousand
times its mass is where the Picard iteration on the rotations has no contraction, and its accepted passes (or the
differences between the liquid's pass and the cloth's) inject energy. Not kept; the cloth change went with it. What
would be needed: a step whose rotations are solved with the positions (Newton on the corotational energy, its
rotation derivatives included) in both solvers, or the liquid's step time cut where the correction is large.

### 1.4 The light coarse flap (acceptance)

24 cells, 0.05 kg/m², dam break, 2 s, the dam's height perturbed by 0, 0.1, …, 0.9 mm (`jitter 0..9`), old binary
(m7j.1 with the jitter option, built from the tree at the start) and new:

| Macro | m7j.1 median / max | m7k.1 median / max | runs |
|---|---|---|---|
| 1/30 s | 4.67 / 8.94 | **3.83 / 6.14** | [2.82 3.14 3.54 3.70 3.79 3.88 4.43 4.59 4.98 6.14] |
| 1/120 s | 4.87 / 9.59 | **4.34 / 8.74** | [3.38 3.64 3.72 3.97 4.22 4.47 4.95 6.40 7.44 8.74] |

Lower median and maximum at both macro steps; the spread stays wide (one run's peak is still not a stable statistic).

### 1.5 Sweep, suite, demos

- **The 60-run sweep** (M7e §4 flap, 0.05–100 kg/m², 24/40/64 cells, 1/30 and 1/120 s, still water and dam break,
  2 s): **60/60 stable**. Dam-break peaks 1.04–4.54 m/s (24 cells 1/30 s 0.05 kg/m²: 4.43, m7j.1 8.94); still water
  0.076–0.77 m/s (the 100 kg/m² rows the flap's own stretch, as before).
- **Coupling suite** (`LiquidClothCouplingTest`): all checks pass. A symmetric (1.6e-16, 1.4e-16), Cholesky succeeds;
  D −3.35e-5 / −1.30e-5, audit 5.75e-4 / 1.45e-4 (unchanged); **F 3.02e-9** of H m g (m7j.1 1.68e-9; gate 1e-6);
  J −0.00337 (−0.00344); K +3.30e-4; L, M unchanged.
- **Demos** (64 cells, final binary):

| Demo | m7j.1 indicator / correction | m7k.1 indicator / correction | wall, memory |
|---|---|---|---|
| `liquid_cloth_wave` | 6.4% / 14% (warnings) | 6.9% / **39%** (warnings) | 75 s, 0.44 GB |
| `liquid_cloth_catch` | 0.54% / 0.58% | 0.54% / 0.58% | 93 s, 0.47 GB |

The wave's reading is one rare step (M7j §2.1: three nearby binaries read 1.3%, 1.7% and 6.4%); it is not met. The
catch at 64 cells holds 393 crossings (m7j.1 401), 0 barrier holes. The catch at 128 cells: §5.

## 2. Thin layers on curved sheets (task 2): not done

No time was left after task 1 (the cloth/copy synchronisation of §1.3 took most of it). M7j §1.4's design stands; one
point learnt here bears on it: the stencils' ghost values across a sheet must be recomputed at every RK stage and
reinitialisation iteration from the current field (the barrier's samples are index pairs into the field today), so
the two-valued form needs a per-stage extension pass, not only a second array. The supported depth stays 5 cells;
the contract's §6 note and the `sheet` help are unchanged in substance.

## 3. Viscosity with sheets; 3-D momentum audit (task 3)

Not done. Viscosity with sheets stays refused.

## 4. Second-order cut-cell velocity (task 4)

Not done.

## 5. Bytes: which reference scenes changed

The m7j.1 binary (built from the tree at the start) and the final binary baked reduced copies (M7i's scripts), each in
its own cache directory; content-addressed frame names compared:

| Scene (reduced) | Result |
|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | identical (39) |
| `liquid_cloth_flap` (staggered) | identical (27) |
| `liquid_cloth_flap`, one-way | identical (27) |
| `liquid_honey_water` (24 cells, 7 frames) | identical (7) |
| `cloth_flag_scene` (31 frames) | identical (32) |
| `rigid_cloth_trampoline` (31 frames) | identical (63) |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | identical (27): no face of its in the contact's band, and the replay's rounding below the outputs' binary32 |
| `liquid_cloth_catch` (24 cells, 13 frames; implicit) | **19 of 27 differ** |
| `liquid_cloth_wave` (full, 64 cells, 61 frames) | **differs** |

Changed: only scenes with implicitly coupled sheets, where a face enters the contact's band (or the replay's rounding
reaches an output).

**Renders** (`rendered/simulation_m7/`, outside git; path tracing 640 × 400, denoised copies beside them):
`m7k_wave_0000.png`, `m7k_wave_0001.png` (the wave demo at 64 cells, t = 0.6 and 1.0 s; final binary);
`m7k_catch_128_0000.png` (the committed catch scene at 128 cells, 17 × 17 cloth, scene camera, t = 0.8 s) and
`m7k_catch_128_below_0000.png` (camera 0.95 0.04 1.05 looking at 0.3 0.25 0.3: from below the cloth). The 128-cell
catch bake (1.6 s): 0 barrier holes, 17,105 crossings held (0.89 litre), indicator 0.085%, correction 0.059%, 765 s,
3.5 GB peak. No m7j.1 bake of this configuration exists to compare the held count with (M7h's 10,080 was a 33 × 33
cloth over 0.8 s). From below, the water leaves the cloth in streams at its pinned rim and corners; no stream
through the cloth's interior is visible.

## 6. Tests, pins

- **Pins.** `liquid.mac` m7j.1 → m7k.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` m3a.7 (`ClothSolverTest`
  unchanged); port counts unchanged.
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the other agents'
  uncommitted changes in the tree): 452 built, **452 passed**. Fire tests were not run.
- **Build.** `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free (Homebrew linker
  notices only).

## 7. Verified, inferred, not done

- **Verified (measured).** §1.1's decomposition (one run); §1.2's drift reading; §1.3's pass logs, sync traces,
  ensembles and wave readings (intermediate binaries with temporary switches, removed); §1.4's ensembles (final
  binary and m7j.1 with the jitter option); §1.5; the byte comparison; the demos' readings.
- **Inferred.** §1.3's reading of why the iteration fails on light coarse edges (no contraction of the Picard map;
  measured divergent passes, the mechanism not isolated); the 40× per-step amplification attributed to rotations
  fitted from states (measured growth, the path not split); §1.3's "what would be needed".
- **Not done.** Tasks 2–4; the wave demo's warnings; the VS2022, Android and Xcode builds (no file added); the GUI;
  fire tests.
