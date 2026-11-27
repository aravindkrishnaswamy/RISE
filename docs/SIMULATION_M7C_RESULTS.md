# M7c results: viscosity and surface tension in the liquid solver (`liquid.mac` m7c.1)

2026-11-26. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §1 (optional physics), §4 cases 3, 10 and 11,
§7 step 5. Builds on [M7a](SIMULATION_M7A_RESULTS.md) (the projection as a kinetic-energy minimisation, §13.2) and
[M7b](SIMULATION_M7B_RESULTS.md) (bodies in the pressure solve, control-volume face fractions). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), one solver-side Appendix C row (no contract member
added or changed).

**Status.** `liquid.mac` gains two options, both off by default and each certified by its own evidence:

- **`viscosity`** (constant dynamic viscosity, Pa s): an implicit step in the variational strain-rate form, derived
  here (§2.1) and of the form of Batty and Bridson 2008. The free surface is traction free as the natural boundary
  condition of the minimisation. Walls, static cut-cell solids and bodies are no-slip, or free-slip with
  `viscous_walls free_slip`. One symmetric positive-definite system over the three velocity components, solved by
  Jacobi-preconditioned conjugate gradients with fixed-order reductions. Bodies the liquid integrates are unknowns of
  the same system (implicit viscous coupling; the lagged version was built first and diverged for light bodies, §3).
  Two additions turned out to be required and are measured in §2.3: an incremental pressure (the split viscous and
  pressure steps otherwise converge to the wrong Stokes flow) and a viscous time-step limit ν dt/h² ≤ 4.
- **`surface_tension`** (N/m): the Laplace jump σκ through the ghost-fluid pressure condition, κ from the level set
  interpolated to the sub-cell interface position, and the capillary time-step limit. Contact angle: 90° at the
  domain walls in the curvature only (§2.5). With surface tension the interface velocity is extrapolated by a
  constant instead of linearly (§2.6): the linear extension drove parasitic currents to the capillary speed. That
  makes the capillary wave first order.

With both off, every published frame is byte-identical to m7b.1 (§9); the version is `m7c.1` regardless.

