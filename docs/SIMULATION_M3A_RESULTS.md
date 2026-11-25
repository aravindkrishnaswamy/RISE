# M3a results: the cloth solver

2026-11-23. Design: [SIMULATION_M3_DESIGN.md](SIMULATION_M3_DESIGN.md) §1–§4 (step M3a). Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md). Contract: `src/Library/Simulation/SimulationSolver.h`
(unchanged in this pass).

**Status.** `cloth.shell` runs on the contract through both hosts, bakes and resumes byte-identically in `rise-sim`,
and earns `predictive` from evidence it computes itself. Validation cases 1–6 pass with second-order convergence.
One design decision did not survive validation: discrete-shell hinge bending on edges does not converge on either
structured triangulation (§3), so the shipped bending energy is a line-based discretisation on the same mesh. The
hinge model stays selectable and publishes `preview`.

**Second pass (m3a.2, same day).** (a) Speed: a geometric multigrid preconditioner with line smoothing replaces
symmetric Gauss–Seidel in CG, assembly and the solver run on a deterministic thread pool (results identical for
any thread count), and Newton reuses the accepted trial's gradient and Hessian; 51 × 51 interactive now runs at
2.2–2.3× real time on 4 threads (0.54× before), bake 3.4× faster (§7). (b) Bake-mode contact is incremental
potential contact: a C² log barrier with the thickness offset against planes, spheres, boxes, cylinders and
static or rigidly moving triangle meshes, additive continuous collision detection bounding every Newton step,
and lagged smoothed Coulomb friction; six contact evidence cases gate `predictive` for bake runs with colliders
(§2, §4b, §5). Self-collision is implemented on the same machinery but not validated (`preview`). Not in this pass:
renderer integration, viewport.

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Cloth/ClothMaterial.{h,cpp}` | Material records, validation, canonical form |
| `src/Library/Simulation/Cloth/ClothLinearAlgebra.{h,cpp}` | 3×3-block CSR matrix, PCG with the grid multigrid preconditioner (line Gauss–Seidel smoothing, Galerkin coarse levels) or block symmetric Gauss–Seidel / Jacobi, banded Cholesky |
| `src/Library/Simulation/Cloth/ClothParallel.{h,cpp}` | Fork-join pool; chunking fixed by problem size, so results do not depend on the thread count |
| `src/Library/Simulation/Cloth/ClothModel.{h,cpp}` | Structured sheet, energies, pins, colliders, loads, integrators, Newton, static solve |
| `src/Library/Simulation/Cloth/ClothContact.{h,cpp}` | Barrier contact: distances and their derivatives, barrier, broad phase, additive CCD, exact crossing test, lagged friction, moving mesh colliders |
| `src/Library/Simulation/Cloth/ClothValidation.{h,cpp}` | References (elastica, elastic catenary, integrator theory) and case runners, shared by the test and the evidence |
| `src/Library/Simulation/Cloth/ClothSolver.{h,cpp}` | `cloth.shell` on the contract; parameters, evidence, frames, ports, checkpoints |
| `tests/ClothValidationTest.cpp` | Cases 1–6, element checks, reference checks, evidence-family errors, contact kernels and contact cases (i)–(v); dual mode (`--extended`) |
| `tests/ClothSolverTest.cpp` | Contract conformance through both hosts, stop/resume (also with barrier contact, friction and a rotating mesh), thread-count independence, macro steps, port feed, labels, mesh and scene-input colliders |
| `tools/rise_sim.cpp` | Registers the cloth solver beside the fire solver |
| Build lists | All five (make `Filelist`, Android cmake, VS2022 vcxproj + filters, Xcode); strict floating point (below) |
| `run_all_tests.{sh,ps1}`, `tests/test_schedule.tsv` | `ClothValidationTest` registered as dual mode; schedule rows |

**Strict floating point.** The library builds with `-ffast-math`. Cloth sources compile with `-fno-fast-math
-ffp-contract=off` in every build system (make library and split test archive, cmake `COMPILE_OPTIONS`, Xcode
`COMPILER_FLAGS`, VS `FloatingPointModel Strict`), as the fire driver does, so checkpoints, frames and evidence digests
do not depend on reassociation or FMA contraction. Bitwise reproducibility holds per platform; across platforms it also
depends on `libm` (`atan2`, `sin`, `cos`, `sqrt` is exact) and is not claimed.

## 2. Model as implemented

**Sheet.** A rectangle of rest size `size_u × size_v` in material coordinates (u = warp, v = weft), `nu × nv` vertices,
cell sides `hu = size_u/(nu-1)`, `hv = size_v/(nv-1)`, each cell split into two right triangles. `alternating`
(default) flips the diagonal in a checkerboard (union jack); `consistent` keeps one diagonal. World placement
`x = origin + stretch_u u axis_u + stretch_v v axis_v` (axes orthonormalised); `stretch > 1` with pinned edges gives
prestress.

**Membrane.** Orthotropic St. Venant–Kirchhoff, constant-strain triangles: `F = Ds Dm⁻¹` (3×2), `E = (FᵀF − I)/2`,
`e = (E11, E22, 2E12)`, `W = A0 eᵀ C e / 2`, plane stress, thickness integrated (N/m):
`C11 = E_w/(1−ν12ν21)`, `C22 = E_f/(1−ν12ν21)`, `C12 = ν12 E_f/(1−ν12ν21)`, `C66 = G`, `ν21 = ν12 E_f/E_w`. Uniaxial
warp tension with free lateral edges gives `S11 = E_w E11` exactly. Gradient `A0 F S β_k` and the full Hessian
(material part `Bᵀ C B` plus geometric part `A0 (β_kᵀ S β_l) I`) are analytic; the Hessian matches finite
differences to 6e-11 in tension.

**Bending: the line model (default, `bending_model = lines`).** Every vertex row and column is a discrete rod: at each
interior vertex of a line, `E = k |κb|²` with the curvature binormal `κb = 2 e1×e2 / (|e1||e2| + e1·e2)`
(Bergou et al. 2008, `|κb| = 2 tan(ψ/2)` for turning angle ψ), `k = B_warp w_j/(2 hu)` on rows (w_j the row's share
of the width: hv, or hv/2 on the edge rows) and `k = B_weft w_i/(2 hv)` on columns. Twist comes from a dihedral hinge
on each cell diagonal, `E = k θ²`, `k = √(B_warp B_weft) hu hv/(hu² + hv²)`. For a quadratic surface
`w = (a u² + 2b uv + c v²)/2` (shown by hand for both triangulations; checked numerically) the energy density is
`(B_warp a² + B_weft c² + 2√(B_warp B_weft) b²)/2`: an orthotropic plate with `D11 = B_warp`, `D22 = B_weft`,
`D66 = √(B_warp B_weft)/2`, **`D12 = 0`**. With `B_warp = B_weft = B` this is the isotropic plate with `D = B` and
**no Poisson coupling in bending**; the model cannot represent ν in bending. Rows and columns measure total curvature
(normal and in-plane), so in-plane bending of a yarn line costs `B κ_g²`; for smooth in-plane fields this is
small against membrane shear (with the cotton record, a 10 cm radius of geodesic curvature costs about 0.5% of the
energy of a 0.1 shear strain). Gradients exact; Hessians Gauss–Newton `2k JᵀJ` (the dropped term is of relative size ψ or θ per element).

**Clamp correction.** A bending element one of whose wings is entirely pinned represents curvature over half a cell,
so its stiffness doubles. Without it a clamp built from two pinned vertex rows puts the zero-slope condition half a
cell outside the clamp and the cantilever and clamped plate converge at first order.

**Bending: the hinge model (`bending_model = hinges`, preview).** Discrete-shell hinges on every interior edge,
`E = k θ²`, `k = B_warp hv/(2hu)` on edges along v, `B_weft hu/(2hv)` on edges along u, diagonal as above. Consistent
(exact for quadratic surfaces, same energy density as above on the alternating mesh) but **not stable**: on the
alternating mesh each edge-along-v hinge measures the second difference along only one vertex row, and which row
alternates from column to column, so each row is hinged at every other vertex and rows can zig-zag against each
other at the grid scale; on the consistent mesh the stencil also couples bend and twist. The cantilever is too soft by
a factor that grows under refinement at fixed cell aspect (§4). Changing the per-edge stiffness formula (the
cotangent/area-weighted discrete-shells form) only rescales the same stencil on right-triangle meshes and cannot remove
the mode; neither triangulation does. This is why the line model ships.

**Mass, loads, damping.** Lumped mass `σ × (tensor-product trapezoid area)` (`hu hv` inside, halves on edges, quarters
at corners), independent of the triangulation. Gravity; `pressure` (Pa on the front face, force `−p` times the
vertex's current vector area) and port loads frozen at the start of each step; optional explicit quadratic air drag
on the normal velocity (default off); Rayleigh damping `D = α M + β K(x_n)` (K the projected elastic Hessian at the
step start), **calibrated, not measured**.

**Contact: penalty (interactive, `contact = penalty`).** Penalty per vertex against planes, spheres, boxes and
cylinders on the depth `d = thickness − signed distance`, stiffness `k = contact_stiffness × vertex area`, C² in d:
`k d³/(6δ)` for `d < δ = thickness/2`, quadratic beyond. A quadratic (C¹) penalty made Newton chatter on the contact
set (6 step halvings in 120 steps on the demonstration, against 1 with the C² form). No collision detection, no
friction, analytic shapes only. Always `preview`.

**Contact: barrier (bake default, `contact = barrier`; `ClothContact.{h,cpp}`).** Incremental potential contact
(Li et al. 2020) with the thickness offset of Li et al. 2021 ("Codimensional IPC"):

- Every primitive pair within reach adds `κ w b(z)`, `z = d − ξ` (d the unsigned distance, ξ = `contact_thickness`),
  `b(z) = −(z − d̂)² ln(z/d̂)` for `0 < z < d̂ = contact_activation`, 0 beyond (C² at d̂); `z ≤ 0` is infeasible.
  `w` is the cloth area the pair stands for (the vertex's dual area; a third of the adjacent triangles' area for an
  edge; a third of the triangle's area for a collider vertex against a cloth triangle), so `κ = barrier_stiffness`
  (N/m³) does not depend on the mesh. A sheet at rest under its weight sits at the gap where `κ |b′(z)| = σ g`,
  `z ≈ d̂ − √(σ g d̂/(3κ))` (0.93 mm above the offset with the defaults).
- Pairs: cloth vertex–analytic shape (signed distance with its Hessian), cloth vertex–collider triangle, cloth
  edge–collider edge, collider vertex–cloth triangle, and with `self_collision` cloth vertex–triangle and edge–edge
  (pairs sharing a vertex excluded). Distances are classified into point–point, point–edge, point–plane and
  edge–edge pieces; each piece's squared distance is the minimum over closest-point parameters of a function
  bilinear in positions and parameters, so its Hessian is the Schur complement of the joint Hessian (exact inside
  the piece; finite differences agree to 5e-9). Hessians: exact on the exact-Hessian try, projected to PSD per pair
  (Jacobi) otherwise. Self-contact pairs add only their diagonal blocks (the matrix pattern is fixed); their
  gradient is exact.
- Broad phase: a hash grid over the collider primitives (cell = max(mean collider edge, 2(ξ + d̂))), queried with
  each cloth primitive's box swept over the Newton step and inflated by ξ + d̂; self pairs use a grid over the cloth.
  One candidate list per Newton iteration serves the CCD, the line search and the next gradient; it is sorted, so
  everything is deterministic.
- Continuous collision detection: additive CCD (conservative advancement with the offset, Li et al. 2021, Alg. 1)
  on every candidate pair caps each Newton step so the gap never falls below 0.1 of its value (s = 0.1); analytic
  shapes use their 1-Lipschitz signed distance the same way. The predictor `x_n + h v_n` is capped the same way.
  An exact test (coplanarity cubic, then the inside test at each root) checks accepted steps in validation.
- Friction: smoothed Coulomb, lagged per step (Li et al. 2020 §5): at the step's start each active pair freezes its
  normal force `λ = κ w |b′(z)|`, its closest-point weights and a tangent basis T; the dissipative potential
  `(μλ/c) F0(|Tᵀ v_rel|)` with `v = c (x − x̃)` the integrator's own velocity (BDF2 or backward Euler),
  `F0′ = f1`, `f1(y) = 2y/ε − y²/ε²` below the slip speed `ε = friction_velocity`, 1 above. One coefficient
  (`friction`, static = kinetic, default 0.3: a representative cloth-on-solid value, not measured). Sticking under
  smoothed friction is a creep at `v = ε (1 − √(1 − tan α/μ))`, which the incline case confirms to 1.2e-6 of ε.
- Mesh colliders: inline arrays or a `mesh.v1` scene input (`ISceneInputs::Sample` at time 0, its identity in the
  effective parameters), moved rigidly (`fixed`, `translate`, `rotate`, `oscillate`); within a step they sit at the
  step's end. A step starts from `x_n` with the vertices that a moving collider could reach during the step carried
  rigidly with it (region: three times its travel, twice the reach and twice the longest cloth edge around it), so
  the start is feasible; then the predictor and Newton as above.
- Not supported: implicit midpoint with barrier contact (refused; `x_{n+1} = 2y − x_n` is not CCD-bounded), pins
  driven into a collider (the step fails), deforming (non-rigid) colliders.

**Pins.** Dirichlet vertices (corner, edge or boundary sets, `depth` rows deep), fixed or following a rigid motion
(translate, rotate about an axis, oscillate); positions and velocities are set analytically at each step's end time.

**Time integration.** Each step minimises the incremental potential
`Φ(x) = |x − x̂|²_M/(2ĥ²) + E(x) − f·(x − x_n) + (c/2)|x − x̃|²_D`:

| Scheme | ĥ, x̂ | Velocity |
|---|---|---|
| Backward Euler | `h`, `x_n + h v_n` | `(x − x_n)/h` |
| BDF2, variable step (`ω = h/h_prev`; first step backward Euler) | `1/a`, `−(b x_n + c x_{n−1})/a − (b v_n + c v_{n−1})/a²` with `a = 1/h + 1/(h+k)`, `b = −(h+k)/(hk)`, `c = h/(k(h+k))`, `k = h_prev` | `a (x − x̃)`, `x̃ = −(b x_n + c x_{n−1})/a` |
| Implicit midpoint | solves for the midpoint y: `h/2`, `x_n + (h/2) v_n` | `x_{n+1} = 2y − x_n`, `v_{n+1} = 2(x_{n+1} − x_n)/h − v_n` |

**Newton.** Each iteration first uses the exact Hessian (membrane exact, bending Gauss–Newton). When the linear solver
finds it indefinite (CG negative curvature, Cholesky pivot) or the direction is not a descent direction, that iteration
uses the projected Hessian, which keeps the compressive membrane stress each triangle's share of the inertia can
absorb: eigenvalue `l < 0` of S with eigenvector e becomes `max(l, −σ/(4ĥ²|g|²))`, `g_k = β_k·e` (clamped to 0 in
statics). The element's geometric part plus a quarter of its area's inertia is then positive semidefinite, and the
quarters over a vertex's triangles never exceed its lumped mass on this mesh. Backtracking Armijo line search with a
round-off allowance, starting at the step the collision detection allows (barrier contact); the full trial is
evaluated with its gradient and Hessian, which the next iteration reuses when it is accepted. Converged when every free vertex has `|∇Φ_i|/m_i ≤ newton_tolerance` (m/s²) or is below its own
floating-point floor `8 ε |x|_∞ |H_ii|/m_i` (positions carry `ε|x|` absolute error; this floor grows as 1/h² and is
what limits the finest validation levels). A step that fails is retried as two half steps (variable-step BDF2 keeps its
order), up to four levels. On the first second of the demonstration bake (51 × 51, `newton_tolerance` 1e-3) the
sequence of changes went from 14.3 Newton iterations per step and 10 halvings (clamped projection kept for the rest of
a solve once triggered, quadratic penalty) to 9.9 and 1 (inertia-aware projection, C² penalty) to 5.4 and 0 (exact
Hessian retried on every iteration).

**Linear solvers.** Block CSR with the pattern fixed at start. PCG preconditioned by one V(1,1) cycle of a geometric
multigrid on the sheet's vertex grid: levels coarsen by two in each grid direction (the last vertex of a line always
kept) down to 64 vertices, the prolongation is bilinear in the grid indices and zero on pinned vertices, coarse
operators are Galerkin `PᵀAP` (gathered through precomputed term lists), the coarsest level is factored by banded
Cholesky. The smoother is line Gauss–Seidel: every vertex row and column is a block-banded system solved exactly,
rows then columns forward, columns then rows backward (the adjoint, so the cycle is symmetric). Point smoothers
fail here because the woven membrane is strongly anisotropic (warp stiffness E/h² against shear G/h², ratio 75 for
cotton): point-smoothed multigrid needed 10.3 CG iterations per Newton step at 1e-2 on the demonstration against
4.3 with lines, and 41 against 11 at 1e-6. Lines that do not couple share a colour and are relaxed together (in
groups of four whose band solves interleave, hiding the triangular solves' latency, and in parallel). Banded
Cholesky under the narrower of the u-fastest and v-fastest orderings is used when `dofs × bandwidth²` is small
(≤ 2e7 in dynamics, ≤ 5e9 in static solves), falling back to PCG. `linear_solver = pcg` keeps the symmetric
Gauss–Seidel PCG of the first pass.

**Threads.** `threads` (default min(4, cores); not part of the key). Element assembly runs colour by colour (greedy
colouring in element order; elements of a colour share no vertex) in chunks of 128; matrix products, vector updates,
the multigrid's line sweeps, Galerkin products and line factorisations run in fixed chunks; inner products sum fixed
chunks of 4096 entries in order. The chunking depends only on the problem size, so 1, 3 and 4 threads publish
byte-identical frames (`ClothSolverTest`). Contact runs on the caller.

**Static equilibrium.** From the current state, eight pseudo-time stages minimising `Φ + |x − x_k|²_M/(2τ²)`
(τ from 0.01–0.05 s, ×4 per stage), then full Newton on `E − f·x`.

## 3. Deviations from the design note, and why

| Design | Built | Why |
|---|---|---|
| Discrete-shell hinge bending | Line (rod) bending on rows and columns plus diagonal twist hinges; hinges kept as `preview` | Hinges do not converge on structured meshes (§2, §4) |
| Penalty contact (quadratic implied) | C² penalty (interactive) | Newton chattered on the contact set |
| Bake: barrier contact, CCD, smoothed friction | As designed (IPC with offset, additive CCD, lagged friction); self-collision built on it but `preview` | Self-contact only adds diagonal Hessian blocks (fixed matrix pattern) and has no validation case yet |
| Projected element Hessians | Exact first, inertia-aware projection on failure | Clamped projection converged linearly in compression (14 Newton iterations per step, halvings) |
| Block-Jacobi PCG | Multigrid-preconditioned CG with line Gauss–Seidel (m3a.2); symmetric Gauss–Seidel and Jacobi kept | CG was ~90% of the run time; 74 → 4.3 CG iterations per Newton step (interactive), 102 → 11 (bake) |
| Single thread | Deterministic thread pool, results independent of the thread count | Interactive 51 × 51 needs it to reach 2× real time on this machine |
| Material coordinates as UVs in `mesh.v1` | Not published | `mesh.v1` has no UV field; the codec was not changed. u = i·hu, v = j·hv from the vertex index and the effective parameters |
| Material records from Clyde et al. 2017 / Kawabata | Four representative records | No verified measured tables yet; each record says so |

## 4. Validation (`bin/tests/ClothValidationTest`: fast tier 25–40 s with contact, depending on load; `--extended` 62 s adds the finer levels)

Cotton record (`ρ = 0.15 kg/m²`, `E_w = 3000`, `E_f = 2000`, `G = 40 N/m`, `ν = 0.2`, `B_w = 2e-5`,
`B_f = 1.2e-5 N·m`), alternating mesh, line bending, BDF2 with `2.5 n` steps per period (time and space errors both
O(h²)). Tolerances are 1.25 × the observed error at that level, rounded up; orders must reach 1.8.

| # | Case | Reference | Resolution | Error | Tolerance | Result |
|---|---|---|---|---|---|---|
| 1 | Hanging strip, L0 = 1 m, span 0.8 m, no bending | Elastic catenary for the model's StVK law (Irvine 1981 with that law; reduces to Irvine's Hookean solution to 8.6e-7 and to the inextensible catenary to 1e-9) | 10 / 20 / 40 (+ 80 / 160) segments | 5.37e-3 / 1.33e-3 / 3.32e-4 (+ 8.29e-5 / 2.07e-5), max position error / sag | 6.8e-3 / 1.7e-3 / 4.2e-4 (+ 1.1e-4 / 2.6e-5) | pass, orders 2.01, 2.00, 2.00, 2.00 |
| 2 | Cantilever, L = 2 bending lengths, warp (B = 2e-5) and weft (B = 1.2e-5) | Elastica by shooting (RK4, 8000 steps, bisection), with the model's axial stretch | 10 / 20 / 40 (+ 80 / 160) segments | 3.36e-3 / 6.89e-4 / 1.77e-4 (+ 4.75e-5 / 1.24e-5), max position error / tip drop, both directions | 4.2e-3 / 8.7e-4 / 2.3e-4 (+ 6.0e-5 / 1.6e-5) | pass, orders 2.28, 1.96, 1.90, 1.94; tip drop error 2.6e-3, 3.6e-4, 4.8e-5, 6.4e-6 (order ~2.9) |
| 2h | Same, **hinge model** | same | 10 / 20 / 40 | alternating 0.30 / 0.75 / 1.03; consistent 0.38 / 0.73 / 1.01 | — | **does not converge** (documented; `preview`) |
| 3 | Membrane a = 1.003, b = 0.752 m, 0.3% equibiaxial stretch, modes (1,1), (2,1), (1,2) | `f = ½√((T_u (m/a)² + T_v (n/b)²)/σ)` | 8 / 16 / 32 (+ 64) cells | (1,1) 3.77e-2 / 1.02e-2 / 2.61e-3 (+ 6.54e-4); (2,1) 4.96e-2 / 1.34e-2 / 3.42e-3 (+ 8.53e-4); (1,2) 6.48e-2 / 1.75e-2 / 4.46e-3 (+ 1.11e-3) | 1.25 × observed | pass, orders 1.89, 1.97, 2.00 each |
| 3x | Same, measured vs the fully discrete prediction (semi-discrete ω_h through the integrator's principal root) | — | 8 / 16 / 32 | gaps 1.2e-5 … 1.7e-5 | 2e-4 | pass (the code does what the analysis says) |
| 4 | Square plate 0.2 m, isotropic B = 2e-5, simply supported | `ω = 2π²/a² √(D/σ)`, D = B | 8 / 16 / 32 (+ 64) cells | 4.10e-2 / 1.11e-2 / 2.84e-3 (+ 7.43e-4) | 5.2e-2 / 1.4e-2 / 3.6e-3 (+ 9.3e-4) | pass, orders 1.89, 1.97, 1.93 |
| 4 | Same, clamped | `λ² = 35.985` (Leissa 1969; published Ritz values 35.985–35.992, spread 2e-4) | 8 / 16 / 32 (+ 64) | 8.99e-2 / 2.57e-2 / 6.67e-3 (+ 1.68e-3) | 1.13e-1 / 3.3e-2 / 8.4e-3 (+ 2.1e-3) | pass, orders 1.81, 1.94, 1.99 |
| 4 | Simply supported, orthotropic (cotton) | `ω² = (π/a)⁴ (√D11 + √D22)²/σ` (exact for D12 = 0, D66 = √(D11 D22)/2) | 16 | 1.11e-2 | 1.4e-2 | pass |
| 5 | Undamped (1,1) membrane mode, 4 periods | Energy ratio per period `|ζ(iω_h Δt)|^{2N}` of each integrator | 16 / 32 (40 / 80 steps per period) | see below | 1.25 × observed | pass |
| 6 | Refinement | — | as above | orders above | ≥ 1.8 | pass |

Case 5, energy change per period (measured; the linear theory agrees to the error column):

| Integrator | 40 steps/period | 80 steps/period | |measured − theory| |
|---|---|---|---|
| Backward Euler | −62.3% | −38.9% | 2.9e-7, 7.1e-7 |
| BDF2 | −1.14% | −0.150% | 1.4e-7, 2.0e-8 |
| Implicit midpoint | −5.1e-10 | −2.7e-11 | (at the Newton tolerance) |

BDF2's dissipation does not make case 5 uninformative: it matches its exact amplification factor to 1e-7 of the
oscillation energy, which verifies the variable-step coefficients. Implicit midpoint is the energy-conserving option
(`integrator = implicit_midpoint`) and has a quarter of BDF2's phase error (`−(ωΔt)²/12` against `−(ωΔt)²/3`; BDF2 and
backward Euler share the leading phase error). The bake default stays **BDF2**, for a measured reason: on the first
0.5 s of the demonstration (51 × 51, impact on the sphere) implicit midpoint needed 25.8 Newton iterations per step,
16 step halvings and 3.7× the time of BDF2 (5.4 iterations, no halvings). It is not L-stable, so the stiff in-plane
membrane modes (ωΔt of order 100 at bake steps) ring instead of being damped. Use it for undamped, contact-free
studies where energy matters.

Element checks: bending gradients (rod and hinge) against finite differences 2e-9 and 2e-7 relative; membrane
gradient everywhere; projected membrane Hessian PSD in compression.

Membrane locking: none seen in these cases (cylindrical bending and small-amplitude plates do not stretch flat
triangles). Large doubly-curved deformation (drape) can lock constant-strain triangles; that is design case 8 (M3d)
and not tested here.

## 4b. Contact validation (barrier contact; `ClothValidationTest`, "Contact")

Cotton record, BDF2 unless stated, defaults `κ = 1e5 N/m³`, `d̂ = 1 mm`, `ξ = 2 mm`, `μ = 0.3`, `ε = 1 mm/s`.

| # | Case | Reference | Resolution | Error | Tolerance | Result |
|---|---|---|---|---|---|---|
| — | Kernels | Finite differences; closed forms | 400 random pairs, all pieces | distance gradient + Hessian 5.1e-9; barrier 3e-7; shape Hessians < 1e-6; CCD stops at 0.1 of the gap (0.0990); exact test finds crossings | — | pass |
| i | Strip (0.2 m, 20 segments) lying on a frictionless plane, static | Force balance and the barrier law `κ \|b′(z)\| = σ g` per vertex | d̂ = 1, 0.5, 0.25, 0.125 mm | 9.1e-13, 1.4e-13, 8.0e-9, 1.4e-9 | 1e-8 | pass; penetration-free; the gap above the offset 9.32e-4, 4.52e-4, 2.17e-4, 1.02e-4 m: tends to ξ at order 1.04, 1.06, 1.09 in d̂ |
| ii | Heavy strip (half length 6 bending lengths) over a frictionless cylinder (R = 2 bending lengths), static | Wrapped elastica: arc of radius ρ = R + ξ + z*(σg), free parts by shooting on the lift-off point (centre column held at the top by symmetry) | 20 / 40 / 80 segments | 2.38e-3 / 9.01e-4 / 5.83e-4 (lift-off at s = 0.0656 m, 74° of wrap) | 3.0e-3 / 1.2e-3 / 7.3e-4 | pass |
| ii | Same, mesh × d̂ refinement (`--extended`) | same | 20/40/80/160 at d̂ = 1, 0.25, 0.0625 mm | d̂ 1 mm: 2.38e-3, 9.01e-4, 5.83e-4, 5.30e-4; 0.25 mm: 2.16e-3, 9.26e-4, 3.59e-4, 2.94e-4; 0.0625 mm: 2.05e-3, 9.27e-4, 2.39e-4, 1.57e-4 | mesh order over 20 → 80 at the smallest d̂ > 1.4 | pass: mesh orders 1.14, 1.96 then a floor; the floor (finest mesh) falls as d̂^0.42–0.43 (≈ √d̂: the barrier gap varies with the contact pressure along the arc while ρ uses the gap for the weight) |
| iii | Sheet (0.1 m, 10 × 10) on a plane, gravity tilted, undamped | Below the friction angle: creep `ε (1 − √(1 − tan α/μ))` of smoothed friction; above: `g (sin α − μ cos α)` | angles 0.25, 0.5, 0.9 × atan μ; atan μ + 0.05, 0.2, 0.4 | stick 1.0e-12, 1.2e-6, 4.6e-14 (of ε; creep 1.3e-4, 2.9e-4, 6.8e-4 m/s); slide 7.4e-14, 2.1e-13, 1.3e-13 (backward Euler 1.0e-12) | 1e-5 (stick), 1e-6 (slide) | pass; sticks below and slides above the friction angle |
| iv | Sheet (0.5 m, 21 × 21) falling, tilted, onto a corner-up cube given as a 12-triangle mesh, 1 s; static and rotating at 3 rad/s about the vertical | No intersection: every accepted step's straight path checked exactly, and the end gap > 0 | 240 steps each (`--extended`: 31 × 31, 2 s) | 0 violations (static: smallest gap 3.7e-5 m, CCD shortened 44 line searches, 1 halving; rotating: 1.9e-5 m, 37, 0; extended: 0 and 0) | 0 | pass |
| v | Sheet (0.2 m, 11 × 11) dropped 5 cm flat onto a frictionless plane, undamped | Total energy (kinetic + gravity + elastic + barrier) never above its start | 96 steps | largest rise −1.7e-3 of the drop energy (BDF2 and backward Euler; both only lose energy, BDF2 bounces to 32% of the drop height, backward Euler to 6%) | 1e-6 | pass |

What the cases establish: (i) the barrier and its force balance are exact to the Newton tolerance and the gap tends
to the offset linearly in d̂; (ii) a bending-contact problem converges to its analytic solution under mesh
refinement (first to second order: the contact boundary has a jump in the curvature derivative), down to a floor
set by the barrier's gap model that vanishes like √d̂; (iii) friction sticks and slides at the analytic friction
angle and acceleration, and its smoothing is the documented law; (iv) the CCD-bounded Newton step keeps a tumbling
sheet off a sharp mesh collider, static and moving (checked exactly, independently of the CCD); (v) the barrier
injects no energy at impact. Not established: drape against measured fabric (design case 8), self-collision, the
error of a particular bake.

## 5. Evidence and labels

Chosen: **computed at Configure**, by running a fast subset of the cases for the configured material, triangulation,
bending model and integrator, memoised per process (the runs are deterministic, so the memo is exact). First Configure
for a new material/integrator costs about 0.7 s on this machine; later ones are free. Rejected alternative: a
validation program writing an evidence record the solver loads — it adds a file format and a staleness problem for no
gain at this cost.

Cases (each digest-bound to solver id, version, case id, resolution, material values, triangulation, bending model and
integrator through `EvidenceDigest`):

| Id | Check | Tolerance (BE / BDF2 / midpoint) |
|---|---|---|
| `catenary-n20` | case 1 at 20 segments | 1.7e-3 |
| `cantilever-warp-n20`, `cantilever-weft-n20` | case 2 at 20 segments with B_warp, B_weft | 8.7e-4 |
| `membrane-11-n16`, `membrane-21-n16` | case 3 at 16 cells, configured integrator | 1.3e-2 / 1.3e-2 / 5.5e-3; 1.7e-2 / 1.7e-2 / 9.6e-3 |
| `plate-ss-n16` | orthotropic simply supported plate, configured B_warp, B_weft | 1.5e-2 / 1.4e-2 / 6.6e-3 |
| `energy-n16` | case 5, configured integrator | 3.7e-7 / 1.8e-7 / 1.2e-9 |
| `order-cantilever` | orders over 10/20/40 segments: error = max(0, 2 − smallest) | 0.2 |
| `order-membrane` | order over 8/16 cells | 0.2 |
| `contact-rest-n20` | contact (i) at d̂ as configured | 1e-8 (Newton-tolerance level) |
| `contact-cylinder-n40` | contact (ii) at 40 segments | 2.0e-3 |
| `contact-stick` | contact (iii) at half the friction angle: creep vs the smoothed law, / ε | 1e-5 |
| `contact-slide` | contact (iii) 0.2 rad past the friction angle | 1e-6 |
| `contact-mesh` | contact (iv), 11 × 11 sheet, static cube, 0.5 s: violations | 0.5 (none allowed) |
| `contact-bounce` | contact (v): largest energy rise / drop energy | 1e-6 |

The contact cases run only for runs with barrier contact, at the run's own material, integrator and contact
parameters (κ, d̂, μ, ε, ξ, bound into their digests); they cost about 0.5 s at the first Configure and are
memoised like the others. Tolerances come from the four built-in records with both bake integrators
(`ClothValidationTest`, "Contact evidence"): rest, stick and slide are exact up to the Newton tolerance (largest
2.5e-12, 1.2e-6, 1.0e-12), the cylinder's largest error is 1.59e-3 (polyester knit: its soft membrane stretches
against the inextensible reference), the mesh case has no violation and the bounce gains no energy.

Tolerances are 1.25 × the largest error observed over the four built-in records for that integrator
(`ClothValidationTest`, "Evidence resolutions"; all four records give the same errors to 3 digits because the cases are
scaled by each record's own bending length, tension and period). The midpoint energy tolerance sits at the Newton
tolerance and may need loosening on another platform.

The solver **requests `predictive` only in bake mode, without air drag, and either without contact or with
barrier contact and no self-collision**; interactive mode, penalty contact (`contact = penalty`), self-collision and
drag request `preview`. The host then derives the label from the cases (`EffectiveFidelity`), so the hinge model
(cantilever errors 0.75 and 0.62, plate 0.15), any material whose cases fail, and any contact configuration whose
contact cases fail (for example a d̂ so large that the cylinder case exceeds its tolerance, or a material without
bending rigidity) publish `preview`. `ClothSolverTest` confirms `predictive` for all four records with BDF2, for
cotton with implicit midpoint, and for barrier contact against a sphere and a cube mesh.

What `predictive` with contact covers: the cloth model of §2 (structured sheet, line bending, the record's
material) integrated by BDF2 or backward Euler in bake mode, with barrier contact and lagged smoothed friction
against analytic shapes and static or rigidly moving mesh colliders, at contact parameters whose own cases pass.
It certifies the model and its discretisation family on canonical problems (exact force balance, an analytic
drape, analytic friction, an intersection-free tumble, no energy gain); it does not bound the error of a given
bake, and it does not cover self-collision, air drag, imported irregular meshes, measured fabric behaviour or
deforming colliders.

## 6. Solver, parameters, outputs and ports

`cloth.shell`, version `m3a.2` (was `m3a.1`: results for existing parameters changed, because the linear solver,
the assembly order and bake-mode contact changed). Parameters (canonical CBOR/JSON map; unknown keys refused; every value written back
into the effective parameters; `pace_ms` is not keyed):

| Key | Default | Meaning |
|---|---|---|
| `mode` | `bake` | `bake` or `interactive` (backward Euler, truncated Newton) |
| `integrator` | `bdf2` (bake), `backward_euler` (interactive) | also `implicit_midpoint`, `backward_euler` in bake |
| `size_u`, `size_v` | 1, 1 m | rest size |
| `vertices_u`, `vertices_v` | 51, 51 | 2–2000 each |
| `triangulation` | `alternating` | or `consistent` |
| `bending_model` | `lines` | or `hinges` (preview) |
| `origin`, `axis_u`, `axis_v` | (−0.5, 1, 0.5), (1, 0, 0), (0, 0, −1) | placement; front normal `axis_u × axis_v` |
| `stretch_u`, `stretch_v` | 1 | initial placement stretch |
| `material` | `cotton-plain-weave` | built-in name, or a map (`base` plus overrides, or every value with `name` and `source`) |
| `frames`, `frame_rate` | 121, 30 Hz | outputs at `i / frame_rate` |
| `substeps` | 8 (bake), 2 (interactive) | per output interval |
| `gravity` | (0, −9.81, 0) | m/s² |
| `pins` | none | array of `{select: u0v0|u1v0|u0v1|u1v1|u0|u1|v0|v1|boundary, depth, motion: {type: fixed|translate|rotate|oscillate, …}}` |
| `colliders` | none | array of `{type: plane, point, normal}`, `{type: sphere, center, radius}`, `{type: box, min, max}`, `{type: cylinder, point, axis, radius}` (infinite), `{type: mesh, vertices: [x, y, z, …], triangles: [i, j, k, …], motion}` or `{type: mesh, scene_input: name, motion}` (a `mesh.v1` scene input sampled at time 0; its identity is written back as `identity`); mesh `motion` as for pins |
| `contact` | `barrier` (bake), `penalty` (interactive) | contact model; mesh colliders and self-collision need `barrier` |
| `contact_thickness` | 2 mm | offset ξ (both models) |
| `contact_stiffness` | 1e5 N/m³ | penalty per unit vertex area per unit depth |
| `barrier_stiffness`, `contact_activation` | 1e5 N/m³, 1 mm | barrier κ and d̂ |
| `friction`, `friction_velocity` | 0.3, 1 mm/s | Coulomb coefficient (static = kinetic; representative, not measured) and slip speed ε (barrier only) |
| `self_collision` | false | cloth–cloth barrier contact (`preview`) |
| `threads` | min(4, cores) | not keyed: results do not depend on it |
| `pressure` | 0 Pa | on the front face |
| `drag_coefficient`, `air_density` | 0, 1.2 kg/m³ | quadratic normal drag (off by default) |
| `damping` | true | apply the record's Rayleigh damping |
| `newton_tolerance` | 1e-3 (bake), 1e-2 (interactive) m/s² | per-vertex residual acceleration |
| `newton_iterations` | 50 (bake), 3 (interactive) | interactive accepts the iterate after these |
| `cg_tolerance`, `cg_iterations` | 1e-6 / 1e-2, 2000 | relative residual |
| `linear_solver` | `auto` | `auto` (direct when the band is narrow, else multigrid PCG), `mg_pcg`, `pcg` (symmetric Gauss–Seidel PCG), `direct` |

Material records (all: "representative literature-range values, not a measured fabric"):

| Name | σ kg/m² | E_w N/m | E_f N/m | G N/m | ν | B_w N·m | B_f N·m | α 1/s | β s |
|---|---|---|---|---|---|---|---|---|---|
| `cotton-plain-weave` | 0.15 | 3000 | 2000 | 40 | 0.20 | 2.0e-5 | 1.2e-5 | 0.05 | 2e-4 |
| `denim-twill` | 0.33 | 8000 | 5000 | 150 | 0.20 | 8.0e-5 | 5.0e-5 | 0.05 | 2e-4 |
| `silk-lightweight` | 0.07 | 800 | 600 | 10 | 0.20 | 2.0e-6 | 1.5e-6 | 0.05 | 2e-4 |
| `polyester-knit` | 0.18 | 150 | 100 | 5 | 0.30 | 4.0e-6 | 3.0e-6 | 0.05 | 2e-4 |

The record (values, units, source, "damping calibrated") is embedded in the cache's `physicalRecords`.

**Output.** `surface`, `mesh.v1`: positions (binary32 of the double state), velocities (the integrator's), unit
area-weighted vertex normals, fixed topology (the topology digest is constant). **No UVs: `mesh.v1` has no UV field.**

**Ports.** Offers `surface_state` (quantity `surface`, m, `mesh.v1`, the `surface` payloads). Accepts `surface_load`
(quantity `surface load`, Pa, `grid.v1`): channels `pressure` (1 component) and/or `traction` (3 components, world
frame), each `nu × nv × 1` with origin 0, indexed by vertex (i, j), units Pa; blocks may be dense or constant, cells
outside blocks take the background. Accepted at the start of each segment (output interval or imposed macro step) and
held through it. A constant pressure through the port gives byte-identical frames to the same `pressure` parameter.

**Macro steps.** Segments run from the current time to the next output time or the imposed macro end in
`ceil(length/(interval/substeps))` equal substeps. Macro ends on the output grid reproduce free stepping byte for byte;
half-interval ends with an even `substeps` give the same substep lengths and, on the test sheet, identical positions.

**Checkpoints.** Canonical CBOR of the segment state, `x`, `v`, the BDF2 history (`x_{n−1}`, `v_{n−1}`, `h_{n−1}`),
the step clock and the held port loads (doubles bit for bit). Stop and restore in the middle of a segment, and worker
stop/resume/resume, reproduce uninterrupted runs byte for byte (`ClothSolverTest`).

## 7. Demonstration and timings

A 1 m cotton sheet, horizontal at y = 1 m, pinned at the corners u0v0 and u1v0, falling over a sphere of radius 0.3 m
centred at (0, 0.5, 0), 4 s at 30 Hz (121 frames):

```sh
cat > drape.json <<'EOF'
{"mode": "bake", "vertices_u": 51, "vertices_v": 51, "frames": 121,
 "pins": [{"select": "u0v0"}, {"select": "u1v0"}],
 "colliders": [{"type": "sphere", "center": [0, 0.5, 0], "radius": 0.3}]}
