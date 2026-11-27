# M7b results: rigid bodies in the liquid's pressure solve (`liquid.mac` m7b.1)

2026-11-26. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §2 (why a port exchange alone is not enough),
§4 case 9, §7 step 4. Builds on [M7a](SIMULATION_M7A_RESULTS.md) (§9, the `IPressureCoupling` seam; §13, the m7a.2
fixes), the [M7 prerequisites](SIMULATION_M7_PREREQ_RESULTS.md) §5 (mass properties over a port, the owned implicit
exchange) and [M6](SIMULATION_M6_RESULTS.md) (group host, `rigid.bodies`, `transform_source`). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), three Appendix C rows dated 2026-11-26.

**Status.** Rigid bodies (sphere, box, capped cylinder) move with the liquid. A body is *dynamic* (the liquid
integrates it), *prescribed* (pose and twist given as functions of time) or *port* (it follows `rigid.bodies` in a
`simulation_group`). Each step rebuilds the bodies' cut cells from their poses, and every projection solves the
six degrees of freedom of each coupled body with the pressure in one symmetric positive-definite system, derived
below from the variational principle (Batty, Bertails and Bridson 2007 §4 in form). In a group the liquid owns the
exchange (`owned pool drop`): it reads each body's state and mass properties at the macro step's start and offers
the hydrodynamic force and torque that, held by `rigid.bodies`, reproduce its impulse; `rigid.bodies` integrates
and owns contact. The bodies render by `transform_source`.

Evidence: the added mass of impulsively started and oscillating bodies in concentric containers (exact potential
flow), Archimedes equilibrium at density ratios 0.1, 0.5 and 0.9 (second order), a neutral body at rest to the solver
tolerance, heavy bodies' initial acceleration, a floating body's stability and momentum audit, and the bobbing
frequency of a box against an independent linear potential-flow eigen-solve. The light density ratios are stable
in the coupled solve and diverge with the staggered exchange (§5). Bakes without bodies publish frames
byte-identical to m7a.2 (§9).

