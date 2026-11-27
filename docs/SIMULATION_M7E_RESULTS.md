# M7e results: cloth coupled implicitly with the liquid (`liquid.mac` m7e.1)

2026-11-26. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §2 (the stable two-way cloth treatment, the
"later step"). Builds on [M7d](SIMULATION_M7D_RESULTS.md) (sheets as face-crossing moving boundaries, the lagged
exchange and its failure), [M7b](SIMULATION_M7B_RESULTS.md) (bodies in the pressure solve, the owned exchange),
[M6](SIMULATION_M6_RESULTS.md) (group host, `cloth.shell` ports) and [M3a](SIMULATION_M3A_RESULTS.md) (the cloth
integrator). Contract: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), four Appendix C rows (one
additive member, `PortDeclaration::startOfStep`).

**Status.** `preview`. A sheet with `coupling: implicit` puts its cloth's linearised response into every pressure
solve of the liquid, and the cloth then takes the liquid's own steps with the same linear model. In the M7d flap
case the coupling is stable at every combination run (areal densities 0.05–100 kg/m², 24/40/64 cells, macro steps
1/30 and 1/120 s, still water for 2 s and a dam break for 2 s), with no relaxation and no clamp; the staggered exchange
of M7d diverges on the same case (§4). Cotton-weight cloth (0.2 kg/m²) in water works, in the demo too (§9).

Accuracy is checked against a 1-D piston (exact, 4e-5), a plate's added mass (independent of the plate's mass to
0.1%), a membrane's static deflection under a liquid layer (−0.8% against the cloth alone under the analytic
hydrostatic load) and a membrane's natural frequency under a layer (−5.2% against linear potential flow, of which
−4.4% is the cloth's own discrete frequency) (§5). The momentum exchange is exact to binary32 (§6).

What does **not** work: a light sheet that holds liquid on one side only and sweeps across cell centres under
gravity (a membrane sagging centimetres under a water layer) lets the liquid through it; the load-path test passes
only for deflections inside a cell (§5.3, §11). *Update (M7f, [results](SIMULATION_M7F_RESULTS.md)): fixed. In
2-D the cause was the test strip leaving the one-cell slab, so no face was closed; in 3-D it was the level set
reading across the sheet and the volume correction.* A zero-thickness sheet has no buoyancy, so cloth lying on the
surface sinks (§5.5).

Bakes without implicit sheets publish frames byte-identical to m7d.1, staggered and one-way sheets included, and
cloth-only and rigid+cloth groups are unchanged (§8). `cloth.shell` stays `m3a.4`.

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `SheetDynamics` (the accepted linear model; S, its factorisation, predictor, `ApplyS`), `SheetCoupling` (an `IPressureCoupling`), crossing normals and coordinates |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Coupled sheets in the step: face mass, predictor, `ProjectCoupled`, the step's impulse history, sweeps (`SweepSheetCells`, `PinFalseInterface`), geometry at the linearised positions |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | `coupling`, `response`, the three new sheet ports, accepting the mechanics, offering the force and the steps, owned ports, label, checkpoints, version m7e.1; chunk options |
| `src/Library/Simulation/Cloth/ClothModel.{h,cpp}` | `portForce`; `Mechanics` (force and projected Hessian); the linear model `BeginLinear`; `PenaltyContact` factored out (same arithmetic) |
| `src/Library/Simulation/Cloth/ClothSolver.cpp` | Ports `surface_mechanics`, `surface_stiffness` (offered), `surface_load_steps` (accepted), channel `force` on `surface_load`; taking the partner's steps; checkpoints |
| `src/Library/Simulation/SimulationSolver.h`, `SimulationGroup.cpp` | `PortDeclaration::startOfStep`; macro ends snapped to output times within 1e-6 of a macro step |
| `tests/LiquidClothCouplingTest.cpp` (new) | §10 (12 s), plus `--probe`, `--plate`, `--membrane`, `--copy`, `--free` for the tables below |
| `tests/ClothSolverTest.cpp`, `LiquidRigidTest.cpp`, `LiquidSheetTest.cpp` | Pins moved (§12) |
| `scenes/Tests/Simulation/liquid_cloth_wave.RISEscene`, `scripts/simulation_m7e_cloth.sh` | Demo (§9) |

No new library source file: the build lists are unchanged.

## 2. Formulation

### 2.1 What was tried, in order, and why it failed

A per-mode linear model (a cloth mode of mass m and stiffness K, the liquid's added mass M_a for that mode, zero
for modes the grid cannot see) was used first to compare schemes, then each candidate was built and measured.

