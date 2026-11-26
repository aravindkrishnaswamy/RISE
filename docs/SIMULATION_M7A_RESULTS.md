# M7a results: the liquid solver (`liquid.mac`)

2026-11-25. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §1, §3, §4, §7 steps 1–3 and the
infrastructure of step 6. Prerequisites: [SIMULATION_M7_PREREQ_RESULTS.md](SIMULATION_M7_PREREQ_RESULTS.md). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (unchanged; no Appendix C entry needed beyond the
solver-side row below).

**Status.** `liquid.mac` is a single-phase, inviscid, free-surface liquid on a staggered grid. It runs on the
contract through both hosts. It bakes, stops and resumes byte-identically in `rise-sim`, with streamed checkpoints.
It publishes identical bytes for 1, 3 and 8 threads, and it earns `predictive` from evidence it computes at
Configure. The smooth canonical problems converge at second order: transport, standing waves at two depths,
sloshing, the far field of an early-time column collapse, and a Taylor–Green vortex. Hydrostatic rest is exact to
the solver tolerance, including a tilted surface and an immersed sphere.

Six design decisions changed under measurement (§2):

1. Reinitialisation runs only when the level set's gradient has drifted.
2. Momentum takes two stages with a projection each.
3. The interface velocity uses a linearly extrapolated field. The momentum field is extrapolated by a constant.
4. φ is not extrapolated into solids.
5. Marker particles are on by default.
6. Cut faces less than half open are closed.
7. A speed safeguard clamps runaway face velocities (§10).

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

**Velocity extrapolation (measured).** Unknown faces are filled layer by layer from known faces of the same
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

- u₁ = P(uⁿ + dt (L(uⁿ) + g));
- uⁿ⁺¹ = P(uⁿ/2 + (u₁ + dt (L(u₁) + g))/2).

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

**Cut faces less than half open are closed (measured; `min_face_fraction` = 0.5).** An under-resolved film
entering a narrow cut-cell slit is squeezed through faces 20–40% open and jets. The trigger is an obstacle face
0.2h inside a cell: the slit has a fully open inlet and its other faces are 20% open. A 20³ dam break against a box
diverged at a threshold of 0.1. At 64³ it diverged at 0.1 and at 0.3 (on 42%-open faces later). At 0.5 those runs
did not jet at the obstacle. The solid is then located to within half a cell (first order); the hydrostatic cases
stay exact.

**Speed safeguard (not part of the validated model).** Even at 0.5, the demo box (with a ceiling) diverged to NaN
at 48 cells. A sub-cell film trapped in the corner of the far wall and the ceiling amplified its face velocity by
about 10% per step. The root cause was not found in this pass. Two safeguards were added:

- face speeds above `speed_limit` × √(2|g|D) (3 by default) are clamped and counted (`StepStats::clamped`);
- a non-finite state fails the step ("the liquid simulation diverged …") instead of publishing NaN.

Final robustness sweep (dam break against a box; 1 m cube 2.5 s, demo box 1.5 s):

| Run | End | Clamped faces (steps with a clamp / steps) |
|---|---|---|
| cube 20³ | finite, max speed 2.2 | 0 |
| cube 32³ | 3.9 | 3 (3 / 692) |
| cube 48³ | 5.6 | 251 (86 / 1401) |
| cube 64³ | 13.3 (at the limit) | 1250 (481 / 2432) |
| demo 48 × 24 × 24 | 2.4 | 148 (92 / 570) |
| demo 96 × 48 × 48 | 4.7 | 240 (120 / 1209) |

The safeguard acts more often at higher resolution, where more under-resolved films form. Bakes complete, but
their thin films and splashes are not physical where it acts. Within the label's stated scope (features of at
least two cells, no spray or thin sheets) this is excluded. It is still the weakest part of this pass (§10).

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
| Momentum "limited high-order conservative, chosen by measurement" | WENO5 non-conservative with Heun stages and a projection per stage | §2: the only second-order candidate; conservative FV was not built |
| Cut-cell solids (fractions) | As designed, plus closing faces < 50% open | Robustness (§2) |
| Rayleigh–Taylor (case 5) | Early-time column collapse | Two-phase RT is out of scope; §4 justifies the replacement |
| Marker particles "corrected by" | On by default after measurement | §2 |
| Outputs | As designed; `particles` opt-in | Frame size |

## 4. Validation

