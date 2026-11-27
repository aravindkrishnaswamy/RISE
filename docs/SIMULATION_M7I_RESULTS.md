# M7i results: free-edge cover, consistent cloth copy, surface clear of sheets (`liquid.mac` m7i.1)

2026-11-27. Follows [M7h](SIMULATION_M7H_RESULTS.md), [M7g](SIMULATION_M7G_RESULTS.md), [M7f](SIMULATION_M7F_RESULTS.md),
[M7e](SIMULATION_M7E_RESULTS.md). Contract: no Appendix C row (no port or member added or changed; the changes are
behavioural, inside the two solvers).

**Status.** Two models changed and one output. Versions: `liquid.mac` `m7h.1` → `m7i.1`, `cloth.shell` `m3a.5` →
`m3a.6`; `rigid.bodies` unchanged (`m6.2`). Sheets stay `preview`.

| Task | Outcome |
|---|---|
| 1. Free-edge coverage | **Partly** (§1). Built as M7h §3 designed, plus the faces beside a free edge that the sheet does not cross. The worst row of the sweep, 0.05 kg/m² at 24 cells and 1/120 s, 13.6 → 4.86 m/s; the largest dam-break peak of the sweep 13.6 → 4.86; free-plate added mass 135 → 113 kg/m² (an equal-area disc in unbounded water: about 115); still water at 24 cells 0.167 → 0.076 m/s (1 kg/m²). Not met: "no case worse" (7 of 30 dam-break peaks rise by more than 10%, one still-water row 0.091 → 0.160), and the wave's indicator still warns (6.8% → 4.5%). Found on the way and fixed: the cloth and the liquid's copy of it drifted apart at light free edges (up to 7 m/s in one macro step at 64 cells, §1.4). 60/60 stable; operator SPD with free edges; D and F inside their gates (F 4.8e-5 → 2.8e-5); M7h's explanation of F's growth refuted (§1.6) |
| 2. Underside speckle | **Fixed** (§2): the extracted liquid surface lay below the zero-thickness cloth (a third of its vertices over a bag, by up to 0.045 h), not liquid below the sheet; the surface output now keeps 0.1 h clear of a coupled sheet on its wet side (0 vertices below). 128-cell catch re-rendered from below (§2.3) |
| 3. Thin layers on a sheet | **Not fixed** (§3): one more variant measured (no wet-side pin, the sheet a wall like a floor): no change. The loss is in the level set's advection itself near the sheet (−70% of V0 in 2 s without correction), not the pin, the particles or the guards. Supported minimum stated: 5 cells (contract scope, parser help) |
| 4. Viscosity with sheets; 3-D momentum audit | Not done |
| 5. Second-order cut-cell velocity | Not done (left, as instructed) |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `SheetCrossing::cover`, `crossed`, `Barrier()`; `SheetCover` (free edges, the patch's clipped projection, the faces beside an edge); `CrossesSheets` passes a face mostly open; `SheetCoupling` scales J by the cover |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Partly closed faces (`sheetClosed_`, `sheetWall_`, `SheetWall`): fraction A (1 − c), face mass × c, barrier/pin/creep count/`OwnInterface` only for c ≥ 1/2, a mostly open face keeps the projected velocity; `ClearSheets` (surface output); a `guards` stage hook (diagnostic) |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | Version m7i.1; the `sheet` help states the supported layer depth |
| `src/Library/Simulation/Cloth/ClothLinearAlgebra.cpp` | `CorotatedElements::Rotate` fits the rotations to binary32-rounded positions |
| `src/Library/Simulation/Cloth/ClothModel.{h,cpp}`, `ClothSolver.cpp` | Version m3a.6; `RoundState`; with a partner's steps the cloth continues from the binary32 state it offers |
| `tests/LiquidClothCouplingTest.cpp` | A: a free sheet (every edge free) in a 3-D tank, symmetric positive definite; F: the served elements' gradient residue; `--cup` with `M7I_SURFACE` (surface against the sheet); `--layer particles 0|1` |
| `tests/LiquidSheetTest.cpp`, `tests/LiquidRigidTest.cpp`, `tests/ClothSolverTest.cpp` | Version pins |
| `src/Library/Parsers/README.md`, `docs/SIMULATION_SOLVER_CONTRACT.md` | Notes (the layer depth) |

No source file, scene, test or chunk keyword added: build lists, keyword count (191), `RewireConnectionTest`, the cst
golden and the test schedule are unchanged.

## 1. Free-edge coverage (task 1)

### 1.1 Model

A face of a lattice line that a sheet's free edge reaches is split as a body's cut face is (M7b):

- **Free edge.** A boundary edge of an implicitly coupled sheet (one triangle) not pinned at both ends. Sheets pinned
  all round (the catch, the bags) have none and are not touched by this part.
- **Cover.** For a face of axis a whose square (h × h, at the face) a free edge's projection along a meets: c = the area
  of the sheet's projection inside the square over h², the triangles reached from the crossed one through shared
  vertices whose projection meets the square (Sutherland–Hodgman clipping), at most 1 (a fold), 1 above 1 − 1e-9.
  Interior faces keep c = 1 exactly (the bytes of a sheet without free edges are unchanged by this part).
- **Faces beside the edge.** Where a lattice line is not crossed by the sheet at all but its square is covered in part,
  the face holding the free edge's point nearest the line gets that cover (one face per line, the nearest edge point),
  with the edge point as its crossing (weights, coordinate, velocity) and the edge triangle's normal. Without these the
  load jumped from nothing to half a face as the edge passed a cell centre (measured: with crossed faces alone the
  0.2 kg/m² dam break at 1/120 s rose 5.29 → 8.29 m/s). A first version that gave every edge segment its own face on a
  line placed half-closed faces along vertical lines beside the flap's side edges, at heights where the sheet is not:
  still water at 24 cells went 0.08 → 0.36 m/s; one face per uncrossed line fixed it.
- **Projection.** Open fraction A (1 − c) (A the face's fraction before; a face a solid already cuts keeps m7h's full
  closing, and a beside-edge face there is dropped); the closed part c moves with the sheet: J and the face mass scale
  by c. The variational form stays: weights A(1 − c)θ for the open part, c θ A for the face liquid moving with the
  sheet; J^T S^-1 J stays positive semidefinite.
- **Topology.** The level set's barrier, the pin, the through-sheet count and `OwnInterface` treat a face as closed
  for c ≥ 1/2 only; marker particles pass a face with c < 1/2; such a face keeps the liquid velocity the projection
  gave it (the sheet's velocity is written only into barrier faces).

### 1.2 The coupling suite (`LiquidClothCouplingTest`, all checks pass)

| Check | m7h.1 | m7i.1 |
|---|---|---|
| A operator symmetry, Cholesky | 1.6e-16, succeeds | same; **new**: free sheet, 900 unknowns, 48 sheet faces (32 partly covered, 24 beside its edges): 1.4e-16, succeeds |
| B flap 0.2 kg/m², 24 cells, 1/30 s: max \|v\| (submerged); roughness | 0.0786 (0.0687); 0.0184 | 0.0781 (0.0234); 0.0175 |
| C plate added mass (σ 0.05, 10) | 134.9, 138.2 kg/m² | **112.3, 114.0** |
| D piston errors; momentum audits | −4.41e-5, −1.29e-5; 5.75e-4, 1.46e-4 | −3.35e-5, −1.30e-5; 5.75e-4, 1.45e-4 |
| E membrane frequency | −0.0554 | −0.0548 |
| F cloth momentum vs liquid impulses + weight (gate 1e-4) | 4.83e-5 | **2.79e-5** |
| G twins, threads, group checkpoint | identical | identical |
| J membrane at large sag | −0.00283 | −0.00347 |
| K bag | +3.30e-4 | same |
| L rotation, M watertightness | 8.55e-5; 0 holes / 63 | same |

C: the free plate's edges let the liquid round them within the edge's faces; for an equal-area disc in unbounded water
the added mass per area is (8/3)ρR³/(πR²) ≈ 0.849 ρ R = 115 kg/m² (R = 0.1354 m; the tank's walls, not accounted, raise
it): m7i is within a few per cent of it, m7h 18% above (a reference by formula, not a resolved case).

### 1.3 The 60-run sweep (M7e §4 flap, 2 s)

All 60 stable. Max |v| (m/s) and the largest edge strain; "rounding only" is the m7i binary with the cover switched off
(§1.4's change alone, a temporary switch, removed):

| Case (dam, cells, macro, kg/m²) | m7h.1 | rounding only | m7i.1 |
|---|---|---|---|
| still, 24, 1/120, 1 | 0.167; 0.0048 | 0.167; 0.0048 | **0.076**; 0.0040 |
| still, 24, 1/30, 10 | 0.234; 0.028 | 0.234; 0.028 | 0.181; 0.028 |
| still, 40, 1/120, 1 | 0.091; 0.0026 | 0.091; 0.0026 | **0.160**; 0.0061 |
| dam, 24, 1/120, 0.05 | **13.6**; 1.45 | 7.93; 0.26 | **4.86**; 0.19 |
| dam, 24, 1/120, 0.2 | 5.29; 0.27 | 7.73; 1.15 | 4.09; 0.24 |
| dam, 24, 1/120, 100 | 1.01; 0.26 | 1.01; 0.26 | 1.44; 0.27 |
| dam, 24, 1/30, 0.05 | 2.06; 0.15 | 2.56; 0.27 | **4.58**; 0.45 |
| dam, 24, 1/30, 0.2 | 3.94; 0.39 | 3.00; 0.36 | 4.23; 0.50 |
| dam, 24, 1/30, 10 | 3.91; 0.17 | 4.09; 0.24 | 1.88; 0.29 |
| dam, 40, 1/120, 0.05 | 2.57; 0.072 | 3.51; 0.053 | 4.11; 0.096 |
| dam, 40, 1/120, 0.2 | 3.10; 0.084 | 2.91; 0.077 | 2.03; 0.16 |
| dam, 40, 1/30, 0.2 | 1.77; 0.10 | 1.64; 0.081 | 2.28; 0.11 |
| dam, 64, 1/120, 0.2 | 1.99; 0.060 | 2.85; 0.069 | 2.46; 0.091 |
| dam, 64, 1/30, 0.05 | 2.74; 0.038 | 2.46; 0.087 | 2.32; 0.077 |
| dam, 64, 1/30, 1 | 1.36; 0.10 | 1.77; 0.10 | 1.76; 0.085 |

Summary over the 30 dam-break runs (ratio to m7h.1, geometric mean; more than 10% better / worse): rounding only
0.988, 6 / 7; m7i.1 0.977, 8 / 7; the largest peak 13.6 → 7.93 → 4.86. Still water: m7i.1 0.934, 4 better, 1 worse.

**Reading.** A single run's peak is not a stable statistic here: the rounding change alone (§1.4, a change at the
level of binary32 rounding) moved the dam-break peaks by −42% to +46%. Every peak above 3 m/s that was traced is a
light vertex at the edge in air: in the 24-cell, 1/30 s, 0.05 kg/m² run (4.58 m/s) the kicks are single liquid steps
in which a vertex near the top of a side edge goes 0.33 → 7.75 m/s with a liquid impulse of 1e-17 N s on it (no face it
loads has liquid): the cloth's own elastic response (the corotational linear model of a light edge snapping in vacuum,
no air), not the coupling. The free edge's full-face load that M7h located is gone (the 13.6 m/s run was that); what
remains is the flap's in-air whip, history-dependent.

### 1.4 The cloth and its copy drifted apart (found, fixed)

The liquid integrates a linearised copy of the cloth with the pressure; the cloth integrates its own steps driven by
the copy's per-step impulses (binary32). A diagnostic (temporary, removed) compared the cloth's served state at each
macro start with the copy's state at the end of the macro step:

| 64 cells, 1/30 s, 0.05 kg/m², dam | worst \|v_cloth − v_copy\| | first macro steps |
|---|---|---|
| m7h.1 (no cover) | 8.7e-3 m/s | — |
| cover, before the fix | **7.06 m/s** (vertex 137, t = 1.37 s) | 1.3e-4 m/s |
| m7i.1 | 0.67 m/s | 1.2e-6 m/s |

Cause (measured): a "shadow" of the cloth inside the liquid (the copy's own arithmetic, open loop, driven by the
impulses) from the copy's exact start state matched the copy to 1.2e-6 m/s with binary64 impulses and diverged to
2.7 m/s with binary32 ones. The rotations are fitted at each step to the predicted positions, so a difference between
two copies feeds back as an explicit geometric stiffness; the copy has the face liquid's mass on its vertices (closed
loop), the cloth only its own (60 times lighter at 64 cells): differences grew about fourfold per liquid step at a light
free edge. The cover makes the edge's face mass smaller, which is why it showed there. Fix (both solvers):

