# M7f results: one-sided sheets hold their liquid (`liquid.mac` m7f.1)

2026-11-26. Follows [M7e](SIMULATION_M7E_RESULTS.md) (the implicit cloth coupling, whose open defect this was),
[M7d](SIMULATION_M7D_RESULTS.md) (sheets as face-crossing boundaries) and [M7_DESIGN](SIMULATION_M7_DESIGN.md) §2.
Contract: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), one Appendix C row (solver side; no member
added or changed).

**Status.** The leak is fixed for the cases that showed it, and it had two causes.

- **2-D (M7e's case (b)).** The pressure solve leaked, not the level set. The test strip's two vertex rows sagged
  unequally. The triangles' normals then had a component across the one-cell-thick slab, so the hydrostatic load
  pushed the strip across the slab. It left the cell-centre line, and no grid face was closed at all (§2.1). The
  fix: in a grid that is one cell thick along an axis, an implicit sheet's normal is projected onto the grid's plane.
  After it, the barrier never opened in 3 s at 24–128 cells. Case (b) at large sag (16.5 mm, about 3.5 cells at
  64 cells) is now within −0.30%, −0.16% and −0.77% of its reference at 32/64/128 cells (§4.1). It had failed at
  −92% and −95% with m7e.1.
- **3-D (a bag holding liquid).** The barrier never opened, but the level set moved liquid across the sheet. Its
  stencils read across the sheet, and the dry side's "false interface" was pinned only after the volume correction.
  The volume measure counted that interface's tails, and the correction drained the wet side to pay for them. A
  pinned cotton bag (24 cells, 3 cm of water) lost 22% of its water from the wet side in 2 s with m7e.1 (§2.2). The
  fix (§3): the sheet's faces are walls or boundaries of the level set's stencils. Where the sheet holds liquid on
  one side, the level set on both sides is the signed distance to it. That distance is set before the volume is
  measured and is left out of the correction. Cells the sheet uncovers are re-distanced from their own side, and
  marker particles stay two cells clear of the sheet.

**Measured loss through the sheet.** It is zero to the measure's resolution. A watertight bag (a sheet spanning
the tank, pinned to all four walls, 6 cm of water, air below, sag 19 mm with E = 2e4 N/m) was held 3 s at 24, 40 and
64 cells. No cell below the sheet beyond the cells beside it ever held liquid. No cell below it turned wet, apart
from transient sign changes in the cells beside the sheet at 40 cells (6e-4 of the volume, one macro step). The two
sides stayed separate components. The sharp liquid volume (tracked apart from the correction) changed by +0.08%,
+0.02% and −0.95% in 3 s, and the correction added +2.2%, +4.3% and +0.5% of the volume over the run. With cotton
(sag 40 mm), no cell below the sheet turned wet beyond 2e-5 of the volume and the barrier stayed closed. The sharp
volume changed by −2.5%, −3.3% and −2.3% at 24/40/64 cells, with the correction doing 2–16% of the volume's work
(§4.2).

**What still fails.** It holds where the liquid is resolved. A 3 cm layer at 24 cells (2.4 cells deep, with films of
one cell at the rim) still loses 8% of its sharp volume in 2 s at 24 cells, and without the correction its measured
volume grows (§4.2). A cotton bag with a 4 cm sag grows near-zero level-set values in the dry tank corners below the
sheet: none at 24 cells, 0.27% of the volume at 40 and 1.2% at 64 by 3 s. No cell turns wet, but the correction
counts them as liquid and takes that much from the free surface. The cause is not isolated (§4.2, §13). Sheets stay
`preview`.

The other tasks:

- **Unconnected steps are refused (task 2).** `cloth.shell` refuses an implicit sheet's load when `surface_load_steps`
  is not connected (§6).
- **Rotation within a macro step (task 3).** It was measured: a sheet turning at ω is stretched by (ωH)²/2 per macro
  step, which matches the derivation within 3%. That is 0.35% at 10 rad/s and 1/120 s, and 5.4% at 1/30 s. The
  liquid now reports its estimate in the progress detail and warns above 1% (§7).
- **Viscosity with sheets (task 4)** stays refused (§8).
- **Cut-cell velocity accuracy (task 5)** was not attempted (§9).
- **The 3-D momentum audit (task 6)** was not built (§10).