`bin/tests/LiquidValidationTest`: the fast tier is about 40 s on an 8-thread pool; `--extended` is about 160 s and
adds the finer levels, the long vortex, the particle runs at 200 cells, sloshing at 40 cells and the dam break at
64 cells. Tolerances are 1.25 × the error observed at that level with the default numerics, rounded up. Orders must
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
| 2m | Taylor–Green, WENO5, 2 stages | steady exact solution | 16/32/64/128 | 1.34e-3, 2.95e-4, 6.48e-5, 1.36e-5 | 1.7e-3, 3.7e-4, 8.1e-5, 1.7e-5 | 2.19, 2.19, 2.25 | pass |
| 3 | standing wave, kd = 3π/8, ka = 0.01, 3 periods, frequency | ω² = gk tanh(kd) | 16/32/64/128 per wavelength | −1.50e-2, −3.81e-3, −9.28e-4, −2.22e-4 | 1.9e-2, 4.8e-3, 1.2e-3, 2.8e-4 | 1.98, 2.04, 2.07 | pass |
| 3 | same, deep (kd = π) | same | 16/32/64/128 | −1.26e-2, −3.14e-3, −7.85e-4, −1.92e-4 | 1.6e-2, 4.0e-3, 9.9e-4, 2.5e-4 | 2.00, 2.00, 2.03 | pass |
| 3 | numerical decay γ/ω (finite / deep) | 0 (inviscid) | 16/32/64/128 | 5.0e-4, 3.4e-4, 2.8e-4, 1.9e-4 / 4.9e-4, 3.3e-4, 2.7e-4, 1.8e-4 | — | ≈ 0.5 (reported) | report |
| 4 | sloshing, tank 1 × 0.6 m, depth 0.5, modes (1,0) and (0,1) | ω² = gk tanh(kd), k₁₀ = π/a, k₀₁ = π/b | 20/40 across | (1,0) −1.85e-3, +4.63e-4; (0,1) −5.47e-3, −3.94e-4 | 2.4e-3, 5.8e-4; 6.9e-3, 4.9e-4 | 2.00; 3.80 | pass |
| 5 | early-time column collapse, H = 1, L = 1, interfaces off the grid: acceleration at least H/4 from the bottom corner | potential-flow series (below) | 16/32/64/128 per H | 4.86e-3, 1.25e-3, 3.56e-4, 9.43e-5 (/g) | 6.1e-3, 1.6e-3, 4.5e-4, 1.2e-4 | 1.96, 1.81, 1.91 (≥ 1.7) | pass |
| 5 | same, L2 over the column | same | 16/32/64/128 | 1.97e-3, 9.50e-4, 4.67e-4, 2.31e-4 | 1.25 × | 1.05, 1.03, 1.01 (≥ 0.9; log singularity) | pass |
| 5 | the series itself: a_y at the top dam corner | −g (free fall) | 2e5 terms | 1.6e-6 | 1e-4 | — | pass |
| 6 | volume, ka = 0.1 standing wave, 10 periods: raw loss (correction off) | 0 | 32/64/128 | −9.2e-3, −7.2e-3, −1.2e-3 | report | 0.4, 2.6 (irregular) | report |
| 6 | mean / max volume-correction shift (correction on), residual drift | 0 | 32/64/128 | 1.17e-4/4.2e-4, 5.85e-5/3.7e-4, 3.40e-5/5.2e-4 h; drift ≤ 1e-8 | 1.5e-4, 7.4e-5, 4.3e-5; 1e-6 | 1.0, 0.8 | pass |
| 7 | observed orders | | | in the rows above | | | |
| 8 | dam break, square column, front Z vs T | Martin & Moyce 1952 table 1 | 32/64 per width | rms time difference 0.275 / 0.094 (aligned at Z = 1.44); front speed 1.63 / 1.51 √(ga) against M&M 1.39, Ritter 2 | not gated | — | report |
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

**Case 8 sources.** Martin & Moyce 1952 (Phil. Trans. R. Soc. A 244, 312–324), table 1, n² = 1, a = 2¼ in. The
values were transcribed from the JSTOR scan by a research agent; spot-check before relying on single values.

- Scaling: Z = z/a and T = n t √(g/a). The authors normalise time to T = 0.80 at Z = 1.44 (they give no true zero),
  so the comparison aligns there.
- Lobovský et al. 2014 (J. Fluids Struct. 48, 407–434; arXiv 1308.0115) give front positions only as a plot (their
  Fig. 12). Their Table 1 gives average front speeds for t√(g/H) > 1: 1.56 and 1.34 for H = 300 and 600 mm, and
  their reduction of Martin & Moyce's 57 and 114 mm columns is 1.48 and 1.69. All are below Ritter's 2.
- **TODO:** Lobovský's front data as numbers (their supplementary MATLAB figures at
  canal.etsin.upm.es/papers/lobovskyetaljfs2013/ were not fetched).

Our inviscid, free-slip front, 1.51–1.63 √(ga), lies between M&M and Ritter, as expected without bed friction.

## 5. Evidence and the label