| Scheme | Per-mode model | Measured in the code |
|---|---|---|
| Staggered (M7d) | diverges for M_a/m > ~1 | diverges at 0.2 and 10 kg/m² within 3–5 macro steps (§4) |
| (i) cloth predicts its step in vacuum, liquid projects with the lumped masses, cloth takes the impulse as a velocity correction | stable; the elastic force is evaluated at the vacuum prediction, so a mode with H²K/m ≫ 1 is too soft by about 1 + H²K/m (frequency 0.39–0.69 of exact for cotton at H = 1/30 s); statics wrong by the same factor | not built: the model already rules it out for the static case of §5.3 |
| Lumped masses, force at the macro start explicit, no stiffness in the liquid | stable for visible modes, overdamped and wrong frequency | — |
| Liquid with the cloth's stiffness (S = M + dt D + dt² K) in every projection, cloth integrating its own BDF2 substeps with the held mean force | stable in the model | stable for 1 s, then a 0.05–0.2 kg/m² flap grew to 4–20 m/s over 2–3 s (§2.4) |
| The same, the cloth taking the liquid's steps with backward Euler and each step's impulse | — | still grew for light cloth over 3 s: the cloth's nonlinear step differed from the liquid's linear copy |
| **The same, the cloth's elasticity within the macro step being the liquid's linear model (chosen)** | — | stable everywhere (§4) |

The decisive measurements:

- A free cotton sheet with the liquid's copy re-linearised every macro step and no cloth integration at all (`--copy`)
  stayed physical (mean sinking 7 mm/s after 3 s at 40 cells), while the group with the cloth integrating on its own
  grew; the cloth−copy velocity mismatch per macro step was 0.04–0.2 m/s before the growth.
- With the cloth taking the liquid's steps by backward Euler on its own nonlinear model, the mismatch fell to 1e-3 but
  a 0.05 kg/m² flap still diverged over 3 s. With the linear model in the cloth too, the mismatch is 1e-8 m/s (binary32)
  and the same runs are calm.
- Without the face mass of §2.3, a free 0.05 kg/m² sheet tilted 17° in still water accelerated at 0.84 m/s² instead of
  about 3e-3 m/s²: the spurious buoyancy described there.

### 2.2 The coupled projection (derived here)

Derived from the same variational principle as M7b §2.1 (the papers were not consulted in this pass; the result has
the form of Robinson-Mosher et al. 2008's coupled system with the solid's linearised implicit forces). In a step of
length dt, the coupled sheet's vertex velocities V and the liquid's face velocities u minimise

    (rho h^3 / 2) sum_open A_f theta_f (u_f - u*_f)^2  +  (rho h^3 / 2) sum_closed w_f (G_f V - u*_f)^2
    + (1/2) (V - V_c)^T S (V - V_c),        S = M (1 + dt alpha) + (dt beta + dt^2) K

subject to the discrete divergence of every liquid cell, in which a face f the sheet closes carries u_f = G_f V
(G_f V = (w . V) n_a with w the barycentric weights of the crossing on the crossed triangle and n its normal: the
normal velocity, as in M7d). The third term is the sheet's backward-Euler incremental potential linearised about the
macro step's start x0: V_c = S⁻¹ (M v + dt (f0 − K (x_e − x0))), with f0 the cloth's force at x0, K its projected
elastic Hessian, alpha and beta its Rayleigh coefficients and x_e = x0 + Σ dt V the linearised positions. The second
term is §2.3. Eliminating u and V:

    (A + rho h^3 J^T (S + F)^-1 J) q = b,   V = V* + rho h^3 (S + F)^-1 J q,   F = rho h^3 sum w_f G_f^T G_f,
    V* = (S + F)^-1 (S V_c + rho h^3 sum w_f G_f^T u*_f),   J_c = sum_{f of c closed} s_cf G_f^T,

symmetric positive definite (S + F positive definite; tested densely, §10 A). b holds G V* through the solid face
velocities, as for bodies. Heun's two stages are M7b's with (S + F)⁻¹ in place of M⁻¹. Pins keep their accepted
velocity (their rows and columns of S + F are the identity, S_fp V_p goes to the right-hand side).

`SheetCoupling` applies ρh³ Jᵀ(S + F)⁻¹J inside PCG with a banded Cholesky of S + F (reverse Cuthill–McKee order,
factored once per step), in crossing order on one thread: the result does not depend on the thread count.

### 2.3 The liquid on the closed faces (measured)