Bakes without implicit sheets publish frames byte-identical to m7e.1 (§11). `liquid.mac` is now `m7f.1`.

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidLevelSet.{h,cpp}` | `LevelSetBarrier` (wall and boundary faces, sided seven-point WENO samples, kept cells); the barrier in `OneSidedDerivatives`, `UpwindAdvectionRhs`, `AdvectLevelSet`, `Reinitialize`, `GradientDeviation`; `InterfaceArea` with an `exclude` mask. Default arguments: nothing changes without a barrier |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | `BuildBarrier`, `OwnInterface`, `PinSheetInterface` (replaces m7e's `PinFalseInterface`), `Redistance`; the step's order beside coupled sheets (pin, then the correction without the sheet's band); `SweepSheetCells` (constant new-side value, pin, re-distancing, the particle band); `stageHook` (diagnostics only, empty in the solver) |
| `src/Library/Simulation/Liquid/LiquidSheets.cpp` | Implicit sheets' normals projected onto a 2-D grid's plane |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | Version m7f.1; the rotation-stretch diagnostic (progress detail) and its note |
| `src/Library/Simulation/Cloth/ClothSolver.cpp` | The refusal of §6 |
| `tests/LiquidClothCouplingTest.cpp` | J (large sag), K (the bag), L (rotation), the refusal in I; probes `--cup`, `--rotate`, and `M7F_FLOOD` / `M7F_ZFIX` / `M7F_STAGES` / `M7F_SURFACE` / `M7F_DUMP` diagnostics in `--membrane mode model` and `--cup` |
| `tests/LiquidRigidTest.cpp`, `tests/LiquidSheetTest.cpp` | Version pins m7f.1 |
| `scenes/Tests/Simulation/liquid_cloth_catch.RISEscene`, `scripts/simulation_m7f_catch.sh` | Demo (§5) |

No new library source file: the build lists are unchanged.

## 2. Diagnosis

### 2.1 2-D: the strip left the slab (measured)

The model-level membrane (`--membrane mode model`, 64 cells, E = 2e4 N/m, 5 cm layer) was run with a flood fill after
every step. It marked the cells reachable from the top row through open faces whose centres lie below the membrane.
At t = 0.055 s, 554 such cells were reachable: the pressure solve itself was open. Per column, no y face was closed
from x = 0.08 to 0.22 m. The two vertex rows of the strip (one cell wide, at z = h and z = 0 in a slab one cell
thick) had moved in z by 2.2 mm (row 0 to z = 6.9 mm, row 1 to 2.3 mm). The slab's only cell-centre line lies at
z = h/2 = 2.34 mm, so the strip no longer covered it, and no crossing was found.

The cause was the load. The rows sagged by different amounts (0.5 mm apart over the 4.7 mm width, a twist of about
0.1 rad). The triangles' normals therefore had a z component, and a tenth of the 490 Pa hydrostatic load pushed the
2 Pa cloth across the slab.

- Resetting z at every macro step (`M7F_ZFIX`) removed most of the leak but not all, because the drift within a
  macro step still reached 2 mm.
- Projecting the normal onto the grid's plane removed it. The flood fill then never found more than a handful of
  cells over 3 s at 24/40/64/128 cells. Those are cells within a cell of the membrane: the test locates the membrane
  from one vertex row and the crossings from the triangles. The liquid below the membrane stayed at the
  false-interface half cell (1.2% at 64 cells, flat in time).

The physics behind the fix: a liquid with no gradient along an axis exerts no force along it. This 2-D artefact is
what M7e reported as the large-sag failure ("a column's crossing … once vanished for a step"). Its liquid-side
explanations (sweeps, jitter) were secondary.

### 2.2 3-D: the level set across the sheet (measured)

The bag of §4.2 with m7e.1's level-set treatment (cotton, 24 cells, 3 cm of water, 2 s): the barrier stayed closed
(the two components never met) and no cell below the sheet turned wet. Yet the sharp volume above fell by 22%. The
correction kept the smoothed volume at its target, and the "liquid below" of the clamp measure reached 8.5% of the
volume. That is the dry side's false-interface tail: dry cells beside the sheet with φ between 0 and h/2. The
correction counted them as liquid and took their volume from the free surface.

Each part of the fix was tried in turn. The stage-by-stage volume changes came from `M7F_STAGES`, the change of the
smoothed volume made by each stage of the step, summed over a run.

| Variant (24 cells, 3 cm cotton bag unless stated) | Result |
|---|---|
| m7e.1 (stencils across the sheet, dry side pinned after the correction) | above −22% in 2 s, below 8.5% (tails), cumulative correction removing |
| Sided stencils (constant extension), no pin (with a sided volume measure tried alongside) | above −40% in 2 s; the dry side kept the small values the sheet left (h/2 from the start), advection carried them into the dry region, the volume measure counted them |
| Sided stencils, both sides pinned after the correction | above −12% in 2 s: each step the correction shifted the pinned cells too and the pin undid it there, so the free surface kept paying |
| Pin before the correction, the pinned cells left out of it | above −9% in 2 s; without the correction the measured volume swung ±20% with the sag oscillation at 24 cells and drifted +12% in 1 s at 40 cells |
| The same with 6 cm of water (4.8 cells deep, E = 2e4) | smoothed volume −0.8% in 2 s without any correction, stable after the first 0.5 s |
| At rest (cotton, 6 cm, 2–3 s): stage sums | particles +12% of the volume per second (escaped particles beside the sheet), the pin +8%/s, the reinitialisation fighting it, the correction removing it from the free surface |
| Particles kept two cells clear of the sheet, reinitialisation keeping the pinned values, the sheet's band left out of the correction | at rest: the top's sharp volume constant (5.115 → 5.118 L over 1 s); the pin's and the particles' stage sums a third of the above or less |

Two further defects showed only on the flap of M7e §4 (both sides wet, a waterline):

- **Pinning at the waterline.** Cells at the waterline were pinned to the sheet's distance. There the free surface
  and the sheet cross the same segment, so the waterline moved to the sheet every step. The flap's speed in still
  water doubled (0.076 → 0.16 m/s). Cells with an interface of their own within two open steps are now left alone.
- **Constant extension at a one-sided sheet.** A constant extension in the WENO stencils biased the derivatives
  beside a sheet that is the liquid's boundary. The piston's acceleration came out 0.19% low, against 0.011% when
  reading across. Such faces are now boundary faces (§3), and the piston is at −4.4e-5.

And one on the flap's face mass:

- **The geometric fraction.** A geometric liquid fraction on the closed faces (wet centre to crossing) put liquid
  mass on faces at the waterline where the free surface lies below the crossing. It roughened the flap's load
  (0.42 against 0.21). The level-set fraction of m7e is kept; with the pin it equals the geometric one wherever the
  sheet holds liquid on one side.

## 3. Method (m7f.1)

For sheets coupled implicitly only; prescribed and staggered sheets keep m7d's stencils and guards.

1. **2-D grids.** In a grid one cell thick along an axis, the crossed triangle's unit normal is projected onto the
   grid's plane and renormalised. A triangle lying in that plane separates nothing and is skipped.
2. **Barrier.** Every face a coupled sheet closes is one of two kinds for the level set:
   - *Boundary.* The two cells differ in sign and neither has an interface of its own within two steps across open
     faces. The sheet is the liquid's boundary there, a zero (Dirichlet) condition. A stencil reads the cell across
     and continues linearly through it: the k-th sample beyond the last cell t on its side is
     f_t + k (f_across − f_t), which is the signed distance's own continuation. The reinitialisation keeps both cells'
     values.
   - *Wall.* Every other closed face: the same phase on both sides, or a free surface meeting the sheet. A stencil
     sees the last cell on its own side (constant extension, zero normal slope).

   Neither kind reads the other side's further values. This covers the seven-point WENO stencils of the advection
   and the reinitialisation, the gradient that triggers the reinitialisation (one-sided at a wall), and the near-cell
   detection of the non-Sussman schemes.
3. **Pin** (`PinSheetInterface`). Across each closed face whose cells differ in sign, a cell without an interface of
   its own takes σ |s − x_c| |n_a|: its distance to the crossed triangle's plane, negative on the wet side. A cell
   beside several such faces takes the least. In the step this runs after the reinitialisation and the particle
   corrections and before the volume is measured.
4. **Volume correction.** The pinned cells and their same-side face neighbours with no interface of their own (the
   sheet's band) are left out of the correction's area and its shift. The measure is the unchanged smoothed
   Heaviside: with the zero at the sheet, it counts the liquid up to the sheet.
5. **Sweeps.** A cell whose centre the sheet crossed takes the value of the cell beyond the old crossing on its new
   side (constant). The pin then runs at the new position. The cells beside the old crossings that are no longer
   beside the sheet, and swept cells not beside it, get the fast-marching update from their own side's neighbours:
   |φ| solves Σ_k max(0, |φ| − a_k)² = h², with a_k the least |φ| along axis k. A cell beside a free surface is left
   alone. Their values had been distances to where the sheet was.
6. **Particles.** Marker particles in the cells within two face steps of the cells beside a coupled sheet's faces,
   before or now, are removed when the faces change. Their trilinear corrections fought the boundary values.

**Derived here, and what is recalled.** The boundary/wall classification, the Dirichlet continuation, the pin's
order against the volume correction and its exclusion from it, and the 2-D projection are derived here from the
measurements above. Two parts follow published methods in form, as recalled:

- Treating the sheet as a visibility barrier (no stencil reads across it) and assigning swept cells from their new
  side is the treatment of Guendelman, Bridson and Fedkiw 2005, "Coupling water and smoke to thin deformable and
  rigid shells". As recalled, they ray-cast between a point and the grid nodes it reads, replace occluded values by
  visible ones, and take the values of cells a shell passes over from the side they end on. Here the axis-aligned
  lattice faces play the ray casts' part, because every stencil is axis-aligned.
- The coupled pressure system is Robinson-Mosher et al. 2008's form (M7e §2.2).

I did not have the papers in this session and did not check either against them. In particular, I do not recall
whether Guendelman et al. set a Dirichlet value at a one-sided shell or kept the level set two-valued there.

## 4. Acceptance

### 4.1 Case (b) at large sag (M7e §5.3)

The strip membrane across a 2-D tank: span 0.3 m, E = 2e4 N/m, prestretch 1.01, cotton density, rest height on a
face plane (0.046875 m), a 5 cm layer above, gravity. It runs through the group (cloth.shell + liquid.mac, owned
exchange, macro 1/120 s, 4 s). The measure is the mean mid-span sag over the second half. The reference is the cloth
alone under the analytic hydrostatic load, as in M7e: exact for the discrete cloth and the analytic load.

| | sag (reference 16.51 mm) | error | oscillation (±) |
|---|---|---|---|
| m7e.1, 32 cells (1 s) | 1.37 mm | −91.7% (the liquid passed the membrane) | |
| m7e.1, 64 cells (1 s) | 0.76 mm | −95.4% | |
| m7f.1, 32 cells | 16.46 mm | −0.30% | 0.7 mm |
| m7f.1, 64 cells | 16.49 mm | −0.16% | 1.8 mm |
| m7f.1, 128 cells | 16.38 mm | −0.77% | 3.2 mm |

The membrane is undamped (cloth damping off), and the second half of the run still holds an oscillation of about
30 rad/s. The mean over it carries that uncertainty, and the error does not converge with the cell size: it is not
an accuracy statement beyond "within about 1%". The fast test J gates 2% at 32 cells over 3 s (measured −0.32%).

### 4.2 A watertight bag (`--cup`)

A tank L × H × L = 0.3 × 0.3 × 0.3 m and a sheet spanning it at y0 ≈ 0.2 m (on a face plane), pinned along all four
walls (21 × 21 vertices, the rim pinned), cotton-plain-weave with its own density (0.15 kg/m²) and damping. Water
fills a layer d above the sheet and there is air below. The liquid can reach the space below only through the sheet.
The model-level rig re-linearises the cloth at every macro step (1/120 s), which is what the group's cloth integrates
(M7e §2.4).

Measures, apart from the correction:

- the components reachable from the top row and from the bottom row through open faces ("breached" if they meet);
- per component, the sharp volume Σ clamp(½ − φ/h, 0, 1) h³;
- below the sheet: the whole bottom component, which includes the dry cells beside the sheet. Their part-full count
  is the half cell between the centre and the sheet that the liquid above fills, so it is not leaked liquid. The
  leak measure is the part of the bottom component beyond the cells beside the sheet, plus the cells there with φ < 0;
- the volume the correction added, Σ −shift × area, an estimate.

| Case (3 s) | Sag | Below the sheet beyond its cells (max) | Wet cells below (max) | Sharp volume, end | Correction added | Barrier |
|---|---|---|---|---|---|---|
| E 2e4, 6 cm, 24 cells | 18.7 mm | 0 | 0 | +0.08% | +2.2% | intact |
| E 2e4, 6 cm, 40 cells | 18.8 mm | 0 | 6.3e-4 (cells beside the sheet, transient) | +0.02% | +4.3% | intact |
| E 2e4, 6 cm, 64 cells | 18.7 mm | 0 | 0 | −0.95% | +0.5% | intact |
| cotton, 6 cm, 24 cells | 39 mm | 0 | 0 | −2.5% | −16% | intact |
| cotton, 6 cm, 40 cells | 40 mm | 0.27% (a dry corner, from 2.4 s) | 7.8e-5 | −3.3% | −7.0% | intact |
| cotton, 6 cm, 64 cells | 40 mm | 1.2% (dry corners, from about 2 s) | 1.9e-5 | −2.3% | −2.3% | intact |
| cotton, 6 cm, 24 cells, no volume correction | 40 mm | 0.69% | 0 | +2.7% (smoothed +8.1%) | — | intact |
| cotton, 6 cm, 40 cells, no volume correction | 40 mm | 0.18% | 0 | −1.0% (smoothed +1.3%) | — | intact |
| cotton, 6 cm, 24 cells, macro 1/30 s | 40 mm | 0.16% | 0 | −3.8% | −19% | intact |
| cotton, 6 cm, 40 cells, macro 1/30 s | 40 mm | 0.94% | 0 | −3.8% | −11% | intact |
| cotton, **3 cm**, 24 cells, 2 s | 38 mm | 2.2% | 0 | −8.4% | −133% | intact |
| cotton, 3 cm, 24 cells, 2 s, no correction | 38 mm | 8.1% | 0 | +89% (smoothed +99%) | — | intact |

Reading:

- **Through the sheet.** Nothing passed in any case: the barrier stayed closed, and no liquid appeared below the
  sheet beyond the cells beside it, except the two cotton rows marked. The 3 cm row is the under-resolved case, and
  the "below" there is the level set's own corruption in the films at the rim, not flow through the sheet: no cell
  below turned wet.
- **The dry corners (cotton).** From about 2 s, the column of cells in a tank corner below the sheet, where the
  pinned rim meets two walls (at 40 cells x = 0.296, z = 0.004, from 4 to 20 cells below the sheet), holds values
  falling from 0.4h towards zero. No cell turns wet: m7d's guard, which keeps an air cell from turning liquid
  without a wet neighbour, holds them at their last value. It grows with resolution: 0 at 24 cells, 0.27% of the
  volume at 40 and 1.2% at 64 by 3 s. Inferred, not isolated: something in the dry region (the level set's linear
  extension where two domain walls meet below the sheet's rim, or the reinitialisation without an interface there)
  drives those cells towards zero and the guard stops them at the sign. It is not liquid passing the sheet (the
  barrier is closed and no cell between them and the sheet is wet), but the volume measure counts it, and the
  correction takes it from the free surface. One measurement points at the mechanism: at 24 cells without the
  correction (where it reaches 0.69%), it vanishes with the marker particles off and with the reinitialisation off.
  The interaction between the two in the dry region was not isolated. With the reinitialisation off, the measured
  volume also stops growing (−4% instead of +8% in 3 s), so the reinitialisation beside a sagging sheet is what
  creates volume there.
- **The sharp volume against the correction.** With the 19 mm sag (E 2e4), the sharp volume holds to 1% and the
  correction adds 0.5–4.3% of the volume over 3 s. The smoothed measure and the sharp one differ by that much at a
  moving free surface. With the 40 mm cotton sag, the correction removes 2–19% (−16% at 24 cells, −7% at 40, −2.3%
  at 64; −19% and −11% at 24 and 40 cells with 1/30 s macro steps). Without the correction the measured volume grows by 8% (24 cells)
  and 1.3% (40 cells) in 3 s. So the level set beside a strongly sagging and oscillating sheet still creates measured
  volume, shrinking with resolution, and the dry corners add to it at 64 cells.
- **The 3 cm layer** is 2.4 cells deep at 24 cells and one cell at the rim. The free surface and the sheet's
  boundary values then share the same cells, so cells switch between "own interface" and "boundary" as the free
  surface moves. Without the correction, the measured volume doubles in 2 s. This is a resolution limit of a level
  set holding two boundaries one or two cells apart. It is stated, not fixed. The fast test K covers the 6 cm case
  at 24 cells over 1 s.

### 4.3 The M7e stability sweep

The M7e §4 flap (0.36 × 0.45 m, 12 × 12 vertices, released 17° from vertical in still water, or hit by the dam
break), areal density overridden, liquid 8 threads, cloth 4, 2 s, with the m7f.1 binary. "Blew up" means a vertex
faster than 20 m/s or a failed step. Each entry is: result; max |v| m/s (in water, when different); roughness.

Still water:

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | stable; 0.093 (0.045); 0.011 | stable; 0.099 (0.049); 0.016 | stable; 0.10 (0.008); 0.012 | stable; 0.069 (0.028); 0.003 | stable; 0.086 (0.035); 0.001 | stable; 0.077 (0.011); 0.001 |
| 0.2 | stable; 0.095 (0.076); 0.015 | stable; 0.45 (0.081); 0.026 | stable; 0.098 (0.036); 0.019 | stable; 0.066 (0.044); 0.005 | stable; 1.15 (0.072); 0.047 | stable; 0.11 (0.036); 0.008 |
| 1 | stable; 0.095 (0.090); 0.023 | stable; 0.32 (0.093); 0.041 | stable; 0.096 (0.047); 0.019 | stable; 0.12; 0.013 | stable; 0.41 (0.11); 0.070 | stable; 0.093 (0.054); 0.019 |
| 10 | stable; 0.20; 0.051 | stable; 0.22; 0.073 | stable; 0.23; 0.055 | stable; 0.28; 0.071 | stable; 0.22; 0.13 | stable; 0.22; 0.046 |
| 100 | stable; 0.70; 0.27 | stable; 0.74; 0.30 | stable; 0.78; 0.26 | stable; 0.69; 0.26 | stable; 0.75; 0.33 | stable; 0.77; 0.25 |

In still water every combination is stable:

- **Speeds in water** are within 0.03 m/s of M7e's or lower.
- **Roughness** is lower than M7e's everywhere: 0.001–0.07 for the light flaps, against M7e's 0.004–0.53.
- **The heavy flap.** At 100 kg/m² it bounces on its own stretch (0.7–0.8 m/s, in the plane), as in M7e.
- **Above the water.** At 40 cells, with 0.2 and 1 kg/m², the part above the water whips faster than in M7e:
  0.32–1.15 m/s, against M7e's 0.09–0.50 for those four cases. That part is cloth in vacuum (no air), driven by its
  submerged part. Not investigated.

Dam break:

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | stable; 5.66; 1.58 | stable; 4.86; 1.09 | stable; 5.39 (2.2); 0.72 | stable; 5.31; 1.50 | stable; 4.38; 1.26 | stable; 3.27 (2.94); 0.61 |
| 0.2 | stable; 7.98; 1.72 | stable; 2.30 (1.71); 1.49 | stable; 3.78; 1.45 | stable; 3.90; 1.64 | stable; 2.49; 1.71 | stable; 3.13; 0.71 |
| 1 | stable; 6.87; 1.64 | stable; 2.88 (2.07); 1.03 | stable; 4.26; 0.73 | stable; 3.18; 1.88 | stable; 2.95; 1.47 | stable; 2.14; 0.60 |
| 10 | stable; 4.40; 1.72 | stable; 1.88; 0.62 | stable; 2.11; 0.59 | stable; 2.67; 1.33 | stable; 1.69; 2.04 | stable; 2.02; 0.59 |
| 100 | stable; 1.27; 0.89 | stable; 1.77; 0.84 | stable; 1.23; 0.94 | stable; 1.60; 0.85 | stable; 1.65; 0.56 | stable; 1.94; 0.78 |

Every combination is stable in both cases. In the dam break, the peaks at 24 cells (4–8 m/s) are as in M7e (3–9),
and at 40 and 64 cells they are within the surge speed's range (1.2–5.4 m/s against M7e's 1.2–4.2). Roughness near
1 is the surge's impulsive load, as in M7e.

## 5. Demo: a cloth catching a falling block of water

`scenes/Tests/Simulation/liquid_cloth_catch.RISEscene`, `scripts/simulation_m7f_catch.sh [resolution]`.

- **Setup.** A cotton square 0.4 × 0.4 m (17 × 17 vertices, 0.15 kg/m², `pin boundary`) is held flat 0.3 m above
  the floor of a tank 0.6 × 0.5 × 0.6 m. A block of water 0.2 × 0.12 × 0.2 m (4.8 L) is released 6 cm above its
  middle. `sheet cloth port coupling implicit`, five connections, `owned pool cloth`, macro step 1/120 s, 64 cells
  along x (0.94 cm).
- **Run.** The group bake took 87 s on 16 threads (other jobs running) for 49 frames. Both members label preview.
  Caches are under the default per-user scene-cache directory.
- **Frames.** Nine frames (t = 0, 0.2, …, 1.6 s) path traced at 640 × 400 with 16 samples, to
  `rendered/simulation_m7/catch_64*.png`.

What the frames show (looked at, all nine):

- **t = 0.** The block above the flat cloth.
- **t = 0.2.** The water has landed. The cloth hangs as a smooth bag a few centimetres deep, holding the water, whose
  surface is below the rim.
- **t = 0.4.** The cloth springs back. The water spreads to the rim and runs over it in thin sheets at the corners
  and sides.
- **t = 0.6.** The middle of the cloth is tented upward into a cone above the rim's plane, with the water around its
  base, and the first spilled water lies on the floor.
- **t = 0.8.** A narrow jet of water stands about 15 cm above the middle, rising from the pool on the cloth.
- **t = 1.0–1.6.** The cloth settles nearly flat, a thin pool on it. The water that went over the rim lies in
  puddles on the floor.

**Reading.**

- The cloth catches the water and holds it. A cotton trampoline then throws most of the 4.8 L over its edges.
- The frames show the water going over the rim at t = 0.4 s. No stream or drip falling from the underside of the
  cloth is visible in any frame. Whether any water passed through the cloth was not measured in this scene: the bags
  of §4.2 are where that is measured, and in the frames a small amount through the cloth could not be told from the
  spill.
- The upward cone at 0.6 s and the jet at 0.8 s are what a cavity collapsing on a stiff cloth does in kind, but
  their size has no reference.
- The water on the cloth at 1.6 s is about one cell deep, the regime §4.2 calls under-resolved.
- No step-to-step jitter is visible. Folds finer than 2.5 cm cannot appear (17 × 17 vertices).

## 6. Refusal: an implicit sheet's load without its steps (task 2)

The liquid cannot see whether its offer is connected; the cloth can. An implicitly coupled sheet offers its load on
`surface_load` with a `force` channel, which only implicit sheets send. When `cloth.shell` accepts such a load in a
macro step while its `surface_load_steps` port is unbound, it fails:

> surface_load carries an implicitly coupled sheet's force but surface_load_steps is not connected: connect the
> liquid's sheet load steps port (sheet_load_steps by default) to surface_load_steps …

- **When it fires.** The first macro step's load is offered at that step's end, so the refusal comes at the second
  macro step (t = 1/30 s in the test), before the mean-force exchange of M7e §2.1 can grow.
- **Tested.** LiquidClothCouplingTest I: the flap of B at 16 cells with the steps not connected is refused with that
  message.
- **Not refused.** An implicit sheet whose load port is not connected either, which is one-way use. The cloth then
  gets no force and the liquid's copy follows the cloth's own state.

## 7. Rotation within a macro step (task 3)

Within a macro step, the cloth's elasticity is the linear model about the step's start (M7e §2.4). A rigid rotation
then moves each vertex along its tangent. Derived here: a point at r turning at ω moves ωrH in a step of length H,
and ends at r√(1 + (ωH)²), a stretch of (ωH)²/2. Linear elasticity does not see that stretch as strain. At the next
re-linearisation the stretch is real, and the cloth springs back from it.

The measurement (`--rotate`) uses a free cotton square, 0.3 m, 13 × 13 vertices, no gravity or damping, spinning
rigidly. It is integrated with backward Euler in 4 steps per macro step, either with the coupled step's linear model
or with the cloth's own nonlinear model. The table gives the largest edge strain over 0.5 s.

| Axis | ω (rad/s) | H | ωH | Linear per macro step | Nonlinear | (ωH)²/2 |
|---|---|---|---|---|---|---|
| in-plane | 1 | 1/120 | 0.008 | 3.6e-5 | 9.1e-7 | 3.5e-5 |
| in-plane | 3 | 1/120 | 0.025 | 3.1e-4 | 8.2e-6 | 3.1e-4 |
| in-plane | 10 | 1/120 | 0.083 | 3.5e-3 | 9.1e-5 | 3.5e-3 |
| in-plane | 30 | 1/120 | 0.25 | 3.1e-2 | 8.1e-4 | 3.1e-2 |
| in-plane | 1 | 1/30 | 0.033 | 5.6e-4 | 8.5e-7 | 5.6e-4 |
| in-plane | 3 | 1/30 | 0.10 | 5.0e-3 | 7.6e-6 | 5.0e-3 |
| in-plane | 10 | 1/30 | 0.33 | 5.4e-2 | 8.4e-5 | 5.6e-2 |
| in-plane | 30 | 1/30 | 1.0 | 0.41 | 7.1e-4 | 0.50 |
| normal | (same to 3%) | | | | | |

The spurious stretch is (ωH)²/2 to 3% up to ωH = 0.33. At ordinary settings (a flapping cloth at a few rad/s,
1/120 s) it is 0.03–0.35%, 30–40 times the nonlinear model's strain but small in absolute terms. At 1/30 s and
10 rad/s it is 5.4%, which is significant. Cotton's own stretch under its weight is of order 0.1%.

The guard, chosen as the small and sound option:

- **The estimate.** At each macro step's start, the liquid estimates each implicit sheet's turning rate from the
  accepted vertex velocities: the largest rate at which a triangle edge turns, |Δv⊥|/|e|. It keeps the largest
  (ωH)²/2 over the run.
- **The report.** The progress detail reports that value ("sheet rotation stretch per macro step up to x%") and
  warns above 1% ("shorten the macro step"). The solver's notes state the rule.
- **Not chosen.** A corotational correction would need the cloth's linear model to carry per-element rotations
  through the ports, which is a cloth-side model change, so it was not built. A hard macro-step limit would refuse
  configurations whose rotation is unknown until they run.
- **Tested.** Test L checks the linear stretch against (ωH)²/2 within 5% at 10 rad/s and 1/30 s.

The diagnostic itself was only read in a run's progress output by hand, not tested.

## 8. Viscosity with sheets (task 4): still refused

It does not extend naturally, and it was not attempted. What it would take, from reading `LiquidViscosity`:

1. **Sheet faces in the variational viscous solve.** The solve's unknowns are face velocities with
   strain-rate-energy weights over cell and edge control volumes (Batty and Bridson 2008, M7c). A sheet cuts those
   control volumes at sub-cell positions, so each strain-rate sample whose stencil crosses a closed face would need
   a one-sided (ghost) velocity from the sheet's no-slip velocity at the crossing. That is a new cut-cell weighting
   for zero-thickness boundaries, comparable to M7c's solid weights.
2. **The sheet as an unknown.** The sheet's vertex velocities would have to be unknowns of the viscous solve, with
   S in the system, as M7c does for bodies. Otherwise the viscous traction is explicit and light cloth reproduces
   the added-mass instability in shear.
3. **Splitting.** The tangential traction returned to the cloth would come from the viscous solve and the normal one
   from the pressure solve, with M7c's incremental-pressure splitting checked again with a moving sheet.

A Couette check (a sheet sliding in its plane between walls) would validate 1 and 3. Each part is a component the
size of M7c's body coupling, so the refusal stays.

## 9. Velocity accuracy at cut-cell solids (task 5): not attempted

M7b measured first order at cut-cell solids. The candidates are:

- second-order variational weights (sub-cell control-volume fractions from the solid's distance rather than face
  areas);
- a boundary-aware velocity extrapolation into narrow cut faces.

Either changes the published bytes of every bake with solids or bodies. Either would need M7a's solid-boundary
convergence case and M7b's added-mass and Archimedes tables run again before and after. The leak took the session,
so neither was started, and no refinement table was produced.

## 10. Momentum audit in 3-D (task 6): not built

`AuditWalls` already tallies the walls' pressure impulse, and test D audits the 2-D piston. The 3-D audit would also
need two more terms, neither of which exists in a form a test can sum:

- the liquid on the closed faces (§2.3 of M7e), whose fraction now follows the pinned level set;
- the free-slip staircase's tangential impulse on the sheet's faces.

It was judged not cheap and not built. The piston's audit is now 5.8e-4 (σ = 0.2) and 1.5e-4 (σ = 50) of p0 L t,
against 4.7e-5 and 1.5e-5 in m7e.1, within the 5e-3 gate. The difference is from the pin at the macro boundary's
re-linearisation: the face liquid the test sums uses the level set, and the pin moves it. Without that pin it was
7.5e-5.

## 11. Without implicit sheets

The m7e.1 binary (built from the clean tree before any change) and the m7f.1 binary baked reduced copies of the
scenes in a private directory, each scene in its own directory. Every content-addressed frame and static file name
matches:

| Scene (reduced) | Members | |
|---|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | pool (13 frames) | identical |
| `liquid_rigid_bodies` (32 cells, 13 frames) | pool, drop | identical |
| `liquid_cloth_flap` (24 cells, 13 frames; staggered two-way) | pool, flap | identical |
| the same with the load not connected (one-way) | pool, flap | identical |
| `cloth_flag_scene` (31 frames) | flag | identical |
| `rigid_cloth_trampoline` (31 frames) | drop, sheet | identical |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | pool: 12 of 13 frames differ; cloth: identical | changed, as intended |

The wave's cloth frames are identical over these 13 frames (0.4 s). Inferred: the surge reaches the cloth only at
about 0.4 s (M7e §9). Not checked further. The full 2 s wave demo was not baked again.

## 12. Tests, pins, change log

- **LiquidClothCouplingTest.** It now takes about 24 s and adds:
  - J: case (b) at large sag, 32 cells, 3 s, within 2%; measured −0.32%.
  - K: the bag, 24 cells, 6 cm, E 2e4, 1 s: no liquid beyond the sheet's cells, no wet cell below, the barrier
    closed, the sharp volume within 1%; measured +0.03%.
  - L: rotation, as in §7.
  - I: the refusal of §6.

  Gates are about 1.25–6× the observed values.
- **Existing checks.**
  - B: flap max 0.095 m/s, in water 0.076, roughness 0.027; m7e.1 had 0.27, 0.076 and 0.21.
  - C: plate added mass 134.9 and 138.2 kg/m², unchanged.
  - D: piston −4.4e-5 and −1.3e-5.
  - E: membrane frequency −5.54%; m7e.1 had −5.2%.
  - F: unchanged.
  - G: twins, 1/3/8 liquid threads with 1/2/4 cloth threads, and a group checkpoint after three macro steps, all
    byte-identical.
  - H: unchanged.
- **Pins.**
  - `liquid.mac` `m7e.1` → `m7f.1`: `LiquidRigidTest`, `LiquidSheetTest`. `cloth.shell` stays `m3a.4` (a refusal
    only; no output byte or parameter change).
  - `tests/data/cst_derive_golden.txt`: one hand-added line for `liquid_cloth_catch.RISEscene` (2325 bytes, 39
    lines), from `--dump` with the `Job::` lines stripped. The method reproduces `liquid_cloth_wave`'s committed line.
  - `tests/test_schedule.tsv`: LiquidClothCouplingTest 12 → 24 s.
  - Unchanged: keyword count 191 (no chunk change), `RewireConnectionTest`, the build lists.
- **Contract.** One Appendix C row (2026-11-26, M7f, solver side).
- **Test runs.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 452 built warning-free, 452
  pass. That includes `CstDeriveGoldenTest`, with the new scene added to the index, and
  `--filter 'Liquid*,Cloth*,Simulation*'`. Fire tests were not run.

## 13. Verified, inferred, preview, open

- **Verified (measured).**
  - The 2-D leak's cause and its fix: the flood fill and the vertex rows' z.
  - No liquid passes a one-sided sheet in the bags of §4.2 or the membranes of §4.1 over 3–4 s at 24–128 cells.
  - Case (b) within about 1% of its reference.
  - The M7e stability sweep stable everywhere.
  - Byte identity without implicit sheets.
  - The rotation stretch law.
  - The refusal.
- **Inferred, not isolated.**
  - The dry-corner values at 40 cells (§4.2).
  - Why the wave's cloth frames did not change over 13 frames.
  - That the 3 cm layer's failure is resolution, from the 6 cm case working and the stage sums.
- **Preview.** Sheets stay preview. There are no evidence cases, no accuracy statement beyond the cases measured
  here, and the open items below.
- **Open.**
  - Layers of liquid on a sheet thinner than about four cells (§4.2).
  - The volume correction's cumulative work beside a sagging cotton bag (7–16% of the volume over 3 s at 24–40 cells).
  - The dry-corner values.
  - Prescribed and staggered sheets keep m7d's treatment (the barrier and the pin apply to implicit sheets only).
  - Surface tension's curvature still reads across a sheet (surface tension with sheets is not refused and was not
    tested).
  - Lines along which a crossing appears or vanishes (folds, edges) are not swept (m7e's rule).
  - Viscosity with sheets (§8), cut-cell accuracy (§9), the 3-D audit (§10).
  - The rotation diagnostic is a report, not a correction.
- **Not run.**
  - The VS2022, Android and Xcode builds (no source file added, so no list changed).
  - The GUI.
  - The full 2 s wave demo with m7f.1.
  - The bag at 64 cells with a cotton sag at macro 1/30 s.
  - 128 cells in 3-D.
- **Not verified.** Guendelman, Bridson and Fedkiw 2005 and Robinson-Mosher et al. 2008 themselves (§3).