- the corotational rotations are fitted to positions rounded to binary32 (`CorotatedElements::Rotate`): differences
  below a binary32 step no longer reach the rotations (the shadow: 2.7 → 0.012 m/s);
- with a partner's steps the cloth continues from the binary32 state it offers (`RoundState` at the offer; at the
  first macro step after `BeginLinear`, its elements still from the state offered), so the cloth and the copy start
  each macro step from the same values (first-step difference 1.3e-4 → 1.2e-6 m/s).

What remains (0.67 m/s, rare) is the differences above a binary32 step that the exchange's rounding of mass, force and
impulse still makes; a complete fix would carry the face mass to the cloth or iterate the rotations within the step
(not done). Cost: two rounded copies of the positions per step.

### 1.5 The wave demo

`liquid_cloth_wave` (64 cells, committed, 61 frames): the indicator ("sheet rotation stretch per step (corotational)")
is **4.5%** (m7h 6.8%), still above its 1% warning; with the cover switched off it is 4.5% too: the drop is §1.4's, the
cover does not change the largest per-step rotation of the bottom rows in the surge. 108 s, 0.48 GB (final binary,
beside the test tier). Not met.

### 1.6 Test F

M7h attributed F's growth (1.75e-5 → 4.8e-5) to per-element rounding of the gradients. Measured now in F: the served
elements' gradients sum to 1.15e-7 of m g, 240 times less than F's error: that explanation is refuted. With §1.4's
fix F is 2.79e-5. Not isolated further (candidates: the element stiffness blocks' rounding, which breaks their
translation invariance, and the binary32 force and mass at the mechanics port).