M7d's closed faces drop the liquid in their control volume (the cell-sized volume between the two centres) from the
momentum balance. Across such a face the hydrostatic pressure still jumps by ρgh over the cell, and that jump acted on
the sheet as a buoyancy of ρ g h n_a² per area, as if the sheet were a slab a cell thick: 412 Pa at 24 cells against a
cotton sheet's weight of 2 Pa. That liquid now moves with the sheet's normal velocity: its kinetic energy is the second
term of §2.2, with w_f = A_f θ_f the projection's own weight (A_f the face's fraction before the sheet closed it, θ_f
the ghost-fluid fraction). At rest its weight and the pressure jump cancel face by face. The impulse returned to the
cloth is its own momentum change in the linearised step beyond its own forces,

    I = S V_new − (M v + dt (f0 − K (x_e − x0)))  =  rho h^3 J q + rho h^3 sum w_f G_f^T (u*_f − G_f V_new),

the pressure's impulse plus the exchange with that liquid. The term also bounds ρh³ Jᵀ(S + F)⁻¹J by about one open face
per closed face, which kept the PCG iteration count moderate (§7).

### 2.4 The exchange

- **Ports.** The cloth offers `surface_mechanics` (`field.v1` over its vertices: `mass` kg, `fixed`, `force` N ×3 =
  gravity + configured pressure + drag + contact − elastic gradient, `damping_mass` 1/s, `damping_stiffness` s) and
  `surface_stiffness` (`field.v1` over the upper blocks of its projected elastic Hessian: `row`, `column`, `stiffness`
  N/m ×9) with `surface_state`, and accepts `surface_load_steps` (`field.v1` over (step, vertex) pairs: `impulse` N s ×3,
  `segment`) and a `force` channel on `surface_load`. The liquid's implicit sheet accepts the first two (with
  `startOfStep`) and offers the latter two. All five are `ownedImplicitPorts`; the group runs `owned <liquid> <cloth>`.
- **Order.** Gauss–Seidel, liquid first (the owned exchange refuses the other order). At the macro step's start the
  liquid reads the cloth's state and linear model (x0, f0, K, M, pins, alpha, beta), integrates its copy of the sheet
  with every step, and offers each step's impulse I_j and their segment ordinals (the liquid splits a macro step at
  output times into equal steps).
- **The cloth's step.** With the steps connected, the cloth takes exactly those steps (the same split formula, so the
  same step ends bit for bit) by backward Euler, with I_j/dt_j as the step's force, and with its membrane and bending
  energy replaced by the liquid's linear model (`ClothSystem::BeginLinear`: g0 and K0 at x0, rounded to binary32 as the
  ports carry them; Rayleigh damping with K0). Contact stays exact (barrier or penalty). The cloth's state therefore
  equals the liquid's copy to rounding (1e-8 m/s), and the nonlinearity enters only through re-linearising at every
  macro step: a linearly implicit cloth step over the macro step.
- **Geometry.** The copy's geometry drifts with each step's start velocity (M7d) and, after each step, is set to the
  linearised positions x_e, so that at the next macro start the accepted cloth positions coincide with it.
