# M7a results: the liquid solver (`liquid.mac`)

2026-11-25. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §1, §3, §4, §7 steps 1–3 and the
infrastructure of step 6. Prerequisites: [SIMULATION_M7_PREREQ_RESULTS.md](SIMULATION_M7_PREREQ_RESULTS.md). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (unchanged; no Appendix C entry needed beyond the
solver-side row below).

**Update 2026-11-25 (m7a.2, §13).** The velocity runaway in under-resolved films had two causes, both found and
fixed. WENO5 momentum stages created new velocity maxima in films one or two cells thick against a wall. The
velocities of narrow cut faces were transported as if the faces were fully open. Each momentum stage is now
bounded by the range of its upwind cell. Cut faces less than 1/4 open are kept in the pressure solve, but their
velocities are re-extrapolated before transport. No face is closed any more (`min_face_fraction` = 0). The
robustness sweep has no clamped face at any resolution, and a solid-boundary convergence case (5s) measures the
cut cells. The paragraphs of §2 and §10 that this pass superseded are marked; the tables in §2, §4, §5 and §6 are
current.

**Status.** `liquid.mac` is a single-phase, inviscid, free-surface liquid on a staggered grid. It runs on the
contract through both hosts. It bakes, stops and resumes byte-identically in `rise-sim`, with streamed checkpoints.
It publishes identical bytes for 1, 3 and 8 threads, and it earns `predictive` from evidence it computes at
Configure. The smooth canonical problems converge at second order: transport, standing waves at two depths,
sloshing, the far field of an early-time column collapse, and a Taylor–Green vortex. Hydrostatic rest is exact to
the solver tolerance, including a tilted surface and an immersed sphere.

These design decisions changed under measurement (§2, §13):

1. Reinitialisation runs only when the level set's gradient has drifted.
2. Momentum takes two stages with a projection each, and each stage is bounded by its upwind values (m7a.2).
3. The interface velocity uses a linearly extrapolated field. The momentum field is extrapolated by a constant.
4. φ is not extrapolated into solids.
5. Marker particles are on by default.
6. Cut faces less than 1/4 open are solved but not transported (m7a.2). m7a.1 closed faces less than half open.
7. A speed safeguard clamps runaway face velocities. Since m7a.2 it acts on no face in the robustness sweep and
   remains only as a counter (§13).