EOF
bin/tools/rise_sim bake cloth.shell drape.json "$TMPDIR/drape"
```

In m3a.2 this bake uses barrier contact with friction and is `predictive`; `"contact": "penalty"` reproduces the
first pass's physics.

**Speed (m3a.2 against m3a.1), the same physics.** Measured with a driver that steps `ClothSystem` exactly as the
solver does (penalty contact against the sphere, as in m3a.1, so before and after solve the same equations), on this
machine (Apple M1 Max) while a GPU fire bake used about 8 cores and another agent ran test renders (load average
5–10). Two runs each; ranges are the spread.

| Mesh | Mode | m3a.1 (1 thread) | m3a.2, 1 thread | m3a.2, 4 threads | Newton / step | CG / Newton step |
|---|---|---|---|---|---|---|
| 51 × 51 | interactive | 7.15–7.4 s (0.54–0.56× real time) | 3.75–3.86 s (1.04–1.07×) | **1.75–1.76 s (2.27–2.29×)** | 3 → 3 (truncated) | 74.1 → 4.3 |
| 51 × 51 | bake | 75.6–75.7 s | 46.0–47.5 s | 20.6–22.2 s | 6.18 → 5.18 | 102.5 → 10.9 |
| 101 × 101 | interactive | 46.8–47.3 s | 16.4–17.0 s | 5.6–6.2 s (0.65–0.71×) | 3 → 3 | 130 → 4.6 |
| 101 × 101 | bake | 569–572 s | 352 s | 118–126 s | 7.15 → 7.43 | 171 → 13.9 |

Step halvings: 51 bake 2 → 0, 101 bake 18 → 12. Interactive runs are truncated Newton, so their result depends on
the linear solves: the mean final residual of a step fell from 1.3e3 to 9.5e2 m/s² (51 × 51) with the better solves.

Where the m3a.1 → m3a.2 gain comes from, interactive 51 × 51, one thread: multigrid with point Gauss–Seidel
smoothing 7.2 → 3.8 s (10.3 CG iterations per Newton step); line smoothing 4.3 iterations but its band solves were
latency-bound (5.1 s) until lines of a colour were solved interleaved (3.9 s); reusing the accepted trial's
evaluation 3.5 s; threads 1.75 s on 4. A modified Newton (the first iteration's Hessian and preconditioner for the
whole step) reached 2.6 s on one thread but left a ten times larger residual (1.1e4 m/s²) and was not adopted
(`NewtonOptions::lagHessian` remains for experiments).

Remaining time (`sample`, 51 × 51): bake on one thread — line sweeps of the multigrid 60%, matrix products (CG
and the cycle's residuals) 12%, element assembly 11%, Galerkin products and line factorisations 8%, coarse levels
3%. Interactive on 4 threads (all threads) — line sweeps 26%, assembly 16%, setup 9%, products 6%, and 38% of the
workers' time spent waiting at the pool's joins: at 2,601 vertices a line-sweep colour is a few tens of
microseconds of work, so more threads stop paying (6 threads 1.70 s against 1.76 s on 4).
The next steps would be fewer, larger parallel regions (a colour-parallel cycle per level) and a cheaper smoother
on the coarse levels.

**Agreement with m3a.1.** Interactive runs are truncated and differ (above). Bake runs converge each step to the
Newton tolerance, so old and new agree to that tolerance per step, but the drape over the sphere buckles into folds
(a symmetry-breaking instability), which amplifies any difference: at the default `newton_tolerance` (1e-3 m/s²)
the two builds differ by 1.6 cm at frame 10 and 0.1–0.24 m later, and either differs from a tightly converged run
by as much. Run both with `newton_tolerance` 1e-6 and `cg_tolerance` 1e-10 and the difference is 6.0e-12 m at
frame 10 (t = 0.33 s, first contact), 1.3e-8 m at frame 20 and 4.2e-4 m at frame 30: agreement to solver tolerance,
then amplified by the folding. The validation suite passes with unchanged tolerances; its errors are unchanged to the 3–4 digits
reported in §4. Stop/resume byte identity, worker equality and thread-count independence are tested in
`ClothSolverTest`.

**Contact cost** (`rise-sim bake`, the demonstration, 4 threads, same load):

| Run | Wall (4 s simulated, incl. Configure) | Note |
|---|---|---|
| no collider, bake | 15.5 s | evidence 0.7 s |
| sphere, barrier contact + friction (the default bake) | 19.8 s (1 thread: 40.8 s) | `predictive`; contact evidence +0.5 s |
| sphere, `contact = penalty`, bake | 21.5 s | `preview` |
| icosphere mesh collider (642 vertices, 1,280 triangles), barrier | 35.1 s | `predictive` |
| sphere, interactive (penalty) | 2.1 s (1 thread: 4.1 s) | `preview` |
| no collider, interactive | 2.0 s | |

(Load average 7–11 during these runs.) Barrier contact against an analytic shape costs about a quarter on top of
the free sheet and less than the penalty (fewer Newton iterations once friction holds the sheet); against a
1,280-triangle mesh about 2.3× the free sheet (broad phase, edge–edge pairs and CCD on every Newton iteration, on one
thread).

## 8. Limits

- Interactive mode keeps the penalty contact (no collision detection, no friction, analytic shapes only).
- Barrier contact: colliders are analytic shapes or rigidly moving meshes; a mesh scene input is sampled once (time
  0), so colliders cannot yet follow animated scene objects, and pins cannot follow scene transforms. Analytic
  shapes are tested at the vertices only (a sharp box edge can pass between cloth vertices; use a mesh collider for
  sharp geometry). Edge–edge distances of nearly parallel edges switch to point–edge pieces without IPC's mollifier,
  so the barrier is not C² there. Friction is lagged once per step (no fixed-point friction iterations) and has one
  coefficient. Implicit midpoint is refused with barrier contact. A pin driven into a collider fails the step.
- Self-collision (`self_collision`) works on the same machinery but adds only diagonal Hessian blocks (slower Newton
  in folds) and has no validation case: `preview`.
- The barrier's resting gap depends on the contact pressure (`z ≈ d̂ − √(p d̂/(3κ))`), an O(√d̂) modelling error of
  shapes in contact (contact case ii); reduce `contact_activation` for tighter contact.
- No Poisson coupling in bending (D12 = 0). Rows and columns also stiffen in-plane bending of yarn lines (small).
- Only solver-generated rectangular structured sheets. Irregular meshes would need a different bending discretisation
  (the design's subdivision Kirchhoff–Love upgrade) and would run as `preview`.
- Air drag is explicit and unvalidated; pressure and traction are frozen per step (first order in time).
- Interactive mode is truncated Newton (3 iterations; its residual is far from converged, mean 950 m/s² on the
  demonstration) and runs at 2.2× real time at 2.6k vertices on 4 threads but below real time at 10k vertices.
- Damping is calibrated, not measured. Material records are representative, not measured.
- Bitwise reproducibility is per platform (libm).

## 9. Open questions for the contract freeze

Added in m3a.2:

10. **Scene inputs over time.** `ISceneInputs::Sample` gives one encoded frame per time; a moving collider needs
    its motion within a step (rigid transform or vertex positions at both ends) and the CCD needs that path. A
    transform-track input (rigid motion per scene object) beside mesh frames would let colliders and pins follow
    scene objects without resampling meshes; whether `Identity` covers motion is unstated.
11. **Thread count.** Results here do not depend on it, but the contract has no place to say so (or to declare a
    solver's thread use to a host scheduling several solvers); `threads` is an unkeyed parameter like `pace_ms`.
12. **Label conditions.** `predictive` now depends on run conditions (bake mode, barrier contact, no
    self-collision, no drag) that the solver encodes by requesting `preview`; the contract cannot express "this
    evidence covers these conditions" (see question 5).

From m3a.1:

1. **Mesh attributes.** `mesh.v1` cannot carry material coordinates (UVs) or per-vertex scalars (stress, strain). A
   `mesh.v2` with optional named per-vertex attributes, or a per-output static attribute record, is needed before the
   renderer binds cloth textures to material coordinates (M3b).
2. **Per-vertex port payloads.** `surface_load` reuses `grid.v1` as a vertex lattice; it works for structured sheets
   but has no meaning for an irregular mesh, and one `units` string per port cannot describe mixed channels. A
   vertex-attribute codec (or `mesh.v2` attributes) would serve loads and coupling better.
3. **Port timing.** `Accept` returns the latest value at or before a time; loads are held over a segment (first order).
   The contract does not say whether a solver may interpolate between offers or must sample at macro boundaries only.
4. **Macro steps and substeps.** The contract requires reaching `macroEnd` exactly but says nothing about whether
   different macro grids must agree beyond discretisation error; cloth agrees exactly only when the grids give the
   same substeps.
5. **Evidence scope.** Evidence here verifies the model family on canonical problems; it does not bound a given bake's
   error (time step, mesh, contact). Should `predictive` require a per-run check (e.g. a step-halving estimate), and
   should conditions (no contact, bake mode) be expressible in the contract rather than in the requested label?
6. **Evidence cost.** Configure-time evidence costs ~0.7 s per material/integrator; the contract has no notion of an
   evidence cache shared across processes.
7. **Capabilities per configuration.** `interactive` is declared per solver, but cloth's interactivity depends on mode
   and resolution; `costEstimate` is left 0.
8. **Effective parameters vs. description.** "Values that do not change results removed" conflicts with
   "Configure(effective) reproduces the description" when provenance text (material name, source) appears in notes and
   physical records. Cloth keeps name and source in the key.
9. **Progress end time.** `IProgress::Report` end time is `count × interval` in the reference solvers and cloth, while the
   last output is at `(count − 1) × interval`; `rise-sim status` therefore shows `1 / 1.0333`.