- Without `surface_load_steps` connected the cloth applies the mean force over its own substeps (M7d's behaviour);
  that exchange is the unstable one of §2.1 and is not a supported configuration.

### 2.5 Sides of a sheet holding liquid on one side

A sheet that holds liquid on one side has a false interface (the level set's zero at the sheet). Two rules keep it
there for coupled sheets (measured on the membrane of §5.3):

- **Sweeps.** A cell whose centre the sheet crossed in a step (found by pairing the crossings along each lattice line
  before and after) takes its new side's level set, extended with unit slope towards the sheet, a dry side kept dry and
  a wet side wet; marker particles beside a coupled sheet's closed faces are removed. Without it a membrane that
  oscillated across cell centres carried half of the layer above it to its dry side within 0.4 s.
- **Pinning.** Across a closed face with one side wet and the other notably drier and not fed by a wet open neighbour,
  the drier cell's level set is the distance to the crossing. M7d's creep rule (an air cell beside a sheet keeps its
  value while it falls) is not applied beside coupled sheets: it held the false interface while a membrane sagged
  inside a cell gap, and the membrane's oscillation came out stiffer with every step taken (ω 30, 60, 129 rad/s for one,
  four and sixteen steps per 1/120 s; measured with the double-width strip of §5.4's test defect, the dependence on the
  step count being the point; after the change 29.7, 30.3, 30.3 with that strip).

These rules do not stop the leak of §5.3 for large sweeps under gravity.

## 3. Parameters, ports, refusals

- `sheets: [{…, motion: port, coupling: implicit, mechanics_port (sheet_mechanics), stiffness_port (sheet_stiffness),
  steps_port (sheet_load_steps), response: stiffness | lumped}]`; written back only when implicit (staggered sheets keep
  m7d.1's effective parameters). `response: lumped` drops K from the liquid (the masses and f0 alone), kept to show
  why the stiffness is needed. Chunk: `sheet <name> port coupling implicit [mechanics_port …] [stiffness_port …]
  [steps_port …] [response …]`.
- Refused: `relaxation` with `coupling: implicit`; the implicit ports on a staggered sheet; duplicate port names;
  an implicit sheet outside a group's macro steps (free-running); viscosity with sheets (kept, with the message
  "leave viscosity at 0 with sheets": the viscous solve does not see a sheet).
- Label: always `preview` with an implicit sheet, condition `sheets: [port-implicit]`, an M7e scope sentence, and the
  fidelity limit "sheets: a thin surface as a face-crossing moving boundary coupled implicitly through its partner's
  linearised response (preview, not certified)". No evidence case added.

## 4. Stability

The M7d §6 flap in the flume (1 × 0.6 × 0.4 m, still water at 0.4 m), released 17° from the vertical, here 0.36 m wide
and 0.45 m long (12 × 12 vertices) and inside the flume (2 cm from each side wall; M7d's 0.44 m flap reached 2 cm
outside the walls, where it hung in vacuum); cotton with its density overridden. Liquid 8 threads, cloth 4, 2 s.
"Blew up": a vertex faster than 20 m/s or a failed step. Max |v| is over all vertices and over the part below 0.38 m
("in water"); the part above the water is cloth in vacuum (no air), and a light flap whips there. Roughness: M7d's
second difference of the flap's total x force over macro steps (0.3–0.6 smooth, > 2 alternating).

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | stable; 0.77 (0.105 in water); 0.53 | stable; 0.69 (0.058); 0.24 | stable; 0.25 (0.023); 0.02 | stable; 0.40 (0.20); 0.15 | stable; 0.26 (0.066); 0.02 | stable; 0.23 (0.056); 0.004 |
| 0.2 | stable; 0.27 (0.076); 0.12 | stable; 0.16 (0.081); 0.05 | stable; 0.21 (0.029); 0.03 | stable; 0.38 (0.16); 0.05 | stable; 0.19 (0.071); 0.009 | stable; 0.29 (0.052); 0.03 |
| 1 | stable; 0.24 (0.090); 0.03 | stable; 0.50 (0.50); 0.04 | stable; 0.096 (0.038); 0.02 | stable; 0.33; 0.03 | stable; 0.091; 0.009 | stable; 0.093 (0.054); 0.02 |
| 10 | stable; 0.20; 0.05 | stable; 0.22; 0.05 | stable; 0.23; 0.05 | stable; 0.27; 0.08 | stable; 0.22; 0.05 | stable; 0.22; 0.03 |
| 100 | stable; 0.70; 0.28 | stable; 0.74; 0.28 | stable; 0.78; 0.26 | stable; 0.69; 0.26 | stable; 0.75; 0.26 | stable; 0.77; 0.24 |

Entries: result; max |v| m/s (in water, when different); roughness. At 100 kg/m² the flap, released unstretched, bounces
on its own stretch (about 0.7 m/s in the plane, as in vacuum: tangential motion is free slip).

The dam break (the column of the demo against the flume's −x wall, the same flap), 2 s:

| kg/m² | 24 cells 1/30 | 40 cells 1/30 | 64 cells 1/30 | 24 cells 1/120 | 40 cells 1/120 | 64 cells 1/120 |
|---|---|---|---|---|---|---|
| 0.05 | stable; 3.2; 2.1 | stable; 3.8; 0.94 | stable; 4.0; 0.94 | stable; 7.3; 0.90 | stable; 4.2; 1.05 | stable; 3.3; 0.66 |
| 0.2 | stable; 8.9; 1.6 | stable; 2.6; 1.2 | stable; 3.1; 0.94 | stable; 3.2; 1.1 | stable; 3.2; 0.81 | stable; 2.6; 0.57 |
| 1 | stable; 4.4; 2.1 | stable; 2.4; 0.94 | stable; 3.1; 0.51 | stable; 4.5; 1.2 | stable; 3.1; 0.98 | stable; 2.1; 0.55 |
| 10 | stable; 2.0; 1.6 | stable; 1.6; 1.2 | stable; 2.5; 1.3 | stable; 3.1; 1.6 | stable; 1.5; 1.3 | stable; 1.7; 1.5 |
| 100 | stable; 1.3; 0.89 | stable; 1.4; 0.82 | stable; 1.2; 0.80 | stable; 1.5; 1.5 | stable; 1.5; 0.81 | stable; 1.9; 0.61 |

The surge runs at about √(2 g 0.45) = 3 m/s; a light flap moving with it is expected to reach that. At 24 cells two
light cases peak at 7–9 m/s (a coarse surge; not seen at 40 or 64 cells). Roughness near 1 is the impulsive load of
a surge, not an alternation.

The staggered exchange on the still-water case (40 cells, 1/30 s, 1 s): 0.2 kg/m² blew up at macro step 3 (362 m/s),
10 kg/m² at step 5; 100 kg/m² bounded but rough (6.5 m/s, roughness 3.8); 500 kg/m² bounded (2.1 m/s, 1.2). The fast
test (§10 B) gates both sides at 24 cells.

Edge-to-cell ratio: the flap's edges are 3.3 and 4.1 cm, so the ratio is 0.8–1.0 at 24 cells, 1.3–1.6 at 40 and
2.1–2.6 at 64. Finer cloth was also run: a 24 × 24 flap at 24 cells (ratios 0.36–0.47) was stable for 2 s (0.2 kg/m²:
0.17 m/s in water, against 0.08 with 12 × 12), and the copy alone with a 12 × 12 free sheet at 24 cells (ratio 0.52)
was calm for 3 s. Below a ratio of about one, the cloth's sub-cell modes are invisible to the liquid (no face crosses
between their vertices) and move as in vacuum, without added mass. The tested range is 0.36–2.6; it is a statement of
what was run, not a bound.

## 5. Accuracy

### 5.1 A piston (exact)

A stiff free plate spanning a 2-D tank (32 cells across 0.3 m), a 5 cm liquid layer above it, air below, no gravity,
a uniform load p0 = 20 Pa on the plate: a 1-D incompressible column, a = p0/(σ + ρd) exactly.

| σ (kg/m²) | a measured | p0/(σ + ρd) | error |
|---|---|---|---|
| 0.2 | 0.398392 m/s² | 0.398406 | −3.7e-5 |
| 50 | 0.199999 | 0.2 | −7.0e-6 |

Reference quality: exact (no discretisation in the reference). §10 D gates 1%.

### 5.2 A plate's added mass (self-consistency)

A stiff free plate 0.24 × 0.24 m (12 × 12 vertices) released at rest in the flume, horizontal at y = 0.25 (water on
both sides), gravity on: a0 = σg/(σ + m_a) (no buoyancy for zero thickness), so m_a = σ(g/a0 − 1) per area from the first
macro step (1/240 s) must not depend on σ.

| σ (kg/m²) | 0.05 | 0.2 | 1 | 10 | 100 |
|---|---|---|---|---|---|
| m_a, 40 cells (kg/m²) | 143.8 | 143.8 | 143.9 | 144.8 | 149.0 |

The same over macro steps 1/240, 1/60 and 1/30: 143.8, 143.9, 147.1 (σ = 0.05; the last includes motion over the step).
At 24 cells: 134.9 and 138.2 for σ = 0.05 and 10 (2.4% apart). For scale, a disk of the same area in unbounded liquid
has (8/3)ρR³/A = 115 kg/m²; the flume (walls 8 cm from the plate's sides, the surface 15 cm above, the floor 25 cm
below) changes it, so this is an order-of-magnitude check only. Quality: a consistency check (the added mass is a
property of the liquid and geometry), not an absolute reference.

### 5.3 A membrane under a liquid layer (static; load path end to end)

A strip membrane across a 2-D tank (span 0.3 m, pinned at both walls, cotton density with the stretch stiffness
overridden, prestretch 1.01), at the height of a face plane (0.046875 m), a 5 cm water layer above, air below, gravity.
Reference: the cloth alone (`Cloth::ClothSystem::SolveStatic`) under the analytic hydrostatic pressure ρg(y_s − y) on
its upper face, with the level y_s from the layer's volume over the sagged membrane, iterated to a fixed point: exact
for the discrete cloth and the analytic load. Measured: the mean mid-span sag over the second half of 1 s.

| E (N/m) | sag (reference) | 32 cells | 64 cells | 128 cells |
|---|---|---|---|---|
| 1e6 | 0.56 mm | −0.88% | −0.78% | −0.76% |
| 2e5 | 2.76 mm | −0.92% (rest at 0.05 m) | −0.33% (sweeps one cell centre) | — |
| 2e4 | 16.5 mm | **fails**: liquid passes the membrane | **fails** (−85%; 95% of the layer below within 0.6 s) | — |

For sags inside a cell gap the load path is right to −0.8%, and this does not converge: a floor of about 0.76%
remains, plausibly the volume correction misplacing volume beside the false interface (M7d's 1–2% open item), not
isolated. For the large sag the membrane sweeps several cell centres per oscillation with a hydrostatic load of
about 490 Pa on a 2 Pa sheet: the sweeps close and open faces and change the liquid's active cells, the kicks make the
light membrane jitter at the grid scale (a column's crossing jumped 1 cm in a step and once vanished for a step), and
the level set ends up on both sides. The rules of §2.5 reduced it but do not stop it. This is the main open defect.

### 5.4 A membrane's natural frequency under a liquid layer (one side, no gravity)

The same strip (E = 2e4 N/m, prestretch 1.01, T = 217.6 N/m from the cloth's stiffness), 5 cm layer above, no gravity
(the free surface at p = 0), excited by a 5 Pa pressure on the cloth (sag 0.26 mm, inside a cell gap). Reference,
derived here from linear potential flow: with w = W sin(πx/L) and the layer's potential expanded in the wall modes
cos(nπx/L),

    M_a = rho L [a0^2 d + (1/2) sum_{n even >= 2} a_n^2 tanh(k_n d)/k_n],   a0 = 2/pi,  a_n = 4/(pi (1 - n^2)),
    omega^2 = omega_vac^2 (sigma L/2) / (sigma L/2 + M_a),   omega_vac^2 = T k^2 / sigma,

M_a = 7.116 kg/m (86% of it the piston term a0²dρL). A Galerkin check with eight symmetric membrane modes gives 22.375
against 22.381 for the single mode, so the single-mode reference is converged to 3e-4. ω_vac is the Rayleigh quotient of
the sampled sine with the cloth's K (345.45 rad/s).

| | 32 cells | 64 cells | 128 cells |
|---|---|---|---|
| σ = 0.2, macro 1/120 s: ω (rad/s) | 21.21 | 21.22 | 21.22 |
| against the reference 22.38 | −5.2% | −5.2% | −5.2% |
| against the cloth's own discrete fundamental (330.2 rad/s in vacuum, −4.4% from the Rayleigh value) | −0.8% | −0.8% | −0.8% |

Macro 1/480 s: 21.33 (−0.3% against the discrete fundamental; backward Euler's frequency error at ωdt = 0.18 is
−1.1%). σ = 1 and 10 at 32 cells: −5.2% and −5.4% against the Rayleigh reference. The liquid's part does not depend
on the grid here (the layer is resolved; the coupling error is time-step dominated); the remaining −4.4% is the cloth
model's own fundamental (the strip's in-plane coupling; not analysed). §10 E gates 6.6%.

Two test defects found on the way, for the record: a strip twice as wide as the 2-D slab gave 2× the load per liquid
area (the "spurious" factor 2 vanished with the right width), and a membrane resting next to a cell centre gave an
irregular 64-cell value (+19%) from sweeps; the rest height is now a face plane.

### 5.5 A sheet at the surface

A free cotton sheet lying flat on the surface of still water (40 cells, 1/120 s) sinks: 1.7 cm in 2 s at 0.2 kg/m², 8 cm
at 1 kg/m², flat, without jitter (max 2.5 cm/s and 15 cm/s). A zero-thickness sheet with mass has no buoyancy, and
there is no surface tension, so sinking is this model's physics; real cloth floats on trapped air or fibre buoyancy,
which is not modelled. "A floating sheet at rest stays at rest" was therefore not checked. The hydrostatic rest of a
submerged vertical flap is: hanging straight in still water (24 cells, 1 s), its fastest vertex moves at 3.8e-4 m/s
(0.2 kg/m²) and 1.2e-5 m/s (0.05 kg/m²); before §2.3's face mass a tilted 0.05 kg/m² sheet accelerated at 0.84 m/s².

## 6. Momentum

- **The cloth receives the liquid's impulse.** A free sheet (8 × 8, no pins, damping or contact) in the group: per macro
  step, the cloth's momentum change minus H m g minus the sum of the liquid's step impulses (read from the payload) is
  ≤ 3.2e-5 of H m g (§10 F). The residual is the binary32 velocities of the state payload and the binary32 rounding of
  K0 and g0 (whose rows then do not sum exactly to zero).
- **Liquid + the liquid on the closed faces + sheet.** In the piston of §5.1, the vertical momentum of the three grows by
  p0 L t to 4.7e-5 (σ = 0.2) and 1.5e-5 (σ = 50) of it over six macro steps.
- **Not audited:** a 3-D case with gravity and wall impulses (the walls' impulse is tallied by `AuditWalls`, but the sum
  over a flap run was not built). The free-slip staircase also gives the liquid a tangential impulse on the closed faces
  that the sheet does not receive (the closed face's axis e_a against the sheet's n_a n); for a flat sheet under uniform
  pressure the two sum to the same total, otherwise not; its size was not measured.

## 7. Cost

The free 0.24 m sheet in the flume at the model level (the liquid with the coupled copy; 0.3 s, gravity), against the
liquid with no sheet, Apple M4 Max, 8 threads, measured while other jobs ran (timings are indicative):

| Grid | Cloth | PCG iterations per step (two projections) | s per step | projection |
|---|---|---|---|---|
| 40 cells | none | 9.0 | 0.011 | 0.005 |
| 40 cells | 12 × 12 | 26.7 | 0.020 (+84%) | 0.013 |
| 40 cells | 24 × 24 | 26.7 | 0.035 (+215%) | 0.027 |
| 64 cells | none | 14.0 | 0.027 | 0.017 |
| 64 cells | 12 × 12 | 32.8 | 0.048 (+76%) | 0.034 |
| 64 cells | 24 × 24 | 33.4 | 0.066 (+143%) | 0.052 |

The multigrid preconditioner does not know the coupling (it sees the sheet's faces closed): about 9–10 more iterations
per projection. Each iteration applies (S + F)⁻¹ by the banded factor, which grows with the cloth (576 vertices: a third
of the step). The cloth's side: its mechanics are evaluated and offered at every macro step in any group (small), and in
the coupled step it takes the liquid's steps by backward Euler with a quadratic energy (one Newton iteration without
contact). The demo's bake (64 cells, 13 × 19 cloth, 2 s) took 75 s against M7d's 62 s for the staggered flap.

## 8. Without implicit sheets

The m7d.1 binary (built from the clean tree before any change) and the m7e.1 binary baked six reduced scenes in a private
directory; every content-addressed frame file name matches:

| Scene (reduced) | Payloads | |
|---|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | 14 | identical |
| `liquid_rigid_bodies` (32 cells, 13 frames; liquid + rigid group) | 41 | identical |
| `liquid_cloth_flap` (24 cells, 13 frames; M7d staggered two-way) | 29 | identical |
| the same with the load not connected (one-way) | 29 | identical |
| `cloth_flag_scene` (31 frames; cloth alone) | 33 | identical |
| `rigid_cloth_trampoline` (16 frames; rigid + cloth group) | 35 | identical |

The cloth's new offers (mechanics, stiffness) are computed in every group but change no result (the trampoline shows it).
`startOfStep` changes nothing where the liquid steps first (the value at the step's start is what it read before).

## 9. Demo

`scenes/Tests/Simulation/liquid_cloth_wave.RISEscene`, `scripts/simulation_m7e_cloth.sh [resolution]` (scene and caches
under `~/.cache/rise-m7e`, frames to `rendered/simulation_m7/cloth_<res>*.png`). M7d's flume and dam-break column, with
the rubber mat replaced by cotton: `material_density 0.2`, 0.3 × 0.45 m, 13 × 19 vertices (edges 2.5 cm, 1.6 cells at 64
cells), `sheet cloth port coupling implicit`, five connections, `owned pool cloth`, macro step 1/120 s. The bake took 75 s
on 16 threads for 61 frames (both members preview); six frames (t = 0, 0.4, …, 2.0 s) path traced at 640 × 400, 16 samples.

What the frames look like (looked at; the water is drawn as a block without tank walls, as in M7b and M7d):

- t = 0.4: the front has run under the cloth and up the far wall; the cloth hangs straight, its lower edge slightly
  displaced.
- t = 0.8: the water fills the flume to the top; the cloth is submerged and bowed in its lower part.
- t = 1.2: the returning surge lifts the cloth upstream to about 45°, lying in the surface, its lower part folded into
  the water.
- t = 1.6: it hangs nearly vertical again, gently curved.
- t = 2.0: swung downstream and bent, in sloshing water.

It moves as a light sheet carried by the flow: it follows the water rather than resisting it, which is what cotton in
a surge does and what the rubber mat of M7d did not. No step-to-step jitter is visible in the frames, and no water
appears where the cloth blocks it. There is no visible wrinkling: 13 × 19 vertices cannot show folds finer than about
5 cm, and the cloth is smooth. Penetration of the cloth through itself is not prevented (no self-collision configured)
and was not seen. The splash along the flume's ceiling (water clinging to the closed top) is the liquid solver's, as in
M7d. Not looked at: frames between the six, a closer camera, a cloth dropped onto a pool (it would sink, §5.5, and it
holds water on one side under gravity, the configuration of §5.3).

## 10. Tests (`LiquidClothCouplingTest`, 12 s)

| | Check |
|---|---|
| A | The coupled operator with a coupled sheet (2-D, 24 unknowns, 12 sheet faces): max\|A − Aᵀ\|/max\|A\| = 1.6e-16; Cholesky succeeds |
| B | Still-water flap, 0.2 kg/m², 24 cells, 1/30 s, 1 s: implicit stable (in water 0.076 m/s, roughness 0.21, gated at 1.25×); staggered blows up (step 3) |
| C | Plate added mass at σ 0.05 and 10, 24 cells: within 3% of each other |
| D | Piston: a = p0/(σ + ρd) within 1% at σ 0.2 and 50; momentum audit within 5e-3 |
| E | Membrane frequency (32 cells, no gravity) within 6.6% of the potential-flow reference |
| F | The cloth's momentum change = H m g + the liquid's step impulses, within 1e-4 of H m g |
| G | Twins; liquid 1/3/8 threads with cloth 1/2/4; a group checkpoint after three macro steps resumes: byte-identical frames of both members |
| H | A macro step one digit short of 1/120 costs what the exact one does (no slivers); a staggered sheet with the cloth first in Gauss–Seidel publishes the bytes of the Jacobi order |

The sliver guard on the M7d demo scene reduced to 32 cells and 13 frames, with `macro_step 0.00833333333333333`: m7e.1
baked it in 4.4 s (4.9 s with the exact value); m7d.1 had not finished after more than 5 minutes and was stopped.

**Test runs.** `--filter 'Liquid*,Simulation*,Cloth*'`: 29 of 29 pass. The non-fire fast tier (`--filter
'[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 451 built warning-free, 450 pass; `CstDeriveGoldenTest` fails only on the STALE
line of the untracked new scene (148 MATCH, 0 DRIFT). Fire tests were not run.
| I | Refusals: relaxation with implicit, viscosity with sheets (message), an implicit sheet outside a group; an implicit sheet is preview with five owned ports |

## 11. Open, and not verified

- **One-sided sheets that sweep under gravity leak** (§5.3). A sag of centimetres under a layer, or cloth lying on a
  pool, is not handled; a sheet holding liquid on one side within a cell gap is. A remedy would be sided level-set
  stencils (no advection or reinitialisation across closed faces) or a moving cut-cell treatment of the swept cells;
  neither was built.
- **Accuracy beyond the cases of §5** is not certified; the label stays preview. The membrane frequency's −4.4% is the
  cloth model's own and was not analysed; the static floor of −0.76% was not isolated.
- **The cloth's step within a macro step is linear** (re-linearised at every macro step): large rotations within one
  macro step are integrated with linear elasticity (spurious stretch of order θ²). With 1/120 s macro steps this was not
  visible; with 1/30 s and fast flow it was not measured.
- **Sub-cell cloth modes** have no added mass (§4); fine cloth on a coarse grid moves too freely at the cloth's scale.
- **The dam break at 24 cells** peaks at 7–9 m/s for light flaps (§4), not explained.
- **Momentum:** no 3-D audit with walls and gravity; the tangential staircase impulse is not measured (§6).
- **Sheets and bodies on the same face**, sheets with viscosity (refused), cloth with self-collision in water, and moving
  pins with an implicit sheet (the copy holds a pin's accepted velocity over the macro step) were not tested.
- **The exchange needs `surface_load_steps`**; without it the cloth uses the mean force over its own substeps, which is
  the unstable configuration of §2.1. Nothing refuses it (a solver cannot see whether its offer is connected).
- **Gauss–Seidel with the cloth first** is now served correctly for staggered sheets (`startOfStep`); for a cached cloth
  member in a group the cache's rule stands (the step's end in Gauss–Seidel), unchanged.
- **Not run:** the VS2022, Android and Xcode builds (no source file added, so no list changed); the GUI; 128³ or larger
  with a cloth; the full 2 s flap at 128 cells.
- **Not verified:** Robinson-Mosher et al. 2008 and Guendelman, Selle, Losasso and Fedkiw 2005 themselves (the formulation is
  derived here); bitwise reproducibility across platforms (per platform, as M7a).

## 12. Pins and change log

- `liquid.mac` `m7d.1` → `m7e.1`: `LiquidRigidTest`, `LiquidSheetTest` version checks. `cloth.shell` stays `m3a.4` (no
  output byte or parameter changes; new ports and a channel only).
- `ClothSolverTest`: the cloth declares five ports, and `surface_load` three channels (`force` first).
- `tests/data/cst_derive_golden.txt`: one hand-added line for `liquid_cloth_wave.RISEscene` (2512 bytes, 41 lines),
  from `--dump` with the `Job::` lines stripped (the method reproduces `liquid_cloth_flap`'s committed line exactly);
  STALE until the scene is tracked.
- `tests/test_schedule.tsv`: `LiquidClothCouplingTest wide:4`, 12 s.
- Unchanged: keyword count 191 (no new chunk; new `sheet` options are text), `RewireConnectionTest`, `IJob`, the build
  lists.
- Contract, Appendix C (2026-11-26): `PortDeclaration::startOfStep` and the group's serving rule; macro ends snapped to
  output times; `cloth.shell`'s new ports and channel; `liquid.mac` m7e.1's solver-side additions.
- Docs: this file, `SIMULATION_IN_RISE_PROPOSAL.md` status, `SIMULATION_M7_DESIGN.md` §2 and §7, `Parsers/README.md`.