## 2. Underside speckle (task 2)

### 2.1 What it is (measured)

`--cup cells 48 depth 0.06 E 2e4` with `M7I_SURFACE`: of the extracted surface's vertices within 2 h over the sheet,
by their distance along the nearest triangle's normal (pointing up):

| | t 0.25 | 0.5 | 0.75 | 1.0 s |
|---|---|---|---|---|
| m7h.1: vertices over the sheet; below it; deepest | 5387; 1736; −0.020 h | 5408; 1760; −0.019 h | 5405; 1776; −0.045 h | 5397; 1785; −0.015 h |
| m7i.1 | 5346; 0 | 5371; 0 | — | — |

No cell below the sheet is wet (the bag's own measure, and the flood fill of M7h). The surface's vertices on the
sample edges a sheet crosses lie at the sheet (the level set there is the distance to it) and its facets are chords
over a cell, while the cloth's facets are chords between its own vertices: the two surfaces cross each other by a few
hundredths of a cell, and from below the water surface shows through the zero-thickness cloth. So the speckle is the
extracted surface, not liquid.

### 2.2 Fix (`LiquidModel::ClearSheets`, output only)

Each surface vertex on a sample edge across a coupled sheet's barrier face is moved towards the wet sample until it
lies 0.1 h from the crossed triangle's plane (along the edge 0.1 h / |n_a|, at most 0.45 h, never past the wet
sample); the fans' centroids follow. The simulation is unchanged (the level set, the pressure); the surface frames of
scenes with implicit sheets change.