Evidence is computed at Configure, as cloth does: a fast subset of the cases with the configured numerics. It is
memoised per process by the SHA-256 of the numerics, bound to the solver id, version and numerics through
`EvidenceDigest`, and takes about 2–3 s at the first Configure. The tolerances are 1.25 × the error observed with
the default numerics. The transport tolerances have a second value for particles on.

| Id | Check | Tolerance (particles on / off) | Default value |
|---|---|---|---|
| `transport-zalesak-n50` | case 1, L1 | 4.3e-3 / 8.4e-3 | 3.39e-3 |
| `transport-vortex-n50` | case 1, T = 2 | 3.2e-3 / 4.5e-3 | 2.54e-3 |
| `hydrostatic-n32`, `hydrostatic-pressure-n32` | case 2, tilted + sphere, 0.25 s | 1e-7, 1e-7 | 4.1e-10, 1.4e-11 |
| `wave-finite-n32`, `wave-deep-n32` | case 3 | 4.8e-3, 4.0e-3 | 3.81e-3, 3.14e-3 |
| `order-wave` | 16 → 32: max(0, 1.8 − order) | 0.2 | 0 |
| `slosh-n20` | case 4, max of the two modes | 6.9e-3 | 5.47e-3 |
| `collapse-n32`, `order-collapse` | case 5 far field; 16 → 32 order | 1.6e-3, 0.2 | 1.25e-3, 0 |
| `taylor-green-n32`, `order-taylor-green` | momentum; 16 → 32 order | 3.7e-4, 0.2 | 2.95e-4, 0 |
| `volume-correction-n32` | case 6 mean shift / h | 1.5e-4 | 1.17e-4 |

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

The open top and static solids are covered only through hydrostatics (exact) and robustness. No convergence case
measures the solid boundary's position. Closing faces less than half open makes it first order: a limit, §10.

## 6. Solver, parameters, outputs, ports

`liquid.mac` is version `m7a.1`. Parameters are a canonical CBOR/JSON map; unknown keys are refused, and every
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
| `pressure_tolerance`, `theta_min`, `min_face_fraction` | 1e-8, 1e-3, 0.5 | |
| `speed_limit` | 3 | safeguard: face speeds above 3 √(2\|g\|D) are clamped and counted (0: off); not part of the validated model |
| `extrapolation_layers`, `interface_extrapolation_layers`, `linear_extrapolation_layers` | 6, 4, 1 | |
| `publish_fields`, `publish_particles` | false, false | |
| `threads` | min(16, cores) | not keyed |

**Outputs.**

| Output | Codec | Contents |
|---|---|---|
| `surface` | mesh.v2 | inline indices, so a new topology every frame; normal (1), position (m), velocity (m/s) |
| `fields` | grid.v1 | `phi` (m), `pressure` (Pa: q ρh/dt of the step), `velocity` (m/s, cell-centred averages of the faces); one dense block per channel; index i at origin + (i + ½)h |
| `particles` | points.v1 | positions, velocities, radii, ids, `sign` attribute |

Output times do not depend on the step.

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
  `threads` unkeyed; semi-Lagrangian is preview; large grids are not interactive; refusals; every schema default
  equals the solver's; every effective parameter is described; enum values configure; the chunk translation works.
- **B:** six frames with a box solid and a scene-input mesh solid (whose identity is keyed). The surface is closed
  and outward, and at t = 0 encloses the liquid's volume to within its edge rounding (−5.1% at 6 cells across;
  −1.3% and −0.34% at 2× and 4×: second order). Fields and particles decode, and velocities are bounded.
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
safeguard (they affect only the obstacle's cut faces and a final pass over the faces).

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
- **Solid boundaries.** Faces less than half open are closed, which locates solids to half a cell. No case measures
  the solid boundary's convergence. Meshes must be closed (ray parity).
- **Robustness of under-resolved films (the main open problem).** Thin films in corners and narrow cut cells
  amplify their face velocities. Without the speed safeguard, the demo box diverged at 48 cells and a cube dam break
  at 64³ diverged at `min_face_fraction` 0.1 and 0.3. With it, every run completes, but the clamp acts in up to 20%
  of the steps at 64³ (§2 table). The root cause (suspected: the interplay of momentum advection with constant
  extrapolation and the ghost-fluid update in nearly empty cells) was not found. Bakes with many clamps have
  non-physical splashes; the step statistics count them, but the count is not yet surfaced in the cache or the
  label.
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

## 12. What remains (design §7)

- **Step 4:** rigid bodies in the pressure solve (§9), case 9.
- **Step 5:** viscosity (variational, Batty & Bridson 2008) and surface tension (the ghost-fluid jump), cases
  10–11.
- **Step 7:** cloth as a moving boundary (preview).
- Also: a second-order interface–momentum coupling for large deformation, a solid-boundary convergence case,
  measured Lobovský front data, and 256³ performance work (active-cell compaction, fewer joins).