The label stays `preview` for: contact between bodies or with walls inside the liquid, gaps below about two cells,
and bodies smaller than about four cells (§7). The velocity beside cut cells stays first order (as for static solids,
M7a §13.4).

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidBodies.{h,cpp}` (new) | Body shapes (exact signed distances), `LiquidBody` (state, motion), `BodyGeometry` (the moving cut cells: node distances, control-volume face fractions, cell fractions, body faces, enclosed cells, the solid face velocities), `BodyCoupling` (J, `ApplyAdd`, the impulse) |
| `src/Library/Simulation/Liquid/LiquidFlow.{h,cpp}` | `IPressureCoupling::Begin(active)`; `ActiveCells`; the assembly factored out of `Project` (`Assemble`, same arithmetic); `ApplyPressureOperator` (tests) |
| `src/Library/Simulation/Liquid/LiquidGrid.{h,cpp}` | `FaceFraction`, `CellOpenFraction` (one face / cell, as before); `Shape::invert` (validation containers) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Bodies in the step (§2.4), the extension of the level set into bodies, momentum audit helpers (`FluidMomentum`, `FluidMass`, `wallImpulse`) |
| `src/Library/Simulation/Liquid/LiquidValidation.{h,cpp}` | Cases 9a–9g (§4), shared by the tests and the evidence |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | `bodies` parameter, `bodies` output, ports, group exchange, wall contact, checkpoints, body evidence, label; chunk field `body` |
| `src/Library/Simulation/Rigid/RigidSolver.cpp` | `body_mass_properties` gains `external_force` (m g) |
| `src/Library/Simulation/SimulationGroup.cpp`, `SceneSimulation.cpp`, `Parsers/ChunkParserRegistry.cpp`, `Parsers/README.md` | The owned-exchange label rule (§6.3); `simulation_group` field `owned` |
| `tests/LiquidRigidTest.cpp` (new) | Operator symmetry and definiteness, description and label, determinism, stop/restore, rollback, group (owned and staggered), momentum exchange, group checkpoint (7.5 s) |
| `tests/LiquidRigidValidationTest.cpp` (new) | The validation table of §4 (fast tier 35 s; `--extended` 103 s) |
| `scenes/Tests/Simulation/liquid_rigid_bodies.RISEscene`, `scripts/simulation_m7b_bodies.sh` | Demo (§8) |
| Build lists | `LiquidBodies` in all five: make `Filelist` and both strict object lists, Android `CMakeLists.txt` (strict properties) and `rise_sources.cmake`, VS2022 vcxproj (Strict) and filters, the three Xcode Sources phases (`-fno-fast-math -ffp-contract=off`) and the Headers phase |

## 2. The coupled pressure solve

### 2.1 Derivation (from the variational principle)

The paper was not consulted in this pass; the formulation below is derived here from the kinetic-energy
minimisation that m7a's projection already is (M7a §13.2), and it has the form of Batty et al. 2007 §4. Compared with the
paper afterwards (2026-11-26): the same system, ΔtJᵀM⁻¹J added to the matrix, with volume (control-volume) weights beside
bodies; differences are listed in SIMULATION_M7_REFERENCES.md §7.

m7a's projection minimises ρh³/2 Σ_f A_f θ_f (u_f − u*_f)² (A_f the open fraction, θ_f = 1 between liquid cells,
the ghost-fluid fraction at the surface) subject to the discrete divergence of every liquid cell c,

    D_c = Σ_{f of c} s_cf [A_f u_f + (1 − A_f) u_s,f] = 0,      s_cf = ±1 (outward),

with the solid's velocity u_s on the closed part of each face. Its multiplier is the solver's scaled pressure
q = p dt/(ρh): u_f = u*_f + Σ_c s_cf q_c, and eliminating u gives (A q)_c = Σ_f A_f (q_c − q_nb) = b_c.

A rigid body with generalised velocity w = (V, Ω) moves the closed part of a face f of axis a (centre x_f) at
u_s,f = e_a·(V + Ω × (x_f − X)) = g_f·w, g_f = (e_a, (x_f − X) × e_a). Adding its kinetic energy
½ (w − w*)ᵀ M (w − w*), M = diag(m 1, R I Rᵀ), to the minimisation, the stationarity conditions are

    u_f = u*_f + Σ_c s_cf q_c,        w = w* + ρh³ M⁻¹ J q,        J_c = Σ_{f of c, closed by the body} s_cf (1 − A_f) g_f,

and substituting both into D_c = 0:

    (A + ρh³ Jᵀ M⁻¹ J) q = b,        b_c = −Σ_f s_cf [A_f u*_f + (1 − A_f) g_f·w*].

- The body term is symmetric positive semidefinite of rank ≤ 6 per body; with a free surface the system is
  positive definite (tested on dense matrices, §3).
- b already contains Jᵀw* through the solid face velocities, the seam m7a left for this (M7a §9).
- The impulse on the body is ρh³ J q. For a constant pressure p_c in a cell, ρh³ J_c q_c = p_c h² Σ s (1 − A) (e_a,
  r × e_a) is exactly −∫ p n dA (and its moment) over the solid surface inside the cell (divergence theorem on the
  solid part of the cell). So the liquid's impulse on the body is the negative of the body's on the liquid: the
  projection conserves the discrete momentum ρh³ Σ A θ u + M w except for the walls (§6).
- Hydrostatics: with p linear (the ghost fluid makes it exact) and a body whose mass equals ρ times the volume its
  closed y-faces give (V_y = h³ Σ_{y faces} (1 − A)), q hydrostatic, u = 0 and w = 0 satisfy every condition, so a
  discretely neutral body stays at rest to the solver tolerance (9e).
- Locked degrees of freedom have infinite mass (their rows of M⁻¹ are zero).

### 2.2 Geometry of a moving body

- **Node distances.** The static solids' node distances and fractions are computed once. Each step, in a box around
  each body's old and new pose, the node distance is min(static, body) with the body's exact distance in its frame.
- **Face fractions: control volumes, not areas (measured).** m7a uses the open *area* of each face (Ng, Min and
  Gibou). A face parallel to a flat body side is then open or closed outright, so a flat bottom crossing a face
  plane changed the buoyancy by a jump of ρgh per unit area: a staircase in the body's height. A 2-D box bobbing
  with an amplitude of about a cell oscillated in that staircase (frequencies between 4.3 and 9.2 rad/s at 32, 64
  and 128 cells, no convergence, against 5.46). Faces beside a body now take the open fraction of their *control volume* (the cell of side h
  centred on the face), Batty et al.'s weight, sampled at 4³ points with clamp(½ + d/h_s, 0, 1) of the exact
  distance; it varies continuously with the pose. With it the same box gives 5.49, 5.47, 5.45 rad/s (2 cm release),
  Archimedes converges at second order (9d; with area fractions the 3-D ratio-0.9 offsets were 8.2e-2, 3.7e-2,
  1.2e-2 at 24, 32, 48 cells), and the 3-D added mass converges at order 2.0–2.3 (9a). Static solids keep area
  fractions, so bakes without bodies are unchanged.
- **Cell fractions** (the volume correction's measure) beside a body use the same 4³ sampling: continuous in the
  pose.
- **Attribution.** The closed part of a face belongs to the solid with the smallest distance at its centre (a body,
  or the static solids and walls); a body's J and solid velocities use its faces only.

### 2.3 The level set inside a body (measured)

Cells a body encloses (every face closed) take no part in the pressure solve, but the level-set stencils read them
and a moving body carries their values into the cut cells beside it. Left as advected, they did not stay a
continuation of the liquid: under a bobbing ratio-0.5 cylinder the cut cell's level set rose from −4.4 h to −0.9 h in
40 steps and air values appeared inside the body, and over 30 s the bobbing grew (×2.6 at ratio 0.1, ×4.5 at 0.5).
With the volume correction off the same runs decayed: the correction was acting on the corrupted cut cells. Enclosed cells
now take the liquid's level set extended along the body's normal from 1.5 h outside it: φ_o + (x − x_o)·∇φ_o, the
correction limited to |x − x_o| (exact for a plane, so a floating body at rest is undisturbed). With it the column
stays planar and the 30 s runs decay (§5). Marker particles inside a body are
removed each step.

### 2.4 Time integration

One step of m7a (M7a §2) with the bodies inserted:

1. After the interface advection, each body moves to its pose at the step's end: dynamic and coupled port bodies
   drift with their step-start velocity (x += dt v, the torque-free rotation of rigid.bodies), as the interface
   does; prescribed bodies take their pose at t + dt; kinematic (staggered) port bodies move with their port
   velocity. The cut cells are rebuilt there and the level set extended into the enclosed cells.
2. Stage 1: predicted body velocities w*₁ = w + dt F_ext/m (free axes); projection with the coupling; impulse I₁.
3. Stage 2 (Heun): w*₂ = w*₁ + M⁻¹I₁/2; projection; I₂; w_new = w*₂ + M⁻¹ I₂. The step's impulse is I₁/2 + I₂, the
   impulse of the step's pressure q₁/2 + q₂.

For the linearised body–surface system this is symplectic Euler, like the liquid alone. Kinematic bodies are
boundary conditions only (their impulse is measured with the same J). Bodies the liquid integrates stop
inelastically at the domain walls (rigid.bodies' plane contact, restitution 0, friction 0.3; counted in the progress
detail; preview).

### 2.5 In a group (owned implicit exchange)

- Ports (only with port bodies): `body_state` and `body_mass_properties` accepted, `body_load` offered, channel lists
  equal to rigid.bodies'. `ownedImplicitPorts` lists all three when every port body is `coupling implicit`.
- At a macro step's start the liquid accepts each body's pose, velocity and angular velocity, and its mass,
  principal moments and external force (`external_force`, new: m g). At t = 0 the pose must equal the liquid's own
  (refused otherwise).
- At the end it offers, over body_state's whole body set, force = (liquid's impulse)/H and torque = (angular
  impulse)/H about the centre of mass (no `point` channel, so rigid.bodies applies the torque as given). rigid.bodies
  holds the load over the step: its velocity changes by exactly H (F/m + g) = I/m + gH, the liquid's own update.
  The load travels as binary32 (field.v1).
- rigid.bodies integrates the position by kick–drift–kick (trapezoidal on velocity); the liquid's copy drifted
  with symplectic Euler substeps. At the next macro start the liquid takes rigid.bodies' pose: a jump of order
  dt·Δv per macro step, not accumulated. Measured: a 2-D floating cylinder in the group differs from the liquid's own
  dynamic body by 3.2e-4 m and 5.6e-4 m at ratios 0.1 and 0.5 (2–4% of a 1.5 cm oscillation) over 1.5 s.
- Contact is rigid.bodies' (planes). Inside the macro step the liquid stops its copy at the domain walls so that it
  does not squeeze the liquid between the body and a wall: without that, a sphere of density 2500 hitting the tank
  floor at 1.7 m/s (48 cells) received the squeeze impulse and the contact impulse together and bounced back at
  1.6 m/s with restitution 0.1; with it, it stops.
- `coupling staggered`: the body is kinematic in the liquid (port velocity), and the measured impulse is offered
  the same way: the classic partitioned scheme, kept to show its failure (§5).

## 3. Properties tested (`LiquidRigidTest`, 7.5 s)

| Check | Result |
|---|---|
| Dense coupled operator, 2-D (120 unknowns, 22 cells in J) and 3-D (499, 63), a tilted light body: max \|A − Aᵀ\| / max \|A\| | 1.1e-17, 7.9e-18 |
| The body term equals ρh³ Jᵀ M⁻¹ J of the assembled J | 2.2e-17, 6.3e-17 (relative) |
| Cholesky (positive definite) | succeeds; smallest pivots 0.27, 4.5e-3 |
| 1, 3 and 8 threads, a dynamic body and a prescribed spinning, oscillating box, particles on | identical bytes |
| Stop mid-segment and restore (in memory, streamed), rollback | identical to the uninterrupted run; a checkpoint without the bodies is refused |
| Group, owned implicit, ratios 0.1 and 0.5 (2-D, 32 cells, 1.5 s) | stable (max excursion 0.95–0.98 of the release); labels predictive; within 3.2e-4 / 5.6e-4 m of the liquid's own body |
| Group momentum exchange: rigid.bodies' m Δv − H (F − m g) per macro step | 1.6e-16, 2.9e-16 of m g H (exact for the binary32 load) |
| Group, staggered, ratio 0.5 | diverges (max \|v\| 2.16 m/s within 1 s against 0.073); preview |
| Group checkpoint (both members' streams) | resumes byte for byte |
| Refusals | unknown shape, negative radius, density and mass, empty name, zero quaternion, negative frequency, unknown coupling, bad `free`, unknown key, duplicate names, a port body outside a group, a rigid body that does not start where the liquid has it |
| Label | preview for a body below four cells and for one within two cells of a wall; ten body evidence cases pass |

## 4. Validation (`LiquidRigidValidationTest`; case 9 and the prescribed-motion reference)

Tolerances are 1.25 × the error observed at that level with the default numerics, rounded up; orders must reach
1.8 (added mass, 3-D) or 1.7 (Archimedes). 8-thread pool. All rows pass.

### 4.1 References (derived here)

- **Added mass in a concentric container.** Potential flow around a body of radius a moving at U inside a fixed
  container of radius b. 2-D (cylinder): φ = (C r + D/r) cos θ with ∂φ/∂r = U cos θ at a and 0 at b gives
  C = −Ua²/(b² − a²), D = −Ua²b²/(b² − a²), and the kinetic energy −½ρ∮_a φ ∂φ/∂r dS = ½ρU²πa²(b² + a²)/(b² − a²), so
  m_a = ρπa²(b² + a²)/(b² − a²) per unit length. 3-D (sphere): φ = (C r + D/r²) cos θ, C = −Ua³/(b³ − a³),
  D = −Ua³b³/(2(b³ − a³)), m_a = (ρV/2)(b³ + 2a³)/(b³ − a³). The finite container is part of the formula (here
  +33% and +11% over the unbounded values), so no tank correction needs bounding. The container is a static
  cut-cell solid (the complement of a circle or sphere, radius 0.4 around a body of radius 0.15 in a unit box).
  An impulsive start from rest gives the body the impulse −m_a V in the single projection; an oscillation
  x = X sin ωt gives the force m_a X ω² sin ωt plus terms O((X/b)²) (the added mass of an off-centre body varies
  quadratically by symmetry).
- **Heavy body released at rest** in the same container (full of liquid, gravity on): initial acceleration
  (m − ρV) g/(m + m_a), exact.
- **Archimedes.** Submerged depth d of a sphere: πd²(3R − d)/3 = ratio·4πR³/3; of a horizontal cylinder:
  R² acos(1 − d/R) − (R − d)√(2Rd − d²) = ratio·πR² (bisection). The discrete equilibrium is the root of the net
  force on the body held at rest in hydrostatic liquid (secant, one projection per evaluation), the liquid level
  fixed at 0.5.
- **Bobbing.** A 2-D box (width w = 0.6, draft d = 0.2, density ratio 0.5) floating in a tank of width 1 and still
  depth 0.5, leaving two gaps of 0.2. No closed form: the surface-piercing body's added mass depends on the
  frequency. The reference is the lowest symmetric natural frequency of the *linear* potential-flow problem,
  computed by a separate, independent discretisation in `LiquidValidation.cpp`: for each ω, Laplace on a
  cell-centred 5-point grid of the half tank (the box edges on grid lines; Neumann walls and box sides; unit
  heave velocity on the box bottom; φ_y = (ω²/g) φ on the gap surfaces), solved by MINRES (the system is symmetric
  indefinite); ω is the root of ω²(m + ρA(ω)) = ρ g w, A the integral of φ over the box bottom (secant). Derivation
  of that condition: with body velocity V sin ωt and potential φ sin ωt, the bottom pressure −ρΦ_t = −ρωφ cos ωt
  and the hydrostatic restoring ρ g w Z balance m Z̈. Its own convergence (80, 160, 320 cells across the half tank:
  5.4546, 5.4579, 5.4591 rad/s) is order 1.34, as the re-entrant box corners predict (r^{2/3}); the Richardson
  value 5.45998 rad/s is used, uncertain by about 1e-4 relative. The long-wave (manometer) model, plug flow in the
  gaps and lubrication flow under the box, is 17% high: it is not a usable reference for gaps this wide.

### 4.2 Table

| # | Case | Reference | Levels | Error | Tolerance | Order | Result |
|---|---|---|---|---|---|---|---|
| 9a | impulsive start, cylinder, rms over 6 grid offsets (|F_y|/|F_x| ≤ 8.5e-4) | ρπa²(b²+a²)/(b²−a²) | 32/64/128/256/512 | 1.24e-2, 2.57e-3, 2.35e-4, 1.11e-4, 1.05e-4 | 1.6e-2, 3.3e-3, 3.0e-4, 1.4e-4, 1.4e-4 | 2.27, 3.45, then a floor of 1e-4 | pass |
| 9a | impulsive start, sphere, rms over 6 offsets | (ρV/2)(b³+2a³)/(b³−a³) | 24/32/48/64/96 | 4.45e-2, 2.49e-2, 1.06e-2, 5.74e-3, 2.25e-3 | 5.6e-2 … 2.9e-3 | 2.02, 2.10, 2.14, 2.31 (≥ 1.8) | pass |
| 9b | prescribed oscillation, X = 0.01, 2 Hz (moving cut cells): sine coefficient / (m_a X ω²) − 1 | m_a of 9a | 32/64/128/256 | −1.20e-2, −2.67e-3, −3.21e-4, +5.36e-4 | 1.5e-2, 3.4e-3, 4.1e-4, 6.8e-4 | 2.17, 3.05, then the reference's floor (O((X/b)²) ≈ 6e-4) | pass |
| 9b | the force's part in phase with the velocity (cosine / sine coefficient) | 0 | 32/64/128/256 | 2.2e-2, 1.1e-2, 7.5e-3, 5.1e-3 | — | ≈ 0.6–1 (first order) | report |
| 9c | heavy cylinder, ratio 2.0, initial acceleration | (m−ρV)g/(m+m_a) | 32/64/128 | 5.70e-3, 1.18e-3, 4.0e-5 | 7.2e-3, 1.5e-3, 5.1e-5 | 2.3, 4.9 | pass |
| 9c | ratio 5.0 | same | 32/64/128 | 3.14e-3, 6.03e-4, 3.2e-5 | 4.0e-3, 7.6e-4, 4.1e-5 | 2.4, 4.2 | pass |
| 9c | heavy sphere, ratio 2.0 | same | 24/32/48 | 1.02e-2, 5.37e-3, 1.96e-3 | 1.3e-2, 6.8e-3, 2.5e-3 | 2.2, 2.5 | pass |
| 9c | the drop in 0.1 s against a constant-added-mass model (reported: the added mass grows off-centre) | ½ a₀ t² | 32/64/128 | −4.8e-2, −2.7e-2, −1.6e-2 (cylinder 2.0) | — | — | report |
| 9d | Archimedes, sphere, ratio 0.1, (y* − y)/R, rms over 3 offsets | Archimedes | 24/32/48/64 | 2.01e-2, 1.11e-2, 4.85e-3, 2.70e-3 | 2.6e-2 … 3.4e-3 | 2.07, 2.04, 2.03 | pass |
| 9d | sphere, ratio 0.5 | same | 24/32/48/64 | 7.3e-4, 5.1e-4, 2.0e-4, 1.2e-4 | 1.1e-3 … 1.6e-4 | (near zero; symmetric) | pass |
| 9d | sphere, ratio 0.9 | same | 24/32/48/64 | 1.77e-2, 9.47e-3, 4.18e-3, 2.41e-3 | 2.3e-2 … 3.1e-3 | 2.18, 2.01, 1.92 | pass |
| 9d | cylinder (2-D), ratios 0.1 / 0.5 / 0.9 | same | 32/64/128/256 | 7.5e-3, 1.8e-3, 4.6e-4, 1.2e-4 / 2.9e-4 … 1.5e-5 / 7.1e-3, 1.8e-3, 4.4e-4, 9.3e-5 | 1.25 × | 2.03, 1.99, 2.01 / — / 2.00, 1.99, 2.25 | pass |
| 9e | neutral body (mass ρV_y), free in all degrees of freedom, still liquid: max body and liquid speed / √(gh) | 0 | 2-D 32, 1 s; 3-D 24, 0.5 s | 2.9e-11; 5.1e-10 | 1e-7 | exact | pass |
| 9e | the same with the mass ρV: drift (the discrete buoyancy error V_y/V − 1 = +5.6e-4, +9.0e-4) | — | same | +2.0e-4 g, +5.9e-4 g | — | — | report |
| 9f | floating cylinder, released 0.1 R above equilibrium, heave only, 3 s: max excursion from the mean / release | ≤ 1 (no growth) | 32/64, ratios 0.1, 0.5, 0.9 | 1.000 at every level and ratio (the release is the largest) | 1 | — | pass |
| 9f | second half: max excursion / release | — | 32/64 | 0.60, 0.66 / 0.84, 0.89 / 0.88, 0.88 | — | — | report |
| 9f | body impulse audit: \|m Δv − m g T − Σ impulses\| / (m g T) | 0 | 32/64 | ≤ 7e-16 | 1e-12 | round-off | pass |
| 9f | liquid + body momentum audit / ((M + m) g T) | 0 | 32/64 | 1.7e-6, 1.5e-5 / 2.1e-5, 9.9e-6 / 6.9e-6, 4.0e-6 | 1.25 × | — | pass |
| 9f | mean position − Archimedes (with the level corrected for the release) | 0 | 32/64 | +4.4e-3, −1.8e-6 / +2.8e-4, +6.2e-5 / −7.9e-3, −3.4e-3 R | — | irregular | report |
| 9f | sphere, ratio 0.5, 3-D, 1.5 s (`--extended`; audit 6.8e-6) | ≤ 1.25 | 32 | 1.001 | 1.25 | — | pass |
| 9g | bobbing frequency / reference − 1 (1 cm release, 5% of the draft) | FD linear potential flow, 5.45998 rad/s | 32/64/128 | +9.67e-3, +3.86e-3, −1.1e-4 | 1.3e-2, 4.9e-3, 1.4e-4 | 1.3, then within the amplitude effect | pass |
| 9g | the same, 0.5 cm / 2 cm releases (reported) | same | 32/64/128 | +8.8e-3, +3.1e-3, +5.8e-4 / +5.3e-3, +1.3e-3, −2.3e-3 | — | — | report |
| 9g | manometer model | same | — | +1.71e-1 | — | — | report |

Notes:

- **2-D added mass floor.** Over six offsets the cylinder converges to a floor of about 1.0e-4 (mean +1.05e-4 at
  512 cells). Its cause was not isolated; candidates are the static container's area fractions beside the body's
  control-volume fractions, and the 4³ sampling of the fractions.
- **The oscillation's in-phase force** (9b, second row) is a numerical exchange of energy between the moving cut
  cells and the liquid, first order (2.2e-2 of the force amplitude at 32 cells, 5.1e-3 at 256).
- **Bobbing.** At 128 cells the frequency moves by about 0.14% per cm of release (2 cm: −2.3e-3, 1 cm: −1.1e-4,
  0.5 cm: +5.8e-4): a nonlinear effect of the same size as the remaining discretisation error, so the 128-cell row
  shows agreement within the amplitude effect, not an order. Between 32 and 64 cells the error falls at order 1.3.
  The oscillation decays by 0.81–0.89 over four periods at 2 cm (numerical damping and the gap surfaces' sloshing).
- **First order beside cut cells.** The velocity on faces beside cut cells is first order (M7a §13.4); the forces
  above integrate the pressure, which converges at second order.

## 5. Stability at light density ratios

The same 2-D floating cylinder (32 cells, released 0.1 R above equilibrium), `liquid.mac` + `rigid.bodies` in a
group, macro step 1/30 s:

| Ratio | Owned implicit (the coupled pressure solve) | Staggered exchange (lagged force, Gauss–Seidel) |
|---|---|---|
| 0.1 | stable: max excursion 0.95 of the release, max \|v\| 0.097 m/s over 1.5 s (0.094 m/s in the first second) | the body jumps out of the water: 1.5 R above equilibrium at 0.8 s (its bottom 18 cm above the surface), velocities ±1.5 m/s |
| 0.5 | stable: 0.98, max \|v\| 0.073 m/s | the velocity alternates in sign each macro step and grows ×1.5–1.7 per step: −0.17, +0.40, −0.59, +1.00, −1.65, +2.16 m/s at 0.5–1.0 s |

The staggered exchange's failure is the added-mass instability (design §2): its amplification is the ratio of the
added mass to the body's mass, independent of the step. In the coupled solve the liquid alone (the same cylinder,
heave only) runs 30 s (34 periods) at ratios 0.1 and 0.5 without growth: the half-range of the height falls from
0.061 R to 0.007 R (0.1) and from 0.067 R to 0.006 R (0.5), about 7% per period at 32 cells. That damping is numerical
(the liquid is inviscid; the waves the body radiates reflect in the closed tank), and it is larger than the liquid's
own wave damping (M7a: 2e-4 per radian); it is not part of the label. `LiquidRigidTest` E gates the 0.5 divergence
and the stability of both owned runs.

## 6. Momentum audit

1. **The liquid's impulse on a body equals the body's momentum change.** Standalone, m Δv − m g T equals the sum of
   the liquid's step impulses ρh³ J (q₁/2 + q₂) to round-off (≤ 7e-16 of m g T, 9f), by construction of §2.4. In the
   group, rigid.bodies' momentum change per macro step equals H (F − m g) with the offered binary32 force to
   round-off (1.6e-16 and 2.9e-16 of m g H, `LiquidRigidTest` E); the binary32 rounding of F against the liquid's
   binary64 impulse is a relative 6e-8.
2. **Liquid + body against gravity and the walls.** The discrete momentum the projection conserves is
   P = ρh³ Σ_f A_f θ_f u_f (θ = 1 between liquid cells, the ghost-fluid fraction at the surface) plus M w. Over a run,
   ΔP_liquid + ΔP_body − (gravity's impulse on the liquid's face weights and on the body) − (the walls' impulse,
   ρh³ Σ q (1 − A) over the faces closed by walls and static solids) is 1.7e-6 to 2.1e-5 of gravity's impulse on
   liquid and body over 3 s (9f), and 1e-4 to 1e-3 of the impulse on the body alone. The residual is the
   non-conservative momentum advection (WENO5 on the faces), the extrapolation, the change of the face weights as
   the surface moves, and the level set's volume correction; the projection itself conserves it exactly (the
   telescoping in §2.1). It is gated at 1.25 × observed (9f) and in the evidence (`momentum-audit-n32`, 2.2e-6).
3. **The group label rule (fixed).** The M7 prerequisites' rule for owned exchanges did not check that every member
   in a coupled member's closure is predictive. With rigid.bodies' plane contact (preview) the liquid stayed
   predictive. It now requires it, as the other couplings do (`SimulationGroup.cpp`; Appendix C).

## 7. Evidence and the label

With bodies, Configure computes the m7a cases plus ten body cases (memoised per numerics; about 2 s more: 5.0 s at
the first Configure on 16 threads). Without bodies the evidence is m7a.2's.

| Id | Check | Tolerance | Default value |
|---|---|---|---|
| `added-mass-cylinder-n64` | 9a, one offset (0.37 cells) | 2.6e-3 | 2.03e-3 |
| `added-mass-sphere-n32` | 9a | 2.8e-2 | 2.21e-2 |
| `added-mass-oscillation-n32` | 9b | 1.5e-2 | 1.20e-2 |
| `archimedes-0.1-n32`, `-0.5-`, `-0.9-` | 9d, sphere, offset 0.37 | 1.4e-2, 5.0e-4, 1.2e-2 | 1.11e-2, 3.95e-4, 9.53e-3 |
| `neutral-rest-n32` | 9e, 2-D | 1e-7 | 2.9e-11 |
| `light-float-n32` | 9f, ratio 0.1: second-half excursion / release | 0.76 | 0.603 |
| `momentum-audit-n32` | 9f, ratio 0.1, / gravity's impulse | 2.2e-6 | 1.69e-6 |
| `body-impulse-n32` | 9f | 1e-12 | 5.5e-16 |

- Condition `bodies`: {dynamic, prescribed, port-implicit, port-staggered} (those present).
- Scope (one sentence): adds the bodies, the certified cases, and does not cover contact of a body with another body
  or with a wall inside the liquid, gaps below about two cells, bodies smaller than about four cells, and that
  velocities beside cut cells are about first order.
- `preview` with `fidelityLimit` when a body's smallest extent is below four cells, or when at the start a body is
  within two cells of a wall, a static analytic solid or another body (node sampling of its distance). Contact inside
  the liquid cannot be known at Configure; liquid-integrated bodies' wall contacts are counted in the progress detail.
- In a group with rigid.bodies: `coupling: owned-implicit`, predictive only if rigid.bodies is (no planes); the
  staggered exchange is preview.

## 8. Group integration, scene and demo

- **Parameters.** `bodies`: `{name, shape (sphere | box | cylinder), radius, size, height, motion (dynamic |
  prescribed | port), position, orientation (w, x, y, z), velocity, angular_velocity}`, dynamic `density` or `mass`
  and `free [x, y, z, rotation]`, prescribed `amplitude`, `frequency`, `phase`, port `coupling (implicit |
  staggered)`. Written back canonically; `bodies: []` without bodies.
- **Output.** `bodies` (transform.v1, sorted by name) at every output time.
- **Checkpoints.** The bodies' state, the open exchange and the wall-contact count in the stream's header (only with
  bodies); restored geometry is rebuilt from the poses (the level set is restored, not re-extended).
- **Scene.** `liquid_simulation` field `body <name> sphere r | box sx sy sz | cylinder r h [motion …] [at …] …`
  (`Parsers/README.md` "Liquid (M7)"); `simulation_group` field `owned <owner> <partner> …`.
- **Demo.** `scenes/Tests/Simulation/liquid_rigid_bodies.RISEscene`, `scripts/simulation_m7b_bodies.sh [resolution]`
  (scene and caches under `~/.cache/rise-m7b`, frames to `rendered/simulation_m7/bodies_<res>*.png`). A tank
  0.8 × 0.6 × 0.5 m filled to 0.3 m; a sphere of density 300 (r 7 cm), a cube of 700 (11 cm) and a sphere of 2500
  (r 5 cm) dropped from 15–20 cm above the water. 64 cells along x: the group bake took 72 s (61 frames); the
  render of six frames 13 s (640 × 400, 16 samples, path tracing). Label: preview (contact).
- **What the frames look like** (looked at, t = 0, 0.4, 0.8, 1.2, 1.6, 2.0 s). The water is drawn as a block (the
  surface closes on the domain walls; no tank glass is modelled), with a back wall and a floor. t = 0: the three
  bodies above a flat surface. t = 0.4: the yellow buoy at the surface, the crate under the surface, the dark
  sphere on the bottom (seen twice: directly and reflected inside the block's side face); the surface is rippled
  with a small spike above where the heavy sphere entered. t = 0.8–2.0: the buoy floats high, the crate floats with
  its top above the water and visibly tilted at 1.2 s, the heavy sphere rests on the floor and slides slowly (2.6 cm
  in 1.6 s; the liquid's flow against friction 0.5); the surface keeps moving (inviscid: the waves do not settle in
  2 s). Positions from the cache: the buoy's centre oscillates around 0.32–0.34 m (Archimedes with the risen level:
  0.324), the crate's around 0.27–0.30 (0.283). Nothing looks broken; the splash is coarse at 1.25 cm cells (no
  droplets: the level set cannot hold them).

## 9. Bakes without bodies

The step, Initialize and ProjectState take the m7a code path when there are no bodies (a null solid field, no
couplings), and the assembly moved into a function with the same arithmetic. A 24-cell dam break against a box
with fields and particles published (12 frames, 36 payloads) is byte-identical to m7a.2's (the content-addressed
frame names match), checked before and after all changes. Only the version (`m7b.1`, hence the key), the
effective parameters (`bodies: []`) and the schema's output list change.

## 10. Cost (Apple M4 Max, 16 threads)

A 3-D tank filled to half height, floating bodies (ratio 0.5, spheres and boxes of 16 cm), dynamic, particles on;
mean of 20 (64³) or 8 (128³) steps after three.

| Grid | Bodies | s/step | Geometry | Pressure | PCG iterations / step (2 solves) |
|---|---|---|---|---|---|
| 64³ | 0 | 0.065 | — | 0.037 | 11.0 |
| 64³ | 1 | 0.094 (+45%) | 0.002 | 0.061 | 20.2 |
| 64³ | 4 | 0.116 (+79%) | 0.020 | 0.065 | 22.0 |
| 64³ | 8 | 0.156 (+141%) | 0.058 | 0.068 | 23.1 |
| 128³ | 0 | 0.301 | — | 0.167 | 12.0 |
| 128³ | 1 | 0.406 (+35%) | 0.007 | 0.267 | 24.1 |
| 128³ | 4 | 0.465 (+55%) | 0.038 | 0.295 | 27.2 |
| 128³ | 8 | 0.533 (+77%) | 0.094 | 0.316 | 29.4 |

- **The preconditioner.** The same bodies held kinematic (no coupling term) take 12.0, 15.0 and 16.0 iterations per
  step at 64³ (1, 4, 8 bodies); coupled, 20–23. The rank-6 term adds about four iterations per solve, about the same
  for one body as for eight (one step at rest, by density ratio: 0.01: 11 → 15, 0.1: 11 → 13, 1.0: 21 → 27, 2.0:
  21 → 33). Not fixed: a Sherman–Morrison–Woodbury correction of the multigrid preconditioner would cost six
  V-cycles per body per solve, more than the four iterations it would save.
- **Geometry.** The rebuild (distances, 4³-sampled fractions, attribution) grows faster than linearly with the
  number of bodies when their boxes overlap (each sample checks every body whose box holds the cell): 2 ms for one
  body and 58 ms for eight at 64³. Not optimised.
- `costEstimate` is multiplied by 1.3 + 0.06 × (number of bodies) when there are bodies (the 128³ rows).

## 11. Pins and change log

- `liquid.mac` version `m7a.2` → `m7b.1` (keys of liquid caches change; frames without bodies do not).
- `SimulationOwnedCouplingTest`: the `body_mass_properties` channel list (4 channels; indices shifted) and a check of
  `external_force`; `tests/SimulationAddedMassSolver.h` declares the new channel.
- `LiquidSolverTest` A: `bodies` is an effective parameter described outside the schema's scalar list (as `liquid`
  and `solids`).
- `CstDeriveGoldenTest`: `liquid_dam_break.RISEscene` re-derived (the schema's output list gained `bodies`: 1754
  bytes, 33 lines) and a line for `liquid_rigid_bodies.RISEscene` (2870 bytes, 47 lines), both from `--dump` with
  the log lines stripped (the method reproduces `simulated_points_spray`'s line exactly). The new scene is untracked,
  so the test reports its line as STALE until it is committed.
- `SceneEditorSuggestionsTest`, `RewireConnectionTest`: unchanged (no new chunk; the new fields are text).
- `tests/test_schedule.tsv`: `LiquidRigidTest` wide:5, `LiquidRigidValidationTest` wide:3;
  `run_all_tests.{sh,ps1}`: `LiquidRigidValidationTest` is dual mode.
- `IJob`, `ChunkCategory`: unchanged.
- Contract (Appendix C, 2026-11-26): `body_mass_properties` gains `external_force`; the owned-exchange label rule;
  the `owned` group field; `liquid.mac` m7b.1's solver-side additions.

**Tests.** The fast tier with the non-fire filter of earlier passes (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`):
447 tests built warning-free, 446 pass; `CstDeriveGoldenTest` fails only on the STALE line above. A first full
fast-tier run without that filter was stopped when it reached `FireSequenceTest` (the rule is not to run the GPU fire
solver); no fire test result is reported. `LiquidRigidValidationTest --extended`: all rows pass (103 s).