### 2.3 The 128-cell catch (as M7h §4.2: 33 × 33 cloth, 0.8 s, 1/120 s)

| | m7h.1 | m7i.1 |
|---|---|---|
| Liquid steps | 902 | 884 |
| Barrier holes (largest at any macro start) | 0 | 0 |
| Cells wet across the sheet, attempted and held (summed volume) | 10,080 (5.3e-4 m³) | 9,646 (5.0e-4 m³) |
| Wet without a wet neighbour (held) | 39,670 | 37,755 |
| Rotation indicator (per step) | 1.5% (warning) | 1.3% (warning) |
| Wall time, peak memory | 434 s, 2.3 GB | 412 s (beside the test tier), 2.5 GB |

Rendered from below (§5): the underside is a clean red bowl; the speckle of M7h's render
(`catch_m7h_128_below__denoised0000.png`) is gone. The render alone does not separate §2.2 from the other changes of
this milestone (no render was made with §2.2 switched off); §2.1's count does.

## 3. Thin layers on a sheet (task 3): not fixed

3 cm bag (2.4 cells at 24 cells, cotton, 1/120 s, 2 s, `--cup depth 0.03 cells 24 T 2`), sharp volume above the
sheet:

| | With correction | Without |
|---|---|---|
| m7h.1, m7i.1 | −8.37% (correction +27% of V0) | −41.4% |
| variant: no pin on the wet side, every coupled sheet face a wall (the floor's treatment) | −9.71% (+34%) | −41.2% |
| m7i.1 without marker particles | — | −41.4% |
| control: rigid floor, 2.4 cells, with / without particles | — | −2.16% / −2.16% |

Stage sums without correction (smoothed volume, M7F_STAGES, the advection now separated from the guards before it):
advection −0.70 V0, guards −0.0008, pin +6.39, reinitialisation −6.23, sweeps +0.22. With the wall variant: advection
−0.94, pin +10.1, reinitialisation −10.1, sweeps +0.69.

So the loss is not the pin's alternation (M7g §2.3, M7h §2: the pin and the reinitialisation trade 6 V0 a run but
nearly cancel), not the marker particles (their removal beside the sheet changes nothing), not the leak guards: it is
the level set's advection beside the sheet, about 0.35% of V0 per step, at bag speeds of 1 cm/s. The same depth on a
rigid floor loses 2%. A real fix needs the level set to carry two values in the cells a sheet cuts (one per side,
each advected with its own side's stencil and reconstructed from it) or a sub-cell position of the sheet in the
advection's stencils (a cut-cell level set); both touch the advection, the reinitialisation, the volume measure and the
sweeps. Not contained in this session.

**Supported minimum, stated plainly** (contract scope and the `sheet` help): liquid resting on an implicit sheet needs a
layer at least 5 cells deep (M7g measured the 6 cm bag, 4.8 cells, within 3% over 3 s; 2.4 cells loses 8% in 2 s with
the volume correction and 41% without).

## 4. Bytes: which reference scenes changed

The m7h.1 / m3a.5 binary (built from the tree before any change) and the final binary baked reduced copies, each in
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
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | **24 of 27 differ** (12 cloth, 12 pool) |
| `liquid_cloth_catch` (24 cells, 13 frames; implicit) | **24 of 27 differ** (12 cloth, 12 pool) |

Changed: exactly the scenes with implicitly coupled sheets (the catch, pinned all round, through §1.4 and §2; the wave
through all three).

## 5. Renders (looked at)

Path traced, 640 × 400, `rendered/simulation_m7/` (outside git), the final binaries:

- **Catch 128 cells from below, t = 0.8 s** (`m7i_catch_128_below2__denoised0000.png`, camera 0.3 0.2 1.0 at
  0.3 0.27 0.3, slightly below the cloth; and `m7i_catch_128_below__denoised0000.png`, camera 0.95 0.04 1.05 at
  0.3 0.25 0.3): the cloth's underside is uniformly red, no light fragments on it; streams leave over the rim on
  both sides and fall to a splash on the floor; nothing starts from the underside away from the rim.
- **Catch 128 cells, scene camera, t = 0.8 s** (`m7i_catch_128__denoised0000.png`): a thin film on the cloth, streams
  over its edges and corners to the floor, as in M7h.
- **Wave, t = 1.0 s** (`m7i_wave__denoised0000.png`): the surge has run up the far wall and is breaking back over the
  flap, which is bent along it; the water broken into sheets and drops around it.
- **Wave, t = 2.0 s** (`m7i_wave__denoised0001.png`): the flap hangs from its pinned top edge; its bottom edge at the
  water line is still ragged, a small fold of a few triangles (§1.5's indicator); the pool is settling.

## 6. Tests, pins

- **Pins.** `liquid.mac` m7h.1 → m7i.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` m3a.5 → m3a.6
  (`ClothSolverTest`); port counts unchanged.
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the fire agent's
  uncommitted changes in the tree): 452 built, 452 passed (253 s). The build (`all tests build-tool/rise_sim
  build-tool/simulation_cache_import`) is warning-free. Fire tests were not run.

## 7. Verified, inferred, not done

- **Verified (measured).** §1.2–§1.5's tables; the cloth/copy drift and its cause (the shadow with binary64 and
  binary32 impulses); §1.6's residue; §2.1's counts; §3's table and stage sums; the byte comparison.
- **Inferred.** That the remaining large peaks are the flap's in-air whip (traced in one run: the zero-impulse kicks;
  not in every run); the added-mass reference of §1.2 (a disc formula for a square plate, walls ignored); §3's
  diagnosis (where the loss is, measured; why the advection loses it there, not isolated).
- **Not done.** Tasks 4 and 5; a complete fix of the cloth/copy residue (face mass carried to the cloth); the VS2022,
  Android and Xcode builds (no file added); the GUI; fire tests.