What stays `preview`: surface tension without viscosity (an inviscid static drop's parasitic currents are not
bounded, §5), surface tension with solids or bodies (contact lines not validated), and, as before, everything M7a and
M7b list. Variable viscosity is not built. Viscous flow in gaps under about two cells is outside the scope (not
detectable at Configure).

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidViscosity.{h,cpp}` (new) | The viscous system: face kinds, control-volume weights, the strain operator and its transpose (gather form), coupled body unknowns, Jacobi PCG, the boundary-force attribution |
| `src/Library/Simulation/Liquid/LiquidFlow.{h,cpp}` | `PressureOptions` curvature and jump (ghost-fluid Laplace jump in `Assemble` and the update); `SubtractPressureGradient` (the incremental pressure) |
| `src/Library/Simulation/Liquid/LiquidLevelSet.{h,cpp}` | `Curvature` (central differences, mirrored at the walls) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Setup `viscosity`, `noSlip`, `surfaceTension`; options; the step (§2.4); the capillary and viscous time-step limits; no-slip momentum extension; `bodyViscousImpulse`, `viscousWallImpulse` |
| `src/Library/Simulation/Liquid/LiquidValidation.{h,cpp}` | Cases 10a–10f, 11a–11c with their references |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | Parameters, evidence, conditions, scope and preview rules, records, estimates, checkpoints (the bodies' last impulses with viscosity) |
| `tests/LiquidPhysicsValidationTest.cpp` (new) | The validation table of §4 (fast tier 36 s; `--extended` 333 s) |
| `tests/LiquidSolverTest.cpp` | Section H: labels, refusals, threads, stop/restore, macro steps, rollback with both options and a body |
| `scenes/Tests/Simulation/liquid_honey_water.RISEscene`, `scripts/simulation_m7c_honey.sh` | Demo (§10) |
| Build lists | `LiquidViscosity` in all five: make `Filelist` and both strict object lists, Android `CMakeLists.txt` and `rise_sources.cmake`, VS2022 vcxproj (Strict) and filters, the three Xcode Sources phases (`-fno-fast-math -ffp-contract=off`) and the Headers phase |

## 2. The formulation as built

### 2.1 Viscosity: derivation from the variational principle

The paper was not consulted in this pass; the discretisation is derived here, and it has the form of Batty and
Bridson 2008 (compared with the paper afterwards, 2026-11-26: same energy and the same control-volume weights on
cell-centre and edge strains; differences in M7_REFERENCES §7) ("Accurate viscous free surfaces for buckling, coiling, and rotating liquids").

Backward Euler for ρ u_t = ∇·(2μ D(u)), D = ½(∇u + ∇uᵀ), is the stationary point of

    E(u) = (ρ/2) ∫ |u − u*|² dV + Δt μ ∫ D(u) : D(u) dV,

since δ ∫ D:D = 2 ∫ D : ∇δu = −2 ∫ (∇·D)·δu + 2 ∮ (D n)·δu, so δE = 0 gives ρ(u − u*) = Δt ∇·(2μD) inside and,
where the energy's domain ends at a free surface and u is not prescribed, 2μ D n = 0: zero viscous traction is the
natural boundary condition. (A plain Laplacian, μ ∫ |∇u|², has the natural condition (∇u) n = 0, which is not zero
traction: it dissipates a rigid rotation, whose strain is zero. Case 10e checks that one step leaves it unchanged.)

Discretely, on the MAC grid:

- the diagonal strains D_aa = ∂_a u_a live at cell centres (difference of the two faces of axis a);
- the shear strains D_ab = ½(∂_b u_a + ∂_a u_b) live on cell edges parallel to the third axis (two differences of
  two faces each);
- each strain carries the liquid fraction of its control volume (the cube of side h centred on it) outside the
  solids, w_c or w_e, from samples of clamp(½ − φ/h_s) clamp(½ + φ_s/h_s) (4³ samples in a cut volume, h_s = h/4;
  1 or 0 elsewhere). Samples outside the domain count as closed;
- each face carries its kinetic weight m_f, the projection's: A between liquid cells, A θ to the air (θ clamped at
  the pressure's θ_min). Faces only the strains reach get 1e-6. The two steps then measure momentum alike;
- E_h = (ρh³/2) Σ m_f (u_f − u*_f)² + Δt μ h³ [Σ_c w_c Σ_a D_aa² + 2 Σ_e w_e D_ab²].

With G the strains' dependence on the unknown faces, g₀ their part from boundary values and S = (w_c, 2 w_e), the
stationary point solves (M + 2Δtν GᵀSG) u = M u* − 2Δtν GᵀS g₀: symmetric positive definite. Matrix-free: one pass
computes the strains, a second gathers GᵀSε face by face, so it is bit-identical for any thread count (`LiquidSolverTest`
H). Preconditioner: Jacobi. Grids below 32k cells run on the calling thread (some twenty parallel loops per
iteration cost more in joins than in work).

**Boundaries.**

- *Normal velocity at the domain walls*: zero (both modes).
- *No slip (default)*: a face whose centre is inside a solid (static or a body; φ_s, the mean of its four corner
  nodes, negative) is fixed. A difference between a liquid face and a fixed one is taken to the wall crossing:
  (u_wall − u₁)/(θh) with θ = φ_s,1/(φ_s,1 − φ_s,2) clamped at 0.05 (the ghost-fluid Dirichlet condition), u_wall the
  solid's velocity at the crossing (a body's rigid velocity there). With the control-volume weight ≈ θ this
  contributes (u_wall − u₁)²/(θh), the exact energy of a linear profile over the gap. A tangential difference across a
  domain wall puts zero at the wall (θ = ½).
- *Free slip*: solid faces are ordinary unknowns. The open-fraction weights make the solid traction free for this
  step, and the pressure step keeps the normal velocity. Shear strains on the domain walls are dropped.
- *Open top*: traction free.

**Forces.** E's strain part is invariant under adding a constant to one component of every unknown and boundary
value, so the liquid's momentum change is Σ over the boundary values v of ∂E_strain/∂v. That is how the impulse of
the static solids and walls (`viscousWallImpulse`, for the momentum audit) and of kinematic bodies is computed. A
check: one viscous step on random velocities in a free disk changes Σ m Δu by 2e-8 and the angular momentum by 6e-10
of Σ |m Δu r| (round-off and the CG tolerance).

### 2.2 Bodies in the viscous solve (implicit)

A body the liquid integrates (dynamic, or a port body coupled implicitly) is an unknown of the solve. Its boundary
values are its rigid velocity at each crossing, linear in w = (V, Ω), and its kinetic energy ½(w − w*)ᵀ M (w − w*) is
added to E. Locked degrees of freedom stay at w*. The system stays symmetric positive definite. The body's rows come
from a gather over the strains that reach it, listed once per solve. Its impulse is M(w − w*), and the pressure solve
then starts from the new w. The predicted w* includes the previous step's *pressure* impulse on the body, less the
part the stage already holds: the body's counterpart of the incremental pressure (§2.3). Without that prediction the
drag of a heavy settling sphere was evaluated at a velocity about g dt off, which at low Reynolds number is the
terminal velocity itself: it fell 30–40% slow (with the lagged coupling).

### 2.3 Two things the splitting needed (measured)

**Incremental pressure.** Splitting the implicit viscous step from the projection, u = P(V(u*)), has as its steady
state the minimiser of the strain energy without the incompressibility constraint, then projected. That is not Stokes
flow once ν dt/h² is large. The 2-D Stokes drag on a cylinder in a concentric container (case 10d, exact) came out at
26% of the exact value with ν dt/h² ≈ 30. The viscous solve now sees s − G q_prev + dt g (the previous step's pressure
gradient scaled to dt, and the step's gravity). G q_prev is added back before the projection, which then solves for
the whole pressure. This is the incremental pressure-correction scheme, and a steady Stokes flow is its fixed point.
The predictor uses faces between two liquid cells only: with the surface faces included, the Laplace jump's mismatch
divided by θ destabilised a static drop. Gravity moved into the viscous solve too, where the hydrostatic part of
G q_prev cancels it.

**Viscous time-step limit.** The fixed point is reached at a rate that falls as ν dt/h² grows. The drag after 0.3 s
(5 diffusion times) at 32 cells:

| `viscous_cfl` (dt ≤ c h²/ν) | steps | F / exact − 1 |
|---|---|---|
| none (ν dt/h² ≈ 30) | 10 | −3.07e-1 |
| 16 | 20 | −6.94e-2 |
| 4 (default) | 80 | −2.49e-2 |
| 1 | 310 | −2.00e-2 |

The default 4 costs nothing for realistic liquids at bake resolution: for honey (ν ≈ 7e-3 m²/s) it is 9 ms at 4 mm
cells and 57 ms at 1 cm, against a wave limit near 0.6 ms and 1.6 ms. It binds in the very viscous validation cases.

**No-slip momentum extension.** With no slip, closed faces and cut faces narrower than `min_transport_fraction` take
the solid's velocity in the momentum field before advection (they took the liquid's extrapolated velocity, which
transports liquid momentum into a no-slip wall). Circular Couette at Re 3.75: torque error +0.90% / +0.68% at 32 / 64
cells before, +0.75% / +0.53% after. The interface field keeps m7b's extension.

### 2.4 The step

M7a's Heun step with:

- after the volume correction, κ of the new interface (surface tension);
- stage 1 as before; its projection carries the jump at weight 1;
- stage 2: s₂ = ½uⁿ + ½B(s₁ + dt L(s₁)); with viscosity, s₂ − G(q_prev dt/dt_prev − q₁/2) + ½ dt g goes through the
  viscous solve (coupled bodies as unknowns), then G(·) is added back; without viscosity ½ dt g is added as before;
- the final projection carries the jump at weight ½ (the step's pressure is q₁/2 + q₂, so the jump sums to σκ dt).

Viscosity is applied once per step, over dt, by backward Euler: **first order in time** (10a and the Couette
start-up show it). Crank–Nicolson was not used: it does not damp the stiff modes of honey at bake steps.

### 2.5 Surface tension

- **Jump.** Kang, Fedkiw and Liu 2000's ghost fluid: on a face from liquid cell L to air cell R, q = q_I at the
  interface with q_I = (σ dt w/(ρh)) κ_I, so the ghost value is q_L + (q_I − q_L)/θ. The operator is unchanged (A/θ on
  the diagonal), the right-hand side gains A q_I/θ, and the face update is u* + (q_L − q_I)/θ. The preconditioner is
  untouched. The open top keeps p = 0.
- **Curvature.** κ = ∇·(∇φ/|∇φ|) from second-order central differences at cell centres (the 3-D formula with
  mixed derivatives), clamped to (dims − 1)/h, then interpolated to the interface: κ_I = κ_L + θ(κ_R − κ_L)
  (`curvature interpolated`, default) or the liquid cell's (`cell`). For a distance function the cell value is the
  curvature of the level set through the cell centre, 1/(R + φ) for a circle; the interpolation removes that
  first-order offset (§5: the cell variant has a 7% pressure error and 40× the parasitic currents).
- **Contact angle.** The curvature stencil mirrors φ at the domain walls (zero normal slope): 90° in the curvature
  only. The level set's advection imposes no angle, and nothing is done at static solids or bodies (φ is not
  extrapolated into them). With solids or bodies the label is preview. No specified angle was built.
- **Capillary limit.** dt ≤ `capillary_cfl` sqrt(ρh³/(2πσ)) (the form of Brackbill, Kothe and Zemach 1992,
  sqrt((ρ_a + ρ_b)h³/(4πσ)), with the 2π of Denner and van Wachem 2015 and ρ_b = 0 for the absent second phase; checked
  2026-11-26: the paper's own constant is 4π, so the default `capillary_cfl` 0.5 is 0.35 of its limit), `capillary_cfl` 0.5. Symplectic Euler on the capillary wave of the grid's
  highest wavenumber π/h, ω² = σk³/ρ, is stable for ω dt < 2: 0.90 in these units. Measured on the 2-D viscous static
  drop at 32 cells, 10 capillary times, parasitic Ca: 0.5 → 4.3e-5, 1.0 → 1.1e-4, 1.5 → 1.1e-3 (growing), 2.0 → 1.8e-2
  (unstable). 0.5 leaves a margin of about 2.

### 2.6 The interface extension with surface tension (measured)

M7a extrapolates the interface velocity linearly in the first layer (2u₁ − u₂), which made the gravity-wave frequency
second order. With surface tension it amplifies grid-scale noise (a checkerboard triples) into the curvature, a second
derivative. The 2-D inviscid static drop then grew from 3e-3 to 0.2–1 U_σ within 2 capillary times. The 2-D viscous one
(Laplace number 12000) reached Ca 1e-2. A minmod-limited linear extension fixed 2-D and kept the capillary wave second
order (2.84e-2, 7.91e-3, 2.05e-3 at 16/32/64), but the 3-D viscous drop at 32³ still grew to 1.4 U_σ. With surface
tension the interface field is therefore extended by a constant: the 3-D drop stays at Ca ≤ 6.8e-4, and the capillary
wave becomes first order (§4, 11c). Without surface tension nothing changes.

## 3. The lagged viscous body coupling (built, measured, replaced)

The first version held each body at its predicted velocity during the viscous solve and applied the measured impulse
afterwards (explicit). Settling and rising spheres at 24 cells (a = 3.6 cells, ν = 0.01 m²/s, ν dt/h² ≈ 1.6):

| Density ratio | Lagged | Implicit (built) |
|---|---|---|
| 2 (settling) | stable, +9.1% | stable, +7.7% |
| 0.5 (rising) | diverges (0.3 m/s downward within 0.25 s) | stable, +7.5% |
| 0.1 | diverges (3.3 m/s) | stable, +7.6% |
| 0.01 | — | stable, +26% at 0.08 s (not yet terminal) |

The lagged scheme's amplification is the ratio of the liquid mass it drags in one solve (≈ ρ × the body's area × the
reach of the implicit step, about 1.3 cells here) to the body's mass: 3.9 (h/a)/ratio ≈ 1.1/ratio for this sphere,
unstable above about 2. That matches the measured threshold between ratios 2 and 0.5.

## 4. Validation (`LiquidPhysicsValidationTest`)

Tolerances are 1.25 × the error observed at that level with the default numerics, rounded up; extended levels too.
8-thread pool. All rows pass (fast tier 36 s, `--extended` 333 s; the extended numbers below are from the run before
the interface-extension change of §2.6, which does not touch the viscous rows; the surface-tension rows were
re-measured after it).

### 4.1 References (derived or sourced here)

- **10a.** The Taylor–Green vortex u = (sin x cos y, −cos x sin y) in [0, π]² has D_xy = 0 everywhere, so it
  satisfies free slip, and ∇·(2D) = ∇²u = −2u: the exact Navier–Stokes solution decays as exp(−2νt).
- **10b, deep standing wave.** Lamb 1932 §349, derived here: φ = A e^{ky}, ψ = B e^{my}, m² = k² + s/ν. Zero
  tangential stress gives B = −2ik²A/(m² + k²), the kinematic condition gives H = kA/(ν(m² + k²)), and the normal
  stress gives

      (s + 2νk²)² + gk + (σ/ρ)k³ = 4ν²k³ m,

  solved by complex Newton from −2νk² + iω₀. Expanding, decay = 2νk² − (√2/2) k sqrt(ν/ω) · 2νk² + …: the leading term
  is the design note's 2νk² (verified: the irrotational dissipation 2μ∫D:D of cos(kx) cosh(k(y + d)) over a free-slip
  tank gives an energy decay of 4νk² at any depth). The boundary-layer correction is −5% at the parameters used.
  Parameters: tank 1 m, k = 2π, kd = π (e^{−2kd} = 0.002), ν = 1e-3: decay 0.07499 1/s (2νk² = 0.07896), νk²/ω =
  5.0e-3. The physical damping is 28× (32 cells) and 37× (64) the inviscid run's numerical damping (M7a: about
  2e-4 per radian). The surface boundary layer sqrt(2ν/ω) = 1.6 cm is half a cell at 32 and two at 128, so the
  O(ν^{3/2}) correction is only resolved at the finest level. The gate is on the "net" decay: measured minus the
  inviscid twin's.
- **10c.** Circular Couette between r = R₁ spinning at ω and R₂ at rest: u_θ = A r + B/r, A = −ωR₁²/(R₂² − R₁²),
  B = ωR₁²R₂²/(R₂² − R₁²). The torque on the inner cylinder per unit length is −4πμωR₁²R₂²/(R₂² − R₁²). The start-up
  reference is a fine 1-D radial solve of u_t = ν(u_rr + u_r/r − u/r²) (4000 cells, four backward-Euler quarter
  steps then Crank–Nicolson).
- **10d.** A cylinder of radius a translating at U inside a fixed concentric cylinder b (Stokes flow, no slip):
  F = 4πμU/(ln(b/a) − (b² − a²)/(b² + a²)) per unit length (Happel and Brenner). Checked here independently: the
  biharmonic stream function sinθ (A r³ + B r + C r ln r + D/r) with the four boundary conditions, and the drag from
  the dissipation integral 2μ∫D:D/U, agree with the formula to 1e-8 (a = 0.15, b = 0.4: 55.2600 μU).
- **10f.** Stokes U_s = 2(ρ_s/ρ − 1) g a²/(9ν), times the wall factor of Haberman and Sayre 1958 (DTMB report 1143)
  for a sphere on the axis of a long cylinder, K = (1 − 2.105λ + 2.0865λ³ − 1.7068λ⁵ + 0.72603λ⁶)/(1 − 0.75857λ⁵).
  The report could not be fetched (DTIC refused). Checked 2026-11-26 against two secondary sources that print the formula
  (arXiv:2607.02625 and Kagaku Kogaku 12(3):239, which write it as the inverse, the drag
  factor (1 − 0.75857λ⁵)/(1 − 2.1050λ + 2.0865λ³ − 1.7068λ⁵ + 0.72603λ⁶)); the coefficients agree digit for digit. They are
  also cross-checked against
  Bohlin's 1960 series 1 − 2.10443λ + 2.08877λ³ − 0.94813λ⁵ − 1.372λ⁶ + 3.87λ⁸ − 4.19λ¹⁰ (2.10443 as in the Kagaku Kogaku
  table, 2.10444 in arXiv:1103.4939; both secondary; both print the λ⁵ coefficient as 6.94813, a typographical error
  against Faxén's 0.95 and the agreement below, so 0.94813 stands): at
  λ = 0.3 they agree to 5e-6 (0.42200). At λ = 0.1 both give 0.7916, the commonly quoted value. How good the
  reference is:
  - Reynolds number 0.034: the Oseen correction 3Re/16 is 0.6% unbounded, less when confined;
  - the tube is 4R long, closed, and the sphere starts 1.2R from the top. Stokes disturbances decay along a tube
    roughly as exp(−4.5 z/R), so the ends contribute under 0.5%;
  - the reference is therefore good to about 1%, and the measured errors (4.5–7.7%) are the solver's.
- **11a.** Laplace: Δp = σ/R (2-D cylinder), 2σ/R (sphere). The standard parasitic-current measure is the capillary
  number Ca = μU/σ at Laplace number La = σρD/μ² = 12000 (μ = 6.45e-3 here, R = 0.25, ρ = σ = 1).
- **11b.** Lamb's drop in vacuum, derived for 2-D: φ = A rⁿ cos nθ, η = H cos nθ, κ = 1/R + (n² − 1)η/R², so
  ω² = n(n² − 1)σ/(ρR³) (n = 2: 6σ/(ρR³)). In 3-D, ω² = n(n − 1)(n + 2)σ/(ρR³) (standard; not re-derived). With
  viscosity the irrotational damping is 2n(n − 1)ν/R² in 2-D, derived here from KE = ½ρπnA²R^{2n} and dissipation
  2μ · 2πA²n²(n − 1)R^{2n−2}.
- **11c.** ω² = (gk + σk³/ρ) tanh(kd) (the deep limit of the relation above with ν = 0). Bond number 1 here
  (σk²/ρ = g); the contact angle at the tank walls is 90°, consistent with cos(kx).
- **11d, Hysing et al. 2009 rising bubble.** It is two-phase: the bubble is a second fluid with density and viscosity
  ratios 10 (case 1). A single-phase free-surface solver treats the gas as void at p = 0, so the benchmark is out of
  scope. It is dropped, and its role (surface tension in a dynamic, translating shape) is taken by the oscillating
  drop and the capillary wave.

### 4.2 Table

| # | Case | Reference | Levels | Error | Tolerance | Order | Result |
|---|---|---|---|---|---|---|---|
| 10a | viscous Taylor–Green, ν 0.05, T 2: amplitude / exp(−2νT) − 1 | exact | 16/32/64/128 | 5.60e-4, 5.26e-4, 2.71e-4, 1.35e-4 | 7.0e-4, 6.6e-4, 3.4e-4, 1.7e-4 | 0.1, 0.96, 1.00 (first order in time) | pass |
| 10b | standing wave, ν 1e-3, free slip: (decay − inviscid twin)/Lamb − 1 | Lamb's root | 16/32/64 | −1.67e-1, −6.66e-2, −2.81e-2 | 2.1e-1, 8.4e-2, 3.6e-2 | 1.33, 1.24 | pass |
| 10b | same, total decay / Lamb − 1 (numerical damping included) | | 16/32/64 | −1.11e-1, −3.07e-2, −9.7e-4 | evidence 3.9e-2 at 32 | — | report / evidence |
| 10b | no-slip walls and bottom: decay / free-slip Lamb − 1 | (wall boundary layers; no correction computed) | 32 | +7.9e-1 | — | — | report |
| 10c | circular Couette, ω 0.1 (Re 0.375): torque / exact − 1 | exact | 32/64/128 | −6.49e-3, −6.43e-4, +4.18e-3 | 8.2e-3, 8.1e-4, 5.3e-3 | irregular (±0.5%) | pass |
| 10c | steady profile, max / (ωR₁) | A r + B/r | 32/64/128 | 2.02e-2, 9.57e-3, 4.04e-3 | 2.6e-2, 1.2e-2, 5.1e-3 | 1.07, 1.25 | pass |
| 10c | start-up profile at 0.5 s | 1-D radial | 32/64/128 | 2.67e-2, 1.86e-2, 5.19e-3 | 3.4e-2, 2.4e-2, 6.5e-3 | 0.53, 1.84 | pass |
| 10c | Re 3.75 (ω 1): torque / exact − 1 | exact | 32/64 | +1.49e-2, +1.30e-2 | — | — | report |
| 10d | Stokes drag, cylinder in a concentric container, ν 1: F / exact − 1 | Happel and Brenner | 32/64/128 | −2.49e-2, −9.70e-3, −1.85e-3 | 3.2e-2, 1.3e-2, 2.4e-3 | 1.36, 2.39 | pass |
| 10e | free disk in rigid rotation: one viscous step, max \|Δu\|/(ωR) | 0 | 32/64 | 0, 0 | 1e-9 | exact | pass |
| 10e | angular momentum change over 1 rad, viscous − inviscid | 0 | 32/64 | −1.18e-2, −1.00e-2 | — | — | report |
| 10f | settling sphere, λ 0.3, ratio 2: U / U_s K(λ) − 1 | Haberman–Sayre | 24/32/48 (a = 3.6/4.8/7.2 cells) | +7.66e-2, +5.64e-2, +4.48e-2 | 9.6e-2, 7.1e-2, 5.6e-2 | 1.06, 0.57 | pass |
| 10f | rising spheres, ratios 0.5 and 0.1 | same | 24 | +7.50e-2, +7.57e-2 | stable | — | pass (stability) |
| 11a | static drop 2-D, La 12000, 10 t_σ: p / (σ/R) − 1 | Laplace | 32/64 | 2.15e-3, 5.10e-4 | 2.7e-3, 6.4e-4 | 2.08 | pass |
| 11a | same: parasitic Ca (max over the run) | 0 | 32/64 | 4.34e-5, 4.67e-5 | 5.5e-5, 5.9e-5 | does not fall | pass |
| 11a | same, `curvature cell` | Laplace | 32 | p +1.17e-1, Ca 1.87e-3 | — | — | report |
| 11a | inviscid static drop, 10 t_σ | Laplace | 32 | p −2.7e-3; max U 0.15 U_σ | — | not bounded | report (preview) |
| 11a | 3-D static drop 32³, La 12000, 4 t_σ | 2σ/R | 32 | p +1.80e-3; Ca 6.8e-4 (max U 5.2e-2 U_σ, final 4.4e-3) | — | — | report |
| 11b | oscillating drop 2-D, n 2, ε 0.05: ω / Lamb − 1 | n(n²−1)σ/ρR³ | 16/32/64/128 | −4.47e-2, −1.70e-2, −4.25e-3, −1.23e-2 | 5.6e-2, 2.2e-2, 5.4e-3, — | 1.39, 2.00, then the inviscid drift | pass |
| 11b | 3-D, 32³, 1.5 periods (before §2.6) | n(n−1)(n+2)σ/ρR³ | 32 | −2.98e-2 | — | — | report |
| 11b | 2-D viscous (La 12000): decay / (4ν/R²) − 1 (before §2.6) | irrotational | 32 | +1.54e-1 | — | — | report |
| 11c | capillary-gravity wave, Bond 1: ω / exact − 1 | (gk + σk³/ρ) tanh kd | 16/32/64 | −5.62e-2, −2.64e-2, −1.26e-2 | 7.1e-2, 3.3e-2, 1.6e-2 | 1.09, 1.06 (≥ 0.9; first order) | pass |

Notes:

- **Time accuracy.** Viscosity is backward Euler: 10a converges at order 1.0 (the time step scales with h), and the
  Couette start-up at 0.5–1.8.
- **Wave damping.** The net damping converges at order 1.2–1.3 to Lamb's root, with −2.8% left at 64 cells, where the
  surface boundary layer is one cell. With the numerical damping included it happens to match to 1e-3 at 64; that is a
  cancellation, not accuracy. No-slip walls add 79% at 32 cells (the tank's side and bottom boundary layers). Keulegan's
  wall correction was not computed, so the no-slip value is reported, not gated.
- **Couette torque.** At Re 0.375 the error is within ±0.65% at all three levels without a clean order (the cut cells
  of both cylinders change irregularly). At Re 3.75 it stays at +1.3–1.5%. It grew as the time step was cut (CFL 0.125:
  +1.6%), so part of it is per step: the advection of momentum beside the no-slip cut cells (§2.3) is the suspect, not
  isolated. Reported, not gated.
- **Settling.** Converges at order 1.06 then 0.57 toward the reference. Extrapolating, the 48-cell error of 4.5% would
  need several times the resolution to reach the reference's own 1%. First order at the sphere (the no-slip crossing
  is second order per difference, but the strain quadrature in cut control volumes is first order). The 3-D drag of a
  *prescribed* sphere in the same tube was 0.89 of Haberman–Sayre at 24 cells.
- **Parasitic currents.** At La 12000 the 2-D drop's largest parasitic Ca is 4.3e-5 and 4.7e-5 at 32 and 64 cells
  (U ≈ 3.4e-3 U_σ). It does not fall with refinement, though the end-of-run speed does (1.5e-3, 4.1e-4 U_σ) and the
  pressure converges at second order. Before the incremental pressure (§2.3) the same drop had 3.8e-5 and 1.2e-5, so
  the predictor costs a factor of up to 4 at 64 cells.
- **Oscillating drop.** The 128-cell row is worse than 64: the inviscid drop's slow parasitic growth (§5) contaminates
  the second moment over two periods (its fitted decay is negative). Larger amplitudes shift the frequency down
  nonlinearly (−4.6% at ε = 0.2), so ε = 0.05 is used.

## 5. Parasitic currents and the curvature scheme

Static drop, 2-D, 32 cells, R = 0.25 (8 cells), 10 capillary times. U_σ = sqrt(σ/(ρR)).

| Variant | Largest U / U_σ | Ca (La 12000) | Pressure error |
|---|---|---|---|
| default (interpolated κ, constant interface extension, viscous La 12000) | 3.4e-3 | 4.3e-5 | 2.2e-3 |
| `curvature cell` (the liquid cell's κ) | 1.5e-1 | 1.9e-3 | 1.2e-1 |
| linear interface extension (m7b's), viscous | 2.5 (grows) | 3.2e-2 | — |
| inviscid (default numerics) | 0.15 (grows; 0.26 at 10 t_σ in another run) | — | 6.7e-3 |
| inviscid, reinitialisation every step | 1.0e-2 at 9 t_σ, still rising slowly | — | — |
| 3-D 32³, La 12000, 4 t_σ | 5.2e-2 (transient), 4.4e-3 at the end | 6.8e-4 | 1.8e-3 |

- **Curvature at the interface.** Interpolating κ to the sub-cell interface position matters most: the cell value is
  the curvature of a level set up to half a cell away, 1/(R + φ). The initial interface curvature error is 5.1e-3
  interpolated against 1.3e-1 for the cell value, and the parasitic currents differ by 40×.
- **Viscosity is required.** Without it, the parasitic modes are not damped. An inviscid static drop's currents grow
  from 3e-3 U_σ to order 0.1–1 U_σ over 2–10 capillary times, whatever the time step (capillary_cfl 0.1 and 0.5
  alike), and with or without particles, reinitialisation, the second momentum stage or the volume correction. Only
  reinitialising every step held them near 1e-2 U_σ, at the price of first-order gravity waves (M7a §2). Surface
  tension without viscosity is therefore `preview`.
- **The interface extension** (§2.6) was the strongest single factor.

## 6. Evidence and the label

With an option on, Configure adds its cases (memoised per numerics). Measured first Configure, 16 threads: m7b's
cases 3.1 s, + viscosity 1.1 s, + both 2.5 s, so about 5.6 s with both and about 7.6 s with bodies as well.

| Id | Check | Tolerance | Default value |
|---|---|---|---|
| `viscous-taylor-green-n32` | 10a | 6.6e-4 | 5.26e-4 |
| `viscous-wave-n32` | 10b, total decay / Lamb − 1 (the numerical damping included) | 3.9e-2 | 3.07e-2 |
| `couette-torque-n32`, `couette-profile-n32` | 10c, ω 0.1 | 9.2e-3, 2.3e-2 | 7.36e-3, 1.84e-2 |
| `cylinder-drag-n32` | 10d | 3.2e-2 | 2.49e-2 |
| `rigid-rotation-n32` | 10e | 1e-9 | 0 |
| `laplace-pressure-n32`, `parasitic-n32` | 11a, 2 capillary times | 2.6e-3, 3.0e-5 | 2.04e-3, 2.33e-5 |
| `capillary-wave-n32` | 11c | 3.3e-2 | 2.64e-2 |
| `oscillating-drop-n32` | 11b | 2.2e-2 | 1.70e-2 |

- Conditions `viscosity` {off, on} and `surface_tension` {off, on}, always present (sorted with the others).
- The scope sentence gains a clause per option, naming the certified cases, first order in time and in the capillary
  wave, and what is not covered.
- `preview` with `fidelityLimit`: surface tension without viscosity, or surface tension with solids or bodies.
- The settling sphere (case 10) is not in the evidence (20 s or more); it is in the validation test.
- Not keyed into the base evidence: the base cases' numerics are unchanged. The m7c cases bind the base numerics plus
  `capillary_cfl`, `curvature`, `viscosity_tolerance`, `viscous_cfl` and two fixed internals (4 samples per axis,
  θ_min 0.05).

## 7. Parameters

| Key | Default | Meaning |
|---|---|---|
| `viscosity` | 0 | dynamic viscosity, Pa s (0: inviscid) |
| `viscous_walls` | `no_slip` | `no_slip` or `free_slip` for the walls, static solids and bodies |
| `surface_tension` | 0 | N/m (0: none) |
| `viscosity_tolerance` | 1e-8 | relative residual of the viscous CG |
| `viscous_cfl` | 4 | dt ≤ c h²/ν (0: none) |
| `capillary_cfl` | 0.5 | dt ≤ c sqrt(ρh³/(2πσ)) |
| `curvature` | `interpolated` | or `cell` |

Refused: negative viscosity or surface tension, unknown `viscous_walls` or `curvature`, `capillary_cfl` outside
(0, 2], `viscosity_tolerance` outside (0, 1). The chunk fields are generated from the schema (scalars), so no new
chunk keyword. The physical record names viscosity and surface tension when either is on (m7b's record otherwise).
`memoryEstimate` + 300 B/cell with viscosity, + 8 with surface tension. `costEstimate` × 2.8 with viscosity and ×
(capillary steps / other steps) when the capillary limit binds.

**Checkpoints.** With bodies and viscosity the header holds each body's last impulse and its viscous part (the
prediction of §2.2), so stop/resume stays byte-equal (`LiquidSolverTest` H). m7b streams are unchanged otherwise.

## 8. Cost (Apple M4 Max, 16 threads)

3-D dam break in a 1 m cube (column 0.3 × 0.6 × 1 m, a box obstacle, particles on), the mean of 8 steps after 3, at
0.15 s; the scratch driver of M7a §8's setup. Water μ = 1e-3, ρ = 1000; "honey" μ = 10, ρ = 1420; σ = 0.072.

| Grid | Physics | s/step | Viscous solve (CG iterations) | Pressure (PCG / step) | Peak RSS |
|---|---|---|---|---|---|
| 64³ | none | 0.069 | — | 0.033 (22.0) | 251 MB |
| 64³ | water viscosity | 0.106 (+54%) | 0.035 (7.1) | 0.034 | 325 MB |
| 64³ | honey viscosity | 0.169 (+146%) | 0.096 (33.5) | 0.036 | 328 MB |
| 64³ | surface tension | 0.068 (±0) | — | 0.032 | 256 MB |
| 64³ | honey + surface tension | 0.167 | 0.093 (31.9) | 0.036 | 330 MB |
| 128³ | none | 0.311 | — | 0.160 (28.0) | 1.10 GB |
| 128³ | water viscosity | 0.480 (+54%) | 0.174 (9.1) | 0.144 | 1.51 GB |
| 128³ | honey viscosity | 0.876 (+182%) | 0.535 (42.4) | 0.177 | 1.50 GB |
| 128³ | surface tension | 0.290 (±0, noise) | — | 0.142 | 1.11 GB |
| 128³ | honey + surface tension | 0.865 | 0.529 (42.9) | 0.173 | 1.52 GB |

- Viscosity costs +54% for water and +150–180% for honey (Jacobi CG iterations grow with ν dt/h²: 7–9 for water,
  32–43 for honey), and about 200 B/cell of memory. `costEstimate` uses ×2.8.
- Surface tension costs nothing measurable per step (one curvature pass; the jump is a right-hand-side term).
- Not optimised: the CG is Jacobi-preconditioned and touches every face (no active-region compaction); a multigrid or
  block preconditioner would cut the honey iterations.

**The capillary limit for water** (σ = 0.072 N/m, ρ = 1000) at capillary_cfl 0.5 against the other limits of a
1 m-deep tank (wave 0.5h/sqrt(gD), advective at sqrt(2gD) ≈ 4.4 m/s):

| Cell | Capillary limit | Wave limit | Advective limit | Steps shortened? |
|---|---|---|---|---|
| 4 mm | 5.9 ms | 0.64 ms | 0.45 ms | no |
| 1 cm | 23.5 ms | 1.6 ms | 1.1 ms | no |

It binds below h = 2πσ/(ρgD), which is 46 µm for a 1 m domain and 4.6 mm for a 1 cm one. For water at bake
resolutions surface tension costs one curvature pass and a right-hand-side term per projection. For small droplets
and very fine cells it sets the step.

## 9. Bakes with both options off

The step's arithmetic is unchanged with both off:

- the jump terms are guarded by a null curvature;
- gravity is added in the same place and order;
- no viscous code runs;
- the interface extension is m7b's;
- the checkpoint and the physical record are m7b's.

Two baselines were baked with the m7b.1 binary before any change and again with m7c.1, in private scratch directories.
Every content-addressed frame file name matches (36 and 30 payloads):

- a dam break (24 cells, a box obstacle, fields and particles published, 12 frames);
- a tank with a dynamic sphere (density 300) and a prescribed spinning, oscillating box (32 cells, fields, 10 frames).

## 10. Demo

`scenes/Tests/Simulation/liquid_honey_water.RISEscene` and `scripts/simulation_m7c_honey.sh [resolution]` (scene
and caches under `~/.cache/rise-m7c`, frames to `rendered/simulation_m7/honey_<res>*.png`). Two 0.6 × 0.3 × 0.3 m
tanks side by side, each with a 0.15 × 0.15 × 0.3 m column against its −x wall:

- honey: `viscosity 10`, `density 1420`, `surface_tension 0.07`, no-slip walls; amber dielectric, ior 1.49;
- water: inviscid; dielectric, ior 1.33.

At 48 cells along x (1.25 cm), both bakes label predictive. Six frames, path traced at 640 × 400 with 16 samples.

**What the frames look like** (looked at, t = 0, 0.4, 0.8, 1.2, 1.6, 2.0 s; the camera sees honey on the left, water
on the right):

- t = 0: two blocks.
- t = 0.4: the water has crossed its tank and climbed the far wall in a ragged splash sheet. The honey has slumped
  into a smooth wedge with a rounded front about two thirds of the way across, its surface glassy.
- t = 0.8–1.2: the water sloshes back with a rippled, faceted surface. The honey is a smooth slab with a rounded
  front near the far wall.
- t = 2.0: the water still ripples. The honey is a flat, smooth slab over the whole tank floor, with a slightly raised
  rim against the walls.

The contrast reads clearly: smooth and slow against splashing.

- The honey still spreads across the 0.6 m tank in about a second. That is right for a 15 cm column of 10 Pa s
  (the viscous slump speed ρgh²/(3μ) is of order metres per second). It looks fast because the scale is small.
- The amber tint is faint: the transmittance is close to 1 over a few centimetres.
- No coiling or buckling is shown (the scene has no thin stream).
- Bake time at 48 cells, 16 threads: water 18.7 s, honey 40.4 s (61 frames each, both predictive); the whole
  script, with the render of six frames, 73 s. The frames were re-rendered after the final code change (§2.6).

## 11. Pins and change log

- `liquid.mac` version `m7b.1` → `m7c.1`; `LiquidRigidTest`'s version check updated.
- `LiquidSolverTest`: new section H (labels, the 15 + 6 / 15 + 4 / 15 + 10 evidence counts, refusals, 1/3/8 threads
  and twins, stop/restore in memory and streamed, macro steps and rollback with viscosity, surface tension and a
  dynamic body).
- `tests/test_schedule.tsv`: `LiquidPhysicsValidationTest` wide:3; `run_all_tests.{sh,ps1}`: dual mode.
- `CstDeriveGoldenTest`: one hand-added line for `liquid_honey_water.RISEscene` (1992 bytes, 35 lines), from
  `--dump` with the `Job::` log lines stripped. The method reproduces `liquid_rigid_bodies`'s committed line
  exactly. Until the scene is tracked the test may report it as STALE. Existing liquid lines did not drift (the new
  scalar parameters do not enter the dump).
- `SceneEditorSuggestionsTest` (keyword count 191) and `RewireConnectionTest`: unchanged (no new chunk keyword or
  reference field).
- `IJob`, `ChunkCategory`: unchanged. Contract: one solver-side Appendix C row; no member added or changed.
- Docs: `SIMULATION_IN_RISE_PROPOSAL.md` status, `SIMULATION_M7_DESIGN.md` §7 step 5, `Parsers/README.md` "Liquid (M7)".

## 12. Open, and not verified

- **Time accuracy** of viscosity is first order (backward Euler, split once per step), and the incremental pressure
  needs ν dt/h² ≤ 4 to converge per step. A monolithic Stokes solve (pressure and viscosity together, Larionov, Batty
  and Bridson 2017) would remove both the predictor and the limit. It was not built.
- **The free-surface normal stress.** The split step imposes p = 0 and zero viscous traction separately, not
  p = 2μ n·D·n. The wave damping converges (order 1.2–1.3) but slowly.
- **Wall-adjacent advection.** The Re 3.75 Couette torque bias of +1.3% (§4 notes) is not isolated.
- **Settling sphere at first order**; 4.5% at 7 cells per radius.
- **Surface tension is first order** (the constant interface extension). The minmod-limited linear extension was
  second order in 2-D but unstable in 3-D; something better (a smoother curvature, or a height-function curvature) is
  open.
- **Inviscid surface tension** is preview (parasitic growth). **Contact angles**: 90° at domain walls in the
  curvature only; nothing at solids; no specified angle. **Variable viscosity**: not built.
- **The free disk** loses 1% more angular momentum over one radian with ν = 0.01 than without, at 32 and 64 cells,
  although one viscous step conserves it to round-off. The interplay with advection and the surface weights was not
  found.
- **Not run:** 256³; the VS2022, Android and Xcode builds (lists edited by script; the make build compiles
  everything); the GUI; the 3-D oscillating drop and the viscous drop decay after the interface-extension change.
- **Not verified:** the primary reports of Haberman and Sayre 1958 and Bohlin 1960 (not fetched; secondary reproductions
  agree, §4.1 10f); Lamb 1932 §349 itself (the decay rate agrees with the secondary statement of it, M7_REFERENCES §2); Lamb's 3-D drop formula (checked against secondary sources, 2026-11-26, not re-derived; the 2-D n(n²−1) form is derived here and agrees with two secondary sources at n = 2);
  bitwise reproducibility across platforms (per platform, as M7a).