## 12. Open, and not verified

- **Contact inside the liquid** (body–body, body–wall) is preview: rigid.bodies has no body–body contact (bodies
  pass through each other), and its plane contact is an impulse model. The liquid's own wall stop is a crude
  inelastic projection.
- **Thin gaps and small bodies** are outside the scope and only detected at the start.
- **Velocity beside cut cells** first order (as for static solids).
- **The 2-D added-mass floor** of 1e-4 is not explained (§4.2).
- **Closed meshes as bodies** were not built (analytic shapes only); `rigid.bodies` mesh bodies cannot be followed
  by the liquid.
- **Bobbing** is validated in 2-D only; the 128-cell agreement is limited by the amplitude dependence.
- **Rigid rotation in the coupled solve** is exercised by the operator tests, the determinism and group runs and the
  demo (the crate tilts), but no case has a rotational reference (a torque from a known flow).
- **Not run:** the VS2022, Android and Xcode builds (lists edited by script; the make build compiles everything);
  256³; the GUI.
- **Checked against the paper afterwards (2026-11-26):** Batty, Bertails and Bridson 2007 §§2–3 (M7_REFERENCES §7);
  not reproduced: the paper's §4 free-surface separation inequality (not built here). Not verified: bitwise reproducibility across platforms (per platform, as M7a).