Large-deformation accuracy is first order in time and outside the label's scope. The dam break (case 8) is
reported against Martin & Moyce and is not gated. Rigid bodies, viscosity, surface tension and cloth are later
passes; the seam for rigid bodies is in place (§9).

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidGrid.{h,cpp}` | MAC grid layout, deterministic row-chunked loops and reductions, trilinear sampling, shapes and signed distances (analytic, triangle meshes), face-area fractions, initial liquid |
| `src/Library/Simulation/Liquid/LiquidLevelSet.{h,cpp}` | WENO5 one-sided derivatives, SSP-RK3 level-set advection, reinitialisation schemes, volume/area/correction, marker particles (particle level set) |
| `src/Library/Simulation/Liquid/LiquidFlow.{h,cpp}` | Velocity extrapolation, momentum advection (WENO5 / MacCormack / semi-Lagrangian), the pressure system (ghost fluid + cut cells), multigrid-preconditioned CG, the rigid-body seam `IPressureCoupling` |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | State, one step, the time-step limits, marching cubes, the outputs |
| `src/Library/Simulation/Liquid/LiquidValidation.{h,cpp}` | Canonical problems with their references (shared by the test and the evidence) |
| `src/Library/Simulation/Liquid/LiquidSolver.{h,cpp}` | `liquid.mac` on the contract: parameters, evidence, frames, streamed checkpoints, rollback |
| `src/Library/Simulation/Liquid/LiquidParameterSchema.{h,cpp}` | Schema and chunk translation (`liquid_simulation`) |
| `tests/LiquidValidationTest.cpp` | Validation table (fast tier about 40 s on 8 threads; `--extended` about 160 s) |
| `tests/LiquidSolverTest.cpp` | Contract conformance (about 28 s) |
| `scenes/Tests/Simulation/liquid_dam_break.RISEscene`, `scripts/simulation_m7_dam_break.sh` | Sample scene and demo |
| Build lists | All five: make `Filelist` + strict rules, Android `rise_sources.cmake` + strict properties, VS2022 vcxproj (Strict) + filters, the three Xcode Sources phases (`-fno-fast-math -ffp-contract=off`) and the Headers phase |
| `BuiltinSolvers.cpp`, `ChunkParserRegistry.cpp`, `Parsers/README.md` | Registration, chunk, documentation |

All liquid sources except the schema compile without fast-math and without FP contraction, as cloth's do. The
pool is cloth's `ParallelPool`, reused unchanged, so cloth is untouched. Loops run in chunks of whole rows whose
number depends only on the array sizes. Reductions combine chunk partials in chunk order. The results are
therefore bitwise independent of the thread count (`LiquidSolverTest` C).

## 2. The scheme as built, and every measured choice

**Grid.** The grid has nx × ny × nz cubic cells of side h. φ and the pressure live at cell centres. Velocity
component a lives on the faces normal to a. The solid signed distance lives at the cell corners (negative inside a
solid). y is up.

**Level set.** φ < 0 in the liquid. It solves φ_t + u·∇φ = 0. The space derivatives are fifth-order WENO
one-sided derivatives (Jiang & Peng 2000; the Hamilton–Jacobi form of Osher & Fedkiw §3.4, ε = 1e-6·max vᵢ² +
1e-99), upwinded by the sign of the cell-centred velocity. Time stepping is SSP-RK3 (Shu & Osher). Ghost values
beyond the walls are extrapolated linearly, which is exact for a planar φ.

**Reinitialisation (measured).** The PDE φ_τ = −S(φ₀)(|∇φ| − 1) is solved with S = φ₀/√(φ₀² + h²), Godunov's
Hamiltonian on the WENO5 derivatives, SSP-RK3 and dτ = 0.3h. Five near-interface treatments were implemented and
applied every step to resting shapes (the 1r rows: relative volume change after 1000 calls):

| Scheme | circle n = 50 / 100 | slotted disk n = 100 | distorted sphere 64³, drift / h (mean) |
|---|---|---|---|
| Sussman (no fix) | −2.4e-4 / −4.9e-6 | −5.9e-4 | 2.6e-2 |
| Russo–Smereka 2000 (max of one-sided and central) | +3.2e-2 / +2.5e-2 | +9.2e-3 | 4.0e-3 |
| same, central differences | +8.5e-3 / +1.2e-2 | +7.9e-3 | 4.0e-3 |
| interface as boundary, quadratic crossings (Chêne, Min & Gibou 2008 style) | −3.1e-2 / −8.7e-3 | −9.3e-3 | 7.7e-3 |
| frozen interface cells | 4e-8 / 1e-9 | 7e-7 | 5.8e-3 |

The subcell fixes move the zero contour of a one-off distorted level set least. Applied every step, their O(h²)
per-call bias accumulates: Russo–Smereka shrinks near-cell values call after call because its gradient estimate is
biased high, and the quadratic-crossing variant shrinks convex shapes. Sussman is nearly exact on a level set that
is already close to a distance function. A variant taking the one-sided difference toward the sign change (exact
for planes) drifted twice as much on spheres and was dropped.

Transport results with reinitialisation every step (L1 at 50/100/200 cells):

| Case | none | Sussman | Russo–Smereka | quadratic subcell | frozen |
|---|---|---|---|---|---|
| Zalesak | 1.10e-2, 1.30e-3, 3.6e-4 | 6.7e-3, 1.6e-3, 4.4e-4 | 1.4e-2, 8.3e-3, 5.7e-3 | 2.4e-2, 1.4e-2, 6.7e-3 | 6.4e-3, 1.5e-3, 4.1e-4 |
| vortex T = 2 | 1.7e-3, 1.1e-4, 1.6e-5 | 3.5e-3, 7.6e-4, 1.9e-4 | 4.0e-3, 1.6e-3, 1.5e-3 | 1.4e-2, 6.0e-3, 2.8e-3 | 9.1e-3, 4.1e-3, 2.0e-3 |

Reinitialising every step also cost the standing wave its second order. Sussman moves the interface by
O(h × the slope deviation) that advection creates each step, which is a first-order error. Without
reinitialisation the waves converge at 1.98/2.02/2.03 (finite depth) and 2.0/1.98/1.97 (deep). But the long
vortex (T = 8) needs it: without it the 200-cell run loses 15% of the area, against 8% with it.

**Built:** Sussman, one pseudo-time step, run only when max | |∇φ| − 1 | within 3 cells of the interface exceeds
`reinit_threshold` = 0.1.

- Small waves never trigger it, so they stay second order.
- Transport triggers it most steps, giving Sussman's accuracy.
- A threshold of 0.25 gave a vortex order of 1.5 at the finest level; 0.1 gives 1.85.

**Marker particles (measured; default on).** This is the particle level set of Enright, Fedkiw, Ferziger & Mitchell
2002.

- Seeding: 16 particles per cell within 3 cells, attracted to targets in [0.1h, 3h] on their own side, radii in
  [0.1h, 0.5h].
- Advection: RK3.
- Correction: escaped particles correct φ before and after reinitialisation through their spheres
  (φ⁺ = max, φ⁻ = min at the 8 surrounding centres, keeping the smaller magnitude).
- Housekeeping: radii are adjusted after each step, particles escaped by more than 1.5 r are deleted, and the band
  is reseeded every 20 steps.
- Determinism: random numbers come from a hash of (cell, step, index); ids are stable and increasing.

Measured benefit (L1, 50/100/200):

| Case | Level set only | With particles |
|---|---|---|
| Zalesak | 6.7e-3, 1.57e-3, 4.4e-4 | 3.4e-3, 7.3e-4, 2.3e-4 |
| Vortex T = 2 | 3.5e-3, 7.6e-4, 2.1e-4 | 2.5e-3, 5.6e-4, 2.0e-4 |
| Vortex T = 8 (100/200) | 3.7e-2, 6.1e-3 | 1.0e-2, 2.0e-3 (area loss 2.6% against 8%) |

- Cost at 128³ (dam break, 1.6M particles): +0.014 s per 0.25 s step (+5%) and +290 MB.
- Every other evidence case is bit-identical with and without particles, because no particle escapes in those
  smooth flows.
- The particles' observed transport order at the finest level drops to 1.5–1.7, because their correction is not a
  smooth error term. They are still better than the level set alone at every level tested.

Default: on. Publishing them (`publish_particles`) is opt-in: 1.6M particles are about 64 MB per frame.

**Volume.**

- Measure: V = Σ (1 − H_ε(φ)) · open · h³, with ε = 1.5h and `open` the cell's fraction outside solids.
- Each step φ += (V − V₀)/A, with A = Σ δ_ε(φ) |∇φ| open h³ (one Newton step).
- The shift is returned and reported (case 6). It is small and decreases under refinement: mean 1.2e-4 h, 5.8e-5 h
  and 3.4e-5 h at 32, 64 and 128 cells.

**Velocity extrapolation (measured).** Unknown faces (since m7a.2 also cut faces less than 1/4 open, §13) are filled layer by layer from known faces of the same
component (each layer reads only faces known before it). Two fields are built:

- for the interface (level set, particles): 4 layers, the first linear (2u₁ − u₂ along the axis when both are
  known);
- for momentum: 6 layers, constant.

Constant extrapolation everywhere made the surface velocity first order: the standing-wave frequency converged
only at order ≈ 1 (−4.1e-2, −1.9e-2, −6.4e-3, −3.5e-3 deep). Linear extrapolation in the momentum field let the
film ahead of a dam-break front accelerate without bound (10 m/s, then NaN at 32 cells). The split keeps both
right.

**φ is not extrapolated into solids (measured).** Extrapolating φ into solid cells perturbed a resting surface
near an immersed sphere. Neighbour averaging gave 4e-5·√(gh) spurious velocity, growing. Linear extrapolation made
it unstable. Without extrapolation the velocity is extrapolated into the solid's closed faces and carries φ there,
and hydrostatic rest is exact (1e-10).

**Momentum (measured).** Three advection schemes were compared on the Taylor–Green vortex (2T, closed box, rms face
error / max speed), each with one stage (advect u with itself, add gravity, project) and with two:

| Scheme | 1 stage, 16/32/64 | 2 stages, 16/32/64/128 | order (2 stages) |
|---|---|---|---|
| WENO5 (RK) | 1.5e-2, 1.1e-2, 6.6e-3 | 1.34e-3, 2.95e-4, 6.48e-5, 1.36e-5 | 2.19, 2.19, 2.25 |
| MacCormack + Selle limiter | 1.5e-2, 8.3e-3, 5.1e-3 | same as 1 stage | 0.7–0.8 |
| semi-Lagrangian (RK2 backtrace) | 6.9e-2, 3.8e-2, 2.0e-2 | same | 0.9 |

With one stage every scheme is first order in time. Advecting the projected field with itself leaves an
O(dt²) non-gradient remainder per step (the advection of the pressure-gradient part).

**Built: WENO5 with Heun stages**, each projected with the new interface:

- u₁ = P(B(uⁿ + dt L(uⁿ)) + dt g);
- uⁿ⁺¹ = P(uⁿ/2 + (B(u₁ + dt L(u₁)) + dt g)/2).

B (m7a.2, §13) limits each face value to the range of the stage's input over the corners of the face's upwind
cell, so an advection stage creates no new extremum. The numbers below are m7a.1's; m7a.2's are in §4 (Taylor–Green
+3% at 16 cells, orders unchanged).

The kinetic-energy loss over 2T is 2.1e-3, 2.3e-4, 2.8e-5, 3.5e-6 (third order). For the linearised free surface
the two stages equal one projection, so the wave coupling is unchanged. The step's pressure is q₁/2 + q₂. Cost:
two pressure solves per step; the second is warm-started.

**Interface–momentum coupling.** The interface is advanced with the step-start velocity, then the projection uses
the new interface. For the linearised problem this is symplectic Euler: no amplitude error and phase error
O((ω dt)²), with dt ∝ h. For nonlinear interface transport it is **first order in time**. This is not gated and
is stated in the scope.

**Pressure.** The unknown is q = p dt/(ρh) in liquid cells with an open face.

- Operator: (Aq)_c = Σ_f A_f (q_c − q_nb) + Σ_air A_f q_c/θ_f.
- Right-hand side: b_c = −Σ_f ±[A_f u*_f + (1 − A_f) u_s,f].
- This is the variational form of Batty, Bertails & Bridson 2007 with the face-area fractions of Ng, Min & Gibou
  2009. The fractions come from four triangles per face, each with the exact positive area of a linear
  interpolant of the corner distances.
- Free surface: ghost fluid (Gibou et al. 2002; Enright et al. 2003). θ = φ_L/(φ_L − φ_R), clamped to
  [1e-3, 1] (`theta_min`; the θ clamp had no effect on the robustness problem below).
- Open top: p = 0 on the boundary face, with θ = clamp(−φ/h, θ_min, 1/2).
- The prescribed solid velocity u_s enters b, which the moving bodies of later passes need. Static here: 0.
- Velocity update: u = u* − Δq on every open face next to a liquid cell. Across a Dirichlet face it is
  u* ± q/θ.
- Domain walls are free-slip (closed).

**Cut faces less than half open are closed (m7a.1; superseded in m7a.2, §13: no face is closed, and cut faces
less than 1/4 open are solved but not transported).** An under-resolved film
entering a narrow cut-cell slit is squeezed through faces 20–40% open and jets. The trigger is an obstacle face
0.2h inside a cell: the slit has a fully open inlet and its other faces are 20% open. A 20³ dam break against a box
diverged at a threshold of 0.1. At 64³ it diverged at 0.1 and at 0.3 (on 42%-open faces later). At 0.5 those runs
did not jet at the obstacle. The solid is then located to within half a cell (first order); the hydrostatic cases
stay exact.

**Speed safeguard (not part of the validated model; m7a.1 text, the cause is in §13).** Even at 0.5, the demo box
(with a ceiling) diverged to NaN at 48 cells. A sub-cell film trapped in the corner of the far wall and the ceiling amplified its face velocity by
about 10% per step. The root cause was not found in this pass. Two safeguards were added:

- face speeds above `speed_limit` × √(2|g|D) (3 by default) are clamped and counted (`StepStats::clamped`);
- a non-finite state fails the step ("the liquid simulation diverged …") instead of publishing NaN.

Robustness sweep (dam break against a box; 1 m cube 2.5 s, demo box 1.5 s; the clamp on at 3 √(2|g|D) as a
counter: 13.3 m/s in the cube, 9.4 m/s in the demo box):

| Run | m7a.1 (`min_face_fraction` 0.5): speed at the end; clamped faces (steps with a clamp / steps) | m7a.2 (no closing): largest valid speed over the run; clamped faces (steps) |
|---|---|---|
| cube 20³ | 2.2; 0 (0 / 413) | 4.9; 0 (0 / 384) |
| cube 32³ | 3.9; 3 (3 / 692) | 6.1; 0 (0 / 706) |
| cube 48³ | 5.6; 251 (86 / 1401) | 8.0; 0 (0 / 1192) |
| cube 64³ | 13.3 (at the limit); 1250 (481 / 2432) | 7.7; 0 (0 / 1635) |
| demo 48 × 24 × 24 | 2.4; 148 (92 / 570) | 5.7; 0 (0 / 434) |
| demo 96 × 48 × 48 | 4.7; 240 (120 / 1209) | 7.2; 0 (0 / 916) |

The m7a.1 column is the previous pass's table, reproduced exactly by this pass's driver. Its counts include
invalid (air) faces. m7a.2 clamps and counts valid faces only, because invalid faces are replaced by extrapolation
before any use. Counting every face, as m7a.1 did, the m7a.2 sweep has 64 face-steps above the limit, all in cube
64³ (largest 17.9 m/s) and all on invalid narrow cut faces (§13.3). Fewer steps at m7a.2 means smaller peak speeds:
the time step follows the largest valid speed.

**Solver.** Conjugate gradients, preconditioned by one V-cycle of a geometric multigrid:

- Coarsening: cells coarsen by 2 on every axis longer than one cell, down to ≤ 512 cells.
- Coarse operators are rediscretised: open fractions averaged; p = 0 at an inactive coarse neighbour across an
  open face (θ = 1); 2 × the top fraction on an open top.
- Prolongation is cell-centred trilinear; restriction is its transpose scaled by 2^−(d−2) for d coarsened axes.
- Smoother: red–black Gauss–Seidel, 2 sweeps before (red, black) and 2 after (black, red). The coarsest level gets
  30 + 30 sweeps in palindromic order.
- The preconditioner is symmetric (the adjoint smoother, R ∝ Pᵀ) and positive definite for any SPD coarse
  operator.
- Dot products sum fixed chunks in order. A colour's updates are independent, so the result does not depend on
  the thread count.
- Components with no Dirichlet condition (a sealed pocket, a closed full box) keep b and z at zero mean.
- Tolerance: ‖r‖∞ ≤ 1e-8 ‖b‖∞. Measured: 8–12 iterations per solve on 32² hydrostatic cases, 12–15 at 128³.
- Two smoothing sweeps beat one (+37% time) and three (equal).

**Time step.** Each segment of length T (an output interval or an imposed macro step) is split into
n = ceil(T / dt_max) equal steps, with

    dt_max = min(cfl · h / (|u|_max + |g| T),  wave_cfl · h / √(|g| D))

and cfl = wave_cfl = 0.5.

- The first limit is the advective CFL with the speed the liquid can gain over the segment.
- The second is a CFL on the fastest linear gravity wave: the shallow-water speed √(gD), with D the domain's extent
  along g, bounds √(g tanh(kd)/k) for every depth and wavenumber.
- dt therefore scales with h, and the symplectic coupling's O((ω dt)²) phase error is O(h²).
- Macro ends on the output grid give the same segments and the same steps (test E).

**Surface.** Marching cubes on φ with a consistent, crack-free triangulation:

- Each cube face's crossings are joined by segments. An ambiguous face is resolved by the asymptotic decider
  (Nielson & Hamann 1991), which depends only on the face's four values, so adjacent cubes agree.
- A cube's segments chain into closed loops, oriented by walking each face counter-clockwise from outside.
- A three-vertex loop becomes one triangle. A longer loop becomes a fan around its centroid. A fan from a loop
  vertex could put a chord on a cube face that the neighbouring cube also used: measured non-manifold edges on 92 of
  200 random fields before the centroid fan, none after.
- Vertices lie on the sample edges and are shared. Numbering is deterministic: edge vertices in (sample, axis)
  order, then centroids in cube order.

The field is padded with a ghost layer |φ| so the surface closes on the walls. Solids are carved half a cell
inside their surface (φ_render = max(φ, −(φ_s + h/2))), so the liquid meets an obstacle hidden behind its surface.

Normals are ∇φ by central differences, interpolated along the edge. Velocities are sampled from the MAC grid.

Tests: a sphere's area and volume converge at order 2.05/2.06, every surface is closed and outward, and 300 random
fields with ambiguous faces give no open edge.

## 3. Deviations from the design note

| Design | Built | Why |
|---|---|---|
| Reinitialisation every step (subcell-type fix implied) | Sussman, triggered by the gradient deviation | §2: fixes drift when repeated; every-step reinitialisation made waves first order |
| Momentum "limited high-order conservative, chosen by measurement" | WENO5 non-conservative with Heun stages, each bounded by its upwind values, and a projection per stage | §2: the only second-order candidate; conservative FV was not built; the bound since m7a.2 (§13) |
| Cut-cell solids (fractions) | As designed; faces < 25% open solved but not transported (m7a.1: closed < 50%) | Robustness (§13) |
| Rayleigh–Taylor (case 5) | Early-time column collapse | Two-phase RT is out of scope; §4 justifies the replacement |
| Marker particles "corrected by" | On by default after measurement | §2 |
| Outputs | As designed; `particles` opt-in | Frame size |

## 4. Validation

`bin/tests/LiquidValidationTest`: the fast tier is about 45 s on an 8-thread pool; `--extended` is about 160 s
(225 s measured under load) and adds the finer levels, the long vortex, the particle runs at 200 cells, sloshing at
40 cells, case 5s at 128 and the dam break at 64 cells. Tolerances are 1.25 × the error observed at that level with the default numerics, rounded up. Orders must
reach 1.8, or 1.7 where a sharp corner or a singularity lies in the measured region, as stated per row. All rows
pass.

| # | Case | Reference | Resolution | Error | Tolerance | Order | Result |
|---|---|---|---|---|---|---|---|
| 1 | Zalesak disk, 1 revolution, L1 (symmetric difference / perimeter) | exact shape | 50/100/200 (2-D: 1 cell thick in z, z differences vanish) | 6.70e-3, 1.57e-3, 4.44e-4 | 8.4e-3, 2.0e-3, 5.6e-4 | 2.09, 1.82 (≥ 1.7) | pass |
| 1 | same, with particles (default) | | 50/100/200 | 3.39e-3, 7.29e-4, 2.30e-4 | 4.3e-3, 9.2e-4, 2.9e-4 | 2.22, 1.67 (reported) | pass |
| 1 | single vortex, ψ = sin²(πx) sin²(πy) cos(πt/T)/π, u = −ψ_y, v = ψ_x (faces from ψ at corners: discretely divergence-free), T = 2, circle R 0.15 at (0.5, 0.75) | initial circle (flow reverses) | 50/100/200 | 3.54e-3, 7.60e-4, 2.11e-4 | 4.5e-3, 9.5e-4, 2.7e-4 | 2.22, 1.85 | pass |
| 1 | same, with particles | | 50/100/200 | 2.54e-3, 5.61e-4, 2.03e-4 | 3.2e-3, 7.1e-4, 2.6e-4 | 2.18, 1.47 (reported) | pass |
| 1 | area change, level set: Zalesak / vortex | 0 | 50/100/200 | 1.25e-1/4.4e-2, 3.2e-3/9.1e-3, 6.4e-4/2.2e-3 | 1.25 × | — | pass |
| 1 | vortex T = 8 (reported) | | 100/200 | 3.7e-2, 6.1e-3 (particles 1.0e-2, 2.0e-3) | — | — | report |
| 2 | hydrostatic rest: flat, tilted gravity (0.3 rad), tilted + immersed sphere, sphere 1.6 cells below the surface (2-D); tilted + sphere (3-D) | u = 0; p = ρ g depth | 32 (32³) | max \|u\|/√(gh) 4e-11 … 4.5e-10; pressure 4e-12 … 2e-11 of ρgH | 1e-7 (solver level) | exact | pass |
| 2m | Taylor–Green, WENO5, 2 stages | steady exact solution | 16/32/64/128 | 1.383e-3, 2.983e-4, 6.498e-5, 1.363e-5 | 1.8e-3, 3.8e-4, 8.2e-5, 1.8e-5 | 2.21, 2.20, 2.25 | pass |
| 3 | standing wave, kd = 3π/8, ka = 0.01, 3 periods, frequency | ω² = gk tanh(kd) | 16/32/64/128 per wavelength | −1.50e-2, −3.81e-3, −9.28e-4, −2.22e-4 | 1.9e-2, 4.8e-3, 1.2e-3, 2.8e-4 | 1.98, 2.04, 2.07 | pass |
| 3 | same, deep (kd = π) | same | 16/32/64/128 | −1.26e-2, −3.14e-3, −7.85e-4, −1.92e-4 | 1.6e-2, 4.0e-3, 9.9e-4, 2.5e-4 | 2.00, 2.00, 2.03 | pass |
| 3 | numerical decay γ/ω (finite / deep) | 0 (inviscid) | 16/32/64/128 | 5.0e-4, 3.4e-4, 2.8e-4, 1.9e-4 / 4.9e-4, 3.3e-4, 2.7e-4, 1.8e-4 | — | ≈ 0.5 (reported) | report |
| 4 | sloshing, tank 1 × 0.6 m, depth 0.5, modes (1,0) and (0,1) | ω² = gk tanh(kd), k₁₀ = π/a, k₀₁ = π/b | 20/40 across | (1,0) −1.85e-3, +4.62e-4; (0,1) −5.47e-3, −3.92e-4 | 2.4e-3, 5.8e-4; 6.9e-3, 5.0e-4 | 2.00; 3.80 | pass |
| 5 | early-time column collapse, H = 1, L = 1, interfaces off the grid: acceleration at least H/4 from the bottom corner | potential-flow series (below) | 16/32/64/128 per H | 4.86e-3, 1.25e-3, 3.56e-4, 9.43e-5 (/g) | 6.1e-3, 1.6e-3, 4.5e-4, 1.2e-4 | 1.96, 1.81, 1.91 (≥ 1.7) | pass |
| 5 | same, L2 over the column | same | 16/32/64/128 | 1.97e-3, 9.50e-4, 4.67e-4, 2.31e-4 | 1.25 × | 1.05, 1.03, 1.01 (≥ 0.9; log singularity) | pass |
| 5 | the series itself: a_y at the top dam corner | −g (free fall) | 2e5 terms | 1.6e-6 | 1e-4 | — | pass |
| 5s | early-time collapse against two cut-cell solid planes (wall and bed) rotated 0.35 rad, pivot (0.8123, 0.1789) off the grid: velocity, max at least H/4 from the dam corner (the max lies on faces beside the cut cells) | the series in the rotated frame | 16/32/64/128 per H | 2.92e-2, 1.38e-2, 8.28e-3, 5.06e-3 (/g) | 3.7e-2, 1.8e-2, 1.1e-2, 6.4e-3 | 1.08, 0.74, 0.71 (≥ 0.6; first order) | pass |
| 5s | same, pressure, max over the column's cells | same | 16/32/64/128 | 5.37e-4, 1.58e-4, 3.57e-5, 1.25e-5 (/ρgH) | 6.8e-4, 2.0e-4, 4.5e-5, 1.6e-5 | 1.76, 2.15, 1.51 (≥ 1.4) | pass |
| 6 | volume, ka = 0.1 standing wave, 10 periods: raw loss (correction off) | 0 | 32/64/128 | −9.25e-3, −6.96e-3, −1.55e-3 | report | 0.4, 2.2 (irregular) | report |
| 6 | mean / max volume-correction shift (correction on), residual drift | 0 | 32/64/128 | 1.16e-4/4.2e-4, 5.84e-5/5.1e-4, 3.42e-5/3.4e-4 h; drift ≤ 1e-8 | 1.5e-4, 7.4e-5, 4.3e-5; 1e-6 | 1.0, 0.8 | pass |
| 7 | observed orders | | | in the rows above | | | |
| 8 | dam break, square column, front Z vs T | Martin & Moyce 1952 table 1 | 32/64 per width | rms time difference 0.132 / 0.112 (aligned at Z = 1.44); front speed 1.56 / 1.48 √(ga) against M&M 1.39, Ritter 2 (m7a.1: 0.275 / 0.094, 1.63 / 1.51) | not gated | — | report |
| mc | marching cubes on a sphere; random fields | 4πR², 4/3πR³; closed | 16/32/64 | area 1.6e-2, 3.6e-3, 8.8e-4 | closed, outward | 2.05 / 2.06 | pass |

**Case 5 reference (derived here).** The column fills 0 ≤ x ≤ L, 0 ≤ y ≤ H, with the wall at x = 0 and the dam
removed at x = L. At the instant of release the velocity is zero, so the acceleration is a = g − ∇p/ρ with p
harmonic. The boundary conditions are:

- p = 0 on the top and on the dam face;
- ∂p/∂y = −ρg at the bottom (no normal acceleration);
- ∂p/∂x = 0 at the wall.

Write p = ρg(H − y) + q. Then q = Σ Aₙ cos(λₙy) cosh(λₙx)/cosh(λₙL), with λₙ = (2n + 1)π/(2H) and
Aₙ = −(2/H)∫₀ᴴ ρg(H − y) cos(λₙy) dy = −2ρg/(Hλₙ²). This gives

    a_x/g = (2/H) Σ cos(λₙy) sinh(λₙx) / (λₙ cosh(λₙL)),
    a_y/g = −(2/H) Σ sin(λₙy) cosh(λₙx) / (λₙ cosh(λₙL)).

Cross-checks:

- At the top dam corner a_y = −(4/π)(1 − 1/3 + …) g = −g (free fall), confirmed numerically to 1.6e-6.
- a_x = 0 on the wall and on the top surface.
- a_x diverges logarithmically at the bottom corner of the dam face. That is why the L2 norm converges at first
  order and the far field at second.

The numerical value is u/dt after one step from rest: the first projection of dt·g, independent of dt.

Why this case replaces Rayleigh–Taylor:

- It has a closed form.
- It exercises the free-surface Dirichlet condition with off-grid interfaces and a re-entrant corner.
- It is the initial stage of the large-deformation flow of case 8.
- The orifice (Torricelli) alternative needs a contraction coefficient from free-streamline theory and a
  well-resolved jet, and the Ritter limit is a shallow-water asymptote that real and inviscid fronts do not reach.

**Case 5s reference and design (m7a.2).** The column of case 5 (L = H = 1) is rotated by α = 0.35 rad about a
pivot P = (0.8123, 0.1789) in a 2.4 × 1.7 box. The frame is e₁ = (cos α, sin α), e₂ = (−sin α, cos α), with
x′ = (x − P)·e₁ and y′ = (x − P)·e₂. Two static solid planes through P bound the column: the wall (solid where
x′ < 0) and the bed (solid where y′ < 0). Gravity is −g e₂. The free surfaces are the top y′ = H and the dam face
x′ = L, off the grid. The level set is the distance to {x′ ≤ L, y′ ≤ H}, so the liquid continues into the
solids, as everywhere else in the solver. The boundary conditions are those of case 5 in the rotated frame, so the
reference is case 5's series in (x′, y′), with a = a_x′ e₁ + a_y′ e₂. Each component of its even extension is
smooth across both planes, so it serves as the reference at face centres that lie inside a solid. The pressure
reference is p = ρg(H − y′) + Σ Aₙ cos(λₙy′) cosh(λₙx′)/cosh(λₙL) (case 5's q).

The measured quantity is u/dt after one step from rest. It is compared at every face with an open part between
two liquid cells, at the face centre, and reported both overall and on the faces within 2h of a solid. The
pressure is compared at the centres of the liquid cells inside the column. Both exclude a radius of H/4 around
the dam corner's log singularity. The cut fractions change irregularly from level to level, because P is fixed in
space. A self-similar variant, with α = atan(1/2) and P on a grid node, gives the same picture: velocity orders
0.38, 1.14, 0.85 and pressure orders 3.1, 1.89, 1.70 (§13.4).

**Case 8 sources.** Martin & Moyce 1952 (Phil. Trans. R. Soc. A 244, 312–324), table 1, n² = 1, a = 2¼ in. The
values were transcribed from the JSTOR scan by a research agent; checked 2026-11-26 against the scan of the paper
(math.ntu.edu.tw mirror): all 19 means of the n² = 1, a = 2¼ in series agree (M7_REFERENCES §1).

- Scaling: Z = z/a and T = n t √(g/a). The authors normalise time to T = 0.80 at Z = 1.44 (they give no true zero),
  so the comparison aligns there.
- Lobovský et al. 2014 (J. Fluids Struct. 48, 407–434; arXiv 1308.0115) give front positions only as a plot (their
  Fig. 12). Their Table 1 gives average front speeds for t√(g/H) > 1: 1.56 and 1.34 for H = 300 and 600 mm, and
  their reduction of Martin & Moyce's 57 and 114 mm columns is 1.48 and 1.69. All are below Ritter's 2.
- **TODO (cannot be done from the paper):** Lobovský's front positions as numbers. The paper gives them only as its Fig. 12;
  the supplementary MATLAB figures at canal.etsin.upm.es/papers/lobovskyetaljfs2013/ answer 404 (2026-11-26). No digits
  were read off the figure. What the paper does give as numbers is carried instead (`LiquidValidation.h`, case 8 report):
  the average front speeds (1.56, 1.34) and the impact pressure peaks of the 100 runs per depth at four wall sensors
  (tables 2 and 4: median, 2.5 % and 97.5 % percentiles); the report runs the square column in their tank against
  the latter (M7_REFERENCES §1).

Our inviscid, free-slip front, 1.51–1.63 √(ga), lies between M&M and Ritter, as expected without bed friction.

## 5. Evidence and the label

Evidence is computed at Configure, as cloth does: a fast subset of the cases with the configured numerics. It is
memoised per process by the SHA-256 of the numerics, bound to the solver id, version and numerics through
`EvidenceDigest`, and takes about 3 s at the first Configure. The tolerances are 1.25 × the error observed with
the default numerics. The transport tolerances have a second value for particles on.

| Id | Check | Tolerance (particles on / off) | Default value |
|---|---|---|---|
| `transport-zalesak-n50` | case 1, L1 | 4.3e-3 / 8.4e-3 | 3.39e-3 |
| `transport-vortex-n50` | case 1, T = 2 | 3.2e-3 / 4.5e-3 | 2.54e-3 |
| `hydrostatic-n32`, `hydrostatic-pressure-n32` | case 2, tilted + sphere, 0.25 s | 1e-7, 1e-7 | 3.8e-10, 1.0e-11 |
| `wave-finite-n32`, `wave-deep-n32` | case 3 | 4.8e-3, 4.0e-3 | 3.81e-3, 3.14e-3 |
| `order-wave` | 16 → 32: max(0, 1.8 − order) | 0.2 | 0 |
| `slosh-n20` | case 4, max of the two modes | 6.9e-3 | 5.47e-3 |
| `collapse-n32`, `order-collapse` | case 5 far field; 16 → 32 order | 1.6e-3, 0.2 | 1.25e-3, 0 |
| `solid-collapse-n32`, `solid-pressure-n32` (m7a.2) | case 5s: velocity max / g, pressure max / (ρgH) | 1.8e-2, 2.0e-4 | 1.38e-2, 1.58e-4 |
| `taylor-green-n32`, `order-taylor-green` | momentum; 16 → 32 order | 3.8e-4, 0.2 | 2.98e-4, 0 |
| `volume-correction-n32` | case 6 mean shift / h | 1.5e-4 | 1.16e-4 |

Non-default numerics are evaluated on these same cases. For example `momentum semi_lagrangian` fails its order
case and publishes `preview` (`LiquidSolverTest` A).

**Conditions:**

- `interface` {level-set | particle-level-set}
- `momentum` {weno5 | …}
- `solids` {none | static}
- `top` {wall | open}
- `volume_correction` {on | off}

**Scope (one sentence in the description).** Single-phase inviscid incompressible liquid with a free surface
(p = 0, no surface tension), in a box with free-slip walls and static solids, on a uniform staggered grid. It is
certified on canonical problems and is second order on the smooth ones. It does not bound a given bake's error and
does not cover:

- spray, thin sheets, or any feature below about two cells;
- moving solids;
- viscosity;
- surface tension;
- the accuracy of large-deformation flows (first order in time).

The open top is covered only through hydrostatics (exact) and robustness. Static solids are covered through
hydrostatics (exact), case 5s and robustness. At the cut cells the pressure converges at about second order and
the velocity at about first order (§13.4).

## 6. Solver, parameters, outputs, ports

`liquid.mac` is version `m7a.2` (m7a.1 before §13). Parameters are a canonical CBOR/JSON map; unknown keys are refused, and every
value is written back. Notes on the table below:

- In `liquid` region items, `fill h` means everything below y = h.
- `solids` items are `{type: plane | sphere | box | cylinder, …}`, a `mesh` given inline (`vertices`,
  `triangles`), or a `mesh` from a `scene_input` (a mesh.v1/v2 sampled at t = 0). The scene input's identity is
  written back as `identity` and named in `inputs`. The mesh sign comes from ray parity along x, so the mesh must
  be closed.

| Key | Default | Meaning |
|---|---|---|
| `frames`, `frame_rate` | 121, 30 Hz | outputs at i / rate |
| `domain_min`, `domain_size` | (−0.5, 0, −0.25), (1, 0.5, 0.5) m | the box (rounded to whole cells) |
| `resolution` | 64 | cells along the longest side |
| `gravity`, `density`, `top` | (0, −9.81, 0), 1000 kg/m³, `wall` | |
| `initial_velocity` | 0 | |
| `liquid` | box (−0.5, 0, −0.25)–(−0.2, 0.3, 0.25) | regions: box, sphere, `fill h` |
| `solids` | none | see the notes above |
| `momentum`, `momentum_stages` | `weno5`, 2 | |
| `reinit`, `reinit_iterations`, `reinit_threshold` | `sussman`, 1, 0.1 | also `none`, `frozen`, `russo_smereka`, `russo_smereka_central`, `subcell` |
| `particles`, `particles_per_cell` | true, 16 | |
| `volume_correction` | true | |
| `cfl`, `wave_cfl` | 0.5, 0.5 | |
| `pressure_tolerance`, `theta_min` | 1e-8, 1e-3 | |
| `min_face_fraction` | 0 | cut faces less open are closed (0: none; m7a.1's default was 0.5) |
| `min_transport_fraction` | 0.25 | cut faces less open keep their velocity in the pressure solve but are re-extrapolated before transport (§13) |
| `speed_limit` | 3 | safeguard: valid face speeds above 3 √(2\|g\|D) are clamped and counted (0: off); not part of the validated model; reported in the progress detail |
| `extrapolation_layers`, `interface_extrapolation_layers`, `linear_extrapolation_layers` | 6, 4, 1 | |
| `publish_fields`, `publish_particles` | false, false | |
| `threads` | min(16, cores) | not keyed |

**Outputs.**

| Output | Codec | Contents |
|---|---|---|
| `surface` | mesh.v2 | inline indices, so a new topology every frame; normal (1), position (m), velocity (m/s) |
| `fields` | grid.v1 | `phi` (m), `pressure` (Pa: q ρh/dt of the step), `velocity` (m/s, cell-centred averages of the faces); one dense block per channel; index i at origin + (i + ½)h |
| `particles` | points.v1 | positions, velocities, radii, ids, `sign` attribute |

Output times do not depend on the step. Since m7a.2 every published velocity is sampled from the velocity with its
invalid faces extrapolated as for the interface (§13.2). m7a.1 sampled the raw face array, whose air faces held
advected, unprojected values.

**Port.** `fields` is offered as a cached-output port for one-way coupling: grid.v1; channels phi, pressure,
velocity; offered at every output time when `publish_fields` is set.

**Capabilities.**

- Backend: CPU, supported.
- `deterministic`, `resumable`, `externalMacroStep`, `rollback` and `threadCountInvariant` are all set.
- Modes: {bake}.
- `interactive` is set only up to 32³ cells. A step takes 23 ms at 32³ (§8), several times slower than real time;
  "interactive" means only that it is cheap enough to scrub live.
- `memoryEstimate` is 64 MiB + 450 B/cell + 9 B/cell per particle-per-cell + 60 B/cell with fields (measured
  356 B/cell at 128³, plus 25%).
- `costEstimate` is 1.2e-7 s per cell-step (measured, 16 threads) × the steps a second needs at the dam-break speed
  √(2gD) or the wave limit.

**Checkpoints.** Streamed (`CheckpointTo` / `RestoreFrom`); `Checkpoint` / `Restore` go through the same stream in
memory. The layout:

1. magic `RISELIQ1`;
2. u64 length and a canonical CBOR header (segment state, steps, last dt, target volume, grid size, particle count,
   next id);
3. φ and q (binary64);
4. the three face arrays;
5. the three validity masks;
6. the particles (x, y, z, r, sign as binary64; id as u64).

Everything is written in chunks of at most 8 MiB, so a 256³ state is never held twice. Restore checks the grid,
refuses truncation and trailing bytes, and rebuilds the static data (solid fractions) from the configuration.
`SaveState` / `RestoreState` copy the same state.

**Macro steps.** `ceil(len / dt_max)` equal steps per segment, as cloth. Macro ends on the output grid are
byte-equal to free running (test E). Half-interval macro ends run but are not byte-equal.

**Scene.** `liquid_simulation` is generated from the schema. Its chunk fields are `liquid` (box | sphere | fill),
`solid` (plane | sphere | box | cylinder) and `solid_object <object>` (ObjectReference). See
`Parsers/README.md` "Liquid (M7)" and the sample `scenes/Tests/Simulation/liquid_dam_break.RISEscene`: 48 cells,
a box obstacle drawn as `box_geometry`, the surface as `simulated_mesh_geometry source pool.surface` with a
dielectric of ior 1.33.

## 7. Tests (contract)

`LiquidSolverTest` (about 28 s; all pass):

- **A:** id, version, outputs, capabilities, estimates; the evidence table; effective parameters as a fixed point;
  `threads` unkeyed; semi-Lagrangian is preview; large grids are not interactive; refusals (since m7a.2 also
  `min_transport_fraction` 1.5); every schema default equals the solver's; every effective parameter is described;
  enum values configure; the chunk translation works.
- **B:** six frames with a box solid and a scene-input mesh solid (whose identity is keyed). The surface is closed
  and outward, and at t = 0 encloses the liquid's volume to within its edge rounding (−5.1% at 6 cells across;
  −1.3% and −0.34% at 2× and 4×: second order). Fields and particles decode, and velocities are bounded. Since
  m7a.2 the progress detail ends with "clamped faces 0 in 0 steps".
- **C:** 1, 3 and 8 threads, and twin runs, publish identical bytes (particles on).
- **D:** stopping mid-segment with in-memory and streamed checkpoints, with and without particles, equals the
  uninterrupted run; a truncated stream is refused.
- **E:** macro steps on the output grid equal free stepping byte for byte.
- **F:** SaveState / RestoreState repeat a macro step exactly; another instance's token is refused.
- **G:** `rise-sim` bake; bake with a stop after 3 frames, resume with a stop after 2, resume: the sealed cache is
  byte-identical to the uninterrupted bake (box solid, particles published). The in-process host publishes the
  same frames under the same key.

## 8. Performance (Apple M4 Max, 16 cores)

3-D dam break in a 1 m cube: column 0.3 × 0.6 × 1 m with a box obstacle, particles on, measured 0.15 s into the
run. The step costs below are from the scratch driver before the final change of `min_face_fraction` and the speed
safeguard (they affect only the obstacle's cut faces and a final pass over the faces). They were not re-measured
for m7a.2: its bound adds one pass of eight reads per face to each momentum stage, and its runs take fewer steps
(cube 64³: 1635 steps against 2432 for 2.5 s).

| Grid | Threads | s/step | Extrapolation | Level set | Particles | Momentum | Pressure (2 solves) | PCG iterations/step | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|
| 32³ | 16 | 0.023 | 0.003 | 0.003 | 0.001 | 0.006 | 0.010 | 20 | 46 MB |
| 64³ | 16 | 0.062 | 0.007 | 0.007 | 0.003 | 0.013 | 0.033 | 24 | 241 MB |
| 64³ | 4 | 0.062 | 0.008 | 0.010 | — | 0.019 | 0.025 | 24 | 135 MB |
| 128³ | 16 | 0.266 | 0.028 | 0.031 | 0.014 | 0.057 | 0.136 | 28.5 | 1.04 GB |
| 128³ | 1 (no particles) | 1.56 | 0.24 | 0.26 | — | 0.48 | 0.58 | 30 | 0.74 GB |

At 128³ one pressure solve is assembly 3 ms, component check 4 ms, hierarchy 8 ms, PCG 57 ms (about 4 ms per
iteration) and update 1 ms. The surface mesh takes 5 ms (160k triangles).

Changes made by profiling:

- Restriction weights are tabulated per axis, and levels below 32k cells run on the caller.
- The PCG vector operations and the preconditioner's copies touch only each row's active span. With 20% of the
  cells liquid, they had moved about 250 MB per iteration.
- The extrapolation bands are 6 and 4 layers (8 before).
- Effect at 128³: 0.338 → 0.253 s/step without particles.

Scaling is poor above 4 threads at 64³ (equal times on 4 and 16) and 5.9× on 16 at 128³: the passes are memory
bound and the joins short.

**Estimated 5 s dam break at 128³:**

- dt ≈ 1.15–1.25 ms (wave and advective limits), so about 4,200 steps.
- 4,200 × 0.27 s ≈ 19 min on this machine (the capability's `costEstimate` predicts 24 min); about 1 GB RSS.
- The demo's 48 × 24 × 24 bake (1.5 s, 46 frames) took 17.5 s wall with the final defaults (11.8 s earlier),
  including about 3 s of evidence.

`--extended` 256³ was not run: by the per-cell cost about 2.2 s/step and 8 GB, so a 5 s bake would take about
5 h.

## 9. The seam for rigid bodies

`IPressureCoupling` in `LiquidFlow.h` takes `ApplyAdd(q, out)` (out += J M⁻¹ Jᵀ q), `AddRhs(b)` and
`Finish(q)`. `Project` already:

- applies every coupling after the grid stencil in the PCG operator;
- lets each add to the right-hand side;
- excludes coupled systems from the null-space projection;
- calls `Finish` with the solution.

The right-hand side takes the solid's face velocities (`solid` MacField) on the closed parts of faces, which is
what a moving body supplies. A body is one coupling object in the next pass: Batty et al. §4, its six unknowns
eliminated into the SPD term. The preconditioner stays the grid's. Not implemented: the Jacobian assembly, the
body's face fractions moving with it, `rigid.bodies` ports, and the evidence case 9.

## 10. Limits and what was not verified

- **First order in time for nonlinear interface transport.** The interface is advanced with the step-start
  velocity. Dam-break fronts at 32 and 64 cells differ by up to 0.2 in T. Making the coupling second order without
  losing the waves' symplectic structure was not attempted: a Heun on the joint system damps waves at O(dt).
- **Solid boundaries (m7a.2).** No face is closed. Case 5s gives about second order for the pressure, and first
  order (0.7–1.1) for the velocity on the faces beside cut cells. The Ng–Min–Gibou face-fraction operator's
  gradient is one order below its solution at the boundary. Cut faces less than 1/4 open are not transported.
  Meshes must be closed (ray parity).
- **Robustness of under-resolved films (m7a.2: causes found and fixed, §13).** The m7a.1 text: "Thin films in
  corners and narrow cut cells amplify their face velocities … the root cause … was not found." The sweep now has
  no clamped face, and the count reaches the progress detail. Remaining: narrow cut faces still take large
  projected speeds (up to 32 m/s, invalid, never transported). Faces just above the 1/4 threshold take single-step
  peaks of up to 11.8 m/s against the 13.3 m/s limit (cube 64³ with slits of 0.1 and 0.3 h, §13.2). Large-deformation accuracy stays first order.
- **Reinitialisation.** It is triggered by a gradient threshold, so the scheme is state-dependent but
  deterministic. Waves at ka ≥ 0.1 can trigger it.
- **Numerical damping of waves.** About 2e-4 per radian. It decreases only slowly (order about 0.5) and is near the
  precision of a 3-period, 6-extremum fit.
- **Raw volume loss.** Without correction: 1e-3 of the volume over 10 periods at 128 cells, irregular convergence.
  The correction keeps volume to 1e-8.
- **Particles.** Their finest-level transport order is 1.5–1.7. Particle output is large.
- **Not run.** 256³; the VS2022, Android and Xcode builds (lists edited by script; the make build compiles
  everything); the GUI; Windows.
- **Bitwise reproducibility** is per platform (libm `sin`, `cos`, `exp`, `log`, `sqrt`).

## 11. Pins and change log

- `SceneEditorSuggestionsTest`: keyword count 190 → 191 (`liquid_simulation`); the comments are updated.
- `RewireConnectionTest`: repeatable pure-Reference parameters on unaddressable categories go 5 → 6
  (`liquid_simulation.solid_object`).
- `CstDeriveGoldenTest`: one hand-added line for `liquid_dam_break.RISEscene` (1734 bytes, 33 lines). It comes
  from `--dump` with the log line stripped; the method was verified to reproduce `simulated_points_spray`'s line
  exactly. The generator was not run. Until the scene is tracked the test may report the line as STALE.
- `IJob`, `ChunkCategory`: unchanged (the chunk uses `FinalizeSolverChunk`).
- `tests/test_schedule.tsv`: `LiquidSolverTest` wide:3, `LiquidValidationTest` wide:4.
- `run_all_tests.{sh,ps1}`: `LiquidValidationTest` is dual mode.
- `Parsers/README.md`: Simulations 5 → 6; the total line corrected to 191 (it said 187).
- Contract: no change. Appendix C row: solver side only (new solver `liquid.mac`; existing keys, payloads and
  caches unchanged).
- m7a.2 (§13): no pin outside `LiquidValidationTest` and `LiquidSolverTest` moved. Their tolerances and checks are
  listed in §13.5. Caches baked by m7a.1 are stale (the version is keyed).

## 12. What remains (design §7)

- **Step 4:** rigid bodies in the pressure solve (§9), case 9.
- **Step 5:** viscosity (variational, Batty & Bridson 2008) and surface tension (the ghost-fluid jump), cases
  10–11.
- **Step 7:** cloth as a moving boundary (preview).
- Also: a second-order interface–momentum coupling for large deformation, a second-order velocity at cut-cell
  solids (§13.4), measured Lobovský front data, and 256³ performance work (active-cell compaction, fewer joins).

## 13. 2026-11-25 (m7a.2): the velocity runaway in under-resolved films

The runaway had two causes, and a third, geometric defect showed up once faces were no longer closed. Each was
reproduced in a small deterministic case with the safeguard off. Each was measured at a recorded state, with the
cause removed from that same state, and then removed in the solver. The scratch drivers (an instrumented copy of
`LiquidModel::Step` that is bit-identical to it, a runner, and the state-copy comparisons) are outside the repository.

### 13.1 Cause A: WENO5 momentum stages create new velocity maxima in films against a wall

**Reproducer.** The cube dam break in 2-D: 96 × 96 × 1 cells, the obstacle across the whole depth,
`min_face_fraction` 0.5 (m7a.1), `speed_limit` 0. The x-velocity of face (94, 95), in the ceiling row beside the
far wall, grows from 9.3 m/s at step 1376 to 28 m/s at step 1405. The run peaks at 159 m/s. In 3-D, the demo box at
48 × 24 × 24 has the same pattern at face (47, 23, 11), ceiling row against the far wall: from 5.2 m/s at step 305
to 97 m/s at step 321, then NaN.

**State at step 1376 (2-D).** In the ceiling row, the cells 92–95 have φ/h = 0.055, −0.009, −0.150, −0.739. The
x-faces 91–96 carry 0.16, 4.76 (both extrapolated), 8.83, 9.32, 4.26 and 0 (the wall). The film is one cell thick,
runs into the corner, and turns down the far wall. The face's step budget:

| Stage | u (m/s) | Change |
|---|---|---|
| start | 9.323 | |
| advection, stage 1 (dt · L = 6.54e-4 · 1162) | 10.082 | +0.759 |
| projection 1 | 8.934 | −1.148 |
| stage 2 and projection 2 | 9.309 | |
| the following steps | 9.83, 10.59, 11.60, … 28.46 (step 1405) | about +5% a step |

The upwind difference of u along x at the face is +0.49 per cell, and on its own it would decelerate the face.
WENO5's left-biased derivative reads the five differences +4.60, +4.08, +0.49, −5.06 and −4.26 per cell, which
reach two faces downwind, to the wall. All three candidate stencils are non-smooth there and give −2.33, −1.96 and
−3.34 per cell, so the weighted derivative has the wrong sign. The stage therefore puts the face at 10.08, above
every value of its upwind cell (8.83 to 9.32): a new maximum. The excess divergence in cell (94, 95) is removed
mostly through its free-surface face below (θ = 0.30, conductance A/θ = 3.3, against 1.5 to the air on the left
and 1 to the liquid on the right). The film is turned rather than decelerated, the next stage overshoots again,
and the rate grows with u²/h. The defect is local: of 22,269 valid faces, 3 overshoot their upwind range by more
than 1% of the largest speed.

**The cause removed from the same state.** Steps 1377–1406 were continued from the recorded state, once with the
m7a.1 stage and once with the stage bounded by its upwind cell (below). Largest valid speed:

| Step | 1377 | 1380 | 1385 | 1390 | 1395 | 1400 | 1405 |
|---|---|---|---|---|---|---|---|
| m7a.1 stage | 9.31 | 11.60 | 14.66 | 14.25 | 15.77 | 20.10 | 28.46 |
| bounded stage | 8.62 | 7.55 | 6.56 | 6.53 | 6.49 | 6.45 | 6.37 |

In the demo box from step 305, the m7a.1 stage reaches 97 m/s at step 321 and NaN at step 325. The bounded stage
falls from 6.3 m/s to 3.5 m/s. Its stage at that face gives 6.25 m/s where the unbounded one gives 7.35.

**Checks on other suspects (2-D 96², m7a.1 otherwise; peak speed over 2.5 s).** Semi-Lagrangian momentum: 11.9.
MacCormack: 11.5. First-order upwind derivatives: 11.2. One WENO5 stage: 9.5. Two WENO5 stages: 159. Two bounded
WENO5 stages: 7.7. Over the first 1.6 s, without particles the run peaks at 8.3, and without the volume correction
at 9.2. Whether a
corner film forms is chaotic: any change to the run moves it. Without reinitialisation the run diverges by step
1045. The ghost-fluid θ clamp had no effect in m7a.1. None of these changes the defect at the recorded state.

**Fix: bounded stages (`LimitedAdvectionStage`, LiquidFlow.h).** Each forward-Euler stage u + dt L(u) of the WENO5
operator is limited, face by face, to the range of u over the corners of the face's upwind cell. That cell is the
face and its neighbours one step against the advecting velocity along each axis. The exact transport of a
component takes its value from that cell when the Courant number is at most one (the CFL is 0.5). A stage
therefore creates no new extremum, and the Heun and SSP-RK3 combinations, which are convex, keep the bound. The
operator and its stencils are unchanged, so smooth flow is unaffected except at extrema. Taylor–Green changes by
3% at 16 cells and by 0.1% at 128, with orders 2.21, 2.20 and 2.25. The waves, sloshing, collapse and hydrostatic
cases agree with m7a.1 to three or four digits (§4). This is the maximum-principle limiter of Selle et al. 2008
(already used here for MacCormack) applied to each SSP stage.

With the bound alone and `min_face_fraction` 0.5, the whole sweep (§2) has no clamped face. At 0 it has 16 in
cube 20³ and 14 in cube 64³: causes C and B.

### 13.2 Cause B: the velocities of narrow cut faces were transported as full faces

**Reproducer.** The 2-D dam break at 64² with the obstacle's faces 0.2 h inside the cells (x₀ = 35.2 h, top at
12.8 h), `min_face_fraction` 0, the bound of 13.1 on. Faces of A = 0.2 reach 19 m/s. The smallest cases: 32² with
the obstacle at (17.2 h, 6.8 h) reaches 14.8 m/s, and 40² reaches 18.9. In 3-D, cube 64³ has 14 clamped faces.
Every excursion above 12 m/s is on a face of A = 0.20–0.22 beside a cell with φ between −0.001 h and −0.027 h.

**State at step 126 (2-D 64², bound on).** The run-up sheet rises at 5.6 m/s in the 0.2 h slit between the
dam-break column and the obstacle's face. That is cell column x = 35, whose y-faces are 20% open. Cell (35, 12), at
the obstacle's top corner, becomes liquid this step with φ = −0.054 h. Its top face (A = 1) lay between two air
cells at the start of the step, so the extrapolation had copied the slit face below it into it: 5.53 m/s through
a full face, where the slit delivers 0.2 × 5.61 = 1.12. The projection drains the excess of 4.4 face units
(q = −0.59) mostly through the cell's face to the air on its right, (36, 12), which has A = 0.2 and θ = 0.058. The
result there is u* + q/θ = −10.3 m/s. At the next step that velocity is transported like a full face's, through
the extrapolation and the WENO stencils. The cell switches between liquid and air, and the cycle repeats.

**Why.** The projection minimises Σ A_f θ_f (u_f − u*_f)², with θ = 1 between liquid cells, subject to the
divergence. The ghost-fluid update u* + q/θ is exactly this minimiser. A face with little mass takes a large
velocity at little cost. A cut face's velocity is the speed in its open part, and A u is its flux. Copied into a
full face by the extrapolation or the advection, it carries 1/A times the flux it stands for. Closing faces below
1/2 had hidden this by limiting 1/A to 2, at the price of locating solids only to half a cell.

**The cause removed from the same state.** The 2-D 64² run was taken to step 100 with no narrow faces excluded.
The state was then copied into a second model with `min_transport_fraction` 0.25 (below), and both ran 100 more
steps. Peak valid speed: 19.0 m/s without the rule, every peak on a face of A < 0.25; 5.6 m/s with it.

**Fix: `min_transport_fraction` = 0.25.** The step's last projection leaves cut faces less than 1/4 open invalid.
The first projection of the Heun step keeps them for the second stage, so one-step cases such as 5s are unchanged.
Invalid faces keep the velocity that satisfies their cells' constraints, but the extrapolation replaces them
before the next step transports anything. The pressure solve keeps the exact fractions.

- *Why A and not A θ.* Excluding faces of small A θ also cured the jets, but damped waves whose surface sits
  close to a cell centre. A standing wave at kd = 1.0, 16 cells, θ = 0.046 had its frequency error go from
  +6.8e-4 to −3.6e-2 and its decay from 2.1e-4 to 7.4e-2 per radian. With A alone the waves are unchanged.
- *Why 1/4.* The threshold must exceed the slit's 0.2: at 0.1 the 2-D slits still reach 14.8 and 18.9 m/s. Slits
  just above it are transported: 2-D slits of 0.3 h peak at 8.8, 8.1 and 9.5 m/s (32², 40², 48²), and of 0.4 h at
  6.9–7.2.
- *In 3-D.* With slits of 0.1, 0.3 and 0.45 h there is no clamped face. Cube 48³ peaks at 6.6, 9.1 and 7.9 m/s,
  and cube 64³ at 11.8, 11.1 and 9.2 m/s (the limit is 13.3). Each 64³ peak above 9.5 m/s lasts a single step, on a
  face 42% open beside a sliver cell (φ ≈ −0.01 h) at the floor behind the obstacle. That is the same mechanism with
  1/A = 2.4, and it does not grow.
- *At 0.5* the 2-D slits peak at 4.9 and 6.3 m/s. 0.25 is kept so that as many cut faces as possible are
  transported.
- *In both projections.* Applying the rule to the Heun step's first projection as well changed nothing in the 2-D
  slits (8.8 and 8.1 m/s). It made case 5s worse (1.79e-2 against 1.38e-2 at 32 cells), so it applies only to the
  last projection.

**Outputs.** Invalid faces hold values that are never transported: air faces, and narrow cut faces with their
projected speeds. The surface, field and particle velocities are therefore sampled from u extrapolated as for the
interface (`OutputVelocity`), which is the field the next step moves the surface with.

### 13.3 Cause C: round-off slivers where a solid face lies on a grid plane

With `min_face_fraction` 0, cube 20³ (every obstacle face on a grid plane) had 16 clamped faces in 6 steps. They
were inside the obstacle, on faces with open fractions near 1e-16, because node distances such as
0.7 − 14 × 0.05 = ±1e-17 have random signs. The 2-D 40² slit case diverged at step 170 the same way. FaceFractions
now treats node distances within 1e-9 h as zero, and only strictly positive values as open. A solid face on a grid
plane then closes the faces in that plane and opens none inside the solid. Cube 20³ has no cut face, and no
clamped face.

**Counting.** m7a.1 clamped and counted every face, valid or not. m7a.2 clamps and counts valid faces only.
Counting every face, the m7a.2 sweep has 64 face-steps above the limit, all in cube 64³ (largest 17.9 m/s); m7a.1
counted 1250 there. In cube 64³ all 64 are on invalid cut faces less than 1/4 open (§13.2), never transported. The slit variants
counted the same way give 41 face-steps in cube 48³ with 0.1 h slits (largest 31 m/s). In cube 64³ they give 121
with 0.1 h slits (109 on narrow cut faces, 12 on air faces; largest 32 m/s) and 3 with 0.3 h slits.

**Surfaced.** `LiquidModel::clampedFaces` and `clampedSteps` total `StepStats::clamped` over a run. The solver puts
them in the progress detail ("steps N, clamped faces X in Y steps"), the channel cloth uses for its solver counts.
By the worker code, `rise-sim status` prints it as `detail:` while a bake runs; no test exercises that path. The totals are per run, not checkpointed (as cloth's),
and are not in the cache or the label: the contract has no per-run diagnostics record.

### 13.4 Solid-boundary convergence (case 5s)

Case 5s (§4) puts case 5's column against two cut-cell planes rotated 0.35 rad off the grid. It measures the
acceleration after one step from rest against the rotated series.

| Cells per H | 16 | 32 | 64 | 128 | Orders |
|---|---|---|---|---|---|
| velocity max / g (on faces within 2h of a solid) | 2.92e-2 | 1.38e-2 | 8.28e-3 | 5.06e-3 | 1.08, 0.74, 0.71 |
| velocity max / g, faces farther than 2h | 4.90e-3 | 1.89e-3 | 1.16e-3 | 9.00e-4 | 1.38, 0.71, 0.36 |
| velocity L2 / g | 1.48e-2 | 3.15e-3 | 3.44e-3 | 8.97e-4 | 2.23, −0.13, 1.94 |
| pressure max / (ρgH) | 5.37e-4 | 1.58e-4 | 3.57e-5 | 1.25e-5 | 1.76, 2.15, 1.51 |
| m7a.1 (closing below 1/2): velocity max | 2.47e-1 | 3.14e-1 | 3.02e-1 | 2.99e-1 | none |
| m7a.1: pressure max | 2.16e-2 | 8.30e-3 | 6.47e-3 | 2.96e-3 | 1.38, 0.36, 1.13 |

- The pressure converges at about second order.
- The velocity converges at about first order: below 2. Its largest errors are on faces beside small cut cells
  (open fractions down to 0.005). It pollutes the faces a few cells farther in, whose order falls to 0.4 at 128.
- The velocity is the pressure's difference across a face, and at the boundary that loses an order. This is the
  known behaviour of the face-fraction (Ng, Min and Gibou 2009) operator.
- The self-similar variant (α = atan ½, pivot on a node, every level cut alike, open fractions ≥ 0.5) gives
  velocity orders 0.38, 1.14, 0.85 and pressure orders 3.1, 1.89, 1.70.
- Closing faces below 1/2 gave a velocity error of 0.3 g at every resolution: the acceleration beside the wall
  was wrong by a third of gravity and did not converge.
- The rule of 13.2 does not change this case, because both of its projections keep every face.
- The gates: tolerances 1.25 × observed; order minimums 0.6 (velocity) and 1.4 (pressure), below the observed
  orders. In the evidence: `solid-collapse-n32` and `solid-pressure-n32`.

### 13.5 Changed results, tolerances and pins

- Version `m7a.2`.
- New parameter `min_transport_fraction` (0.25).
- `min_face_fraction` default 0 (was 0.5).
- `speed_limit` acts on valid faces.
- The evidence numerics include `min_transport_fraction`.
- Re-measured tolerances (1.25 × observed, rounded up): Taylor–Green 1.8e-3, 3.8e-4, 8.2e-5, 1.8e-5 (were 1.7e-3,
  3.7e-4, 8.1e-5, 1.7e-5), evidence `taylor-green-n32` 3.8e-4, and sloshing (0,1) at 40 cells 5.0e-4 (was
  4.9e-4, which was already below 1.25 × m7a.1's value). Every other existing tolerance still covers 1.25 × its
  new value, so it is unchanged.
- Case 5s and its two evidence cases are new.
- Changed values: Taylor–Green +3% at 16 cells, orders 2.21, 2.20, 2.25. Volume shifts 1.16e-4, 5.84e-5,
  3.42e-5 h. Raw volume loss −9.25e-3, −6.96e-3, −1.55e-3. The dam break of case 8 is closer to Martin and Moyce:
  rms 0.132 and 0.112 against 0.275 and 0.094, front speeds 1.56 and 1.48 √(ga) against 1.63 and 1.51. The smooth
  cases' orders did not regress.
- `LiquidSolverTest`: a refusal of `min_transport_fraction` 1.5, and the progress detail's count in B. Thread
  invariance (C), stop and resume (D, G) and macro steps (E, F) hold.
- No pin outside the liquid tests moved.

### 13.6 Open, and not verified

- **Open.** The velocity at cut-cell solids is first order (13.4). Narrow cut faces still take large projected
  speeds, which are not transported. Faces just above 1/4 open beside sliver cells take single-step peaks of up to
  89% of the limit (11.8 m/s in cube 64³ with 0.1 h slits). That is the zero-clamp sweep's smallest margin. The
  thin-film physics stays out of the label's scope, as before.
- **Not retested.** Whether linear momentum extrapolation would now be stable (§2). It stays constant.
- **Not measured.** Performance (§8). Solids other than boxes and planes, such as spheres and meshes, outside the
  hydrostatic cases. Resolutions above 96 × 48 × 48 in the sweep.
