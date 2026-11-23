# M3a results: the cloth solver

2026-11-23. Design: [SIMULATION_M3_DESIGN.md](SIMULATION_M3_DESIGN.md) §1–§4 (step M3a). Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md). Contract: `src/Library/Simulation/SimulationSolver.h`
(unchanged in this pass).

**Status.** `cloth.shell` runs on the contract through both hosts, bakes and resumes byte-identically in `rise-sim`,
and earns `predictive` from evidence it computes itself. Validation cases 1–6 pass with second-order convergence.
One design decision did not survive validation: discrete-shell hinge bending on edges does not converge on either
structured triangulation (§3), so the shipped bending energy is a line-based discretisation on the same mesh. The
hinge model stays selectable and publishes `preview`. Not in this pass: barrier contact with continuous collision
detection, self-collision, friction, scene-mesh colliders, renderer integration, viewport.

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Cloth/ClothMaterial.{h,cpp}` | Material records, validation, canonical form |
| `src/Library/Simulation/Cloth/ClothLinearAlgebra.{h,cpp}` | 3×3-block CSR matrix, PCG (block symmetric Gauss–Seidel or block Jacobi), banded Cholesky |
| `src/Library/Simulation/Cloth/ClothModel.{h,cpp}` | Structured sheet, energies, pins, colliders, loads, integrators, Newton, static solve |
| `src/Library/Simulation/Cloth/ClothValidation.{h,cpp}` | References (elastica, elastic catenary, integrator theory) and case runners, shared by the test and the evidence |
| `src/Library/Simulation/Cloth/ClothSolver.{h,cpp}` | `cloth.shell` on the contract; parameters, evidence, frames, ports, checkpoints |
| `tests/ClothValidationTest.cpp` | Cases 1–6, element checks, reference checks, evidence-family errors; dual mode (`--extended`) |
| `tests/ClothSolverTest.cpp` | Contract conformance through both hosts, stop/resume, macro steps, port feed, labels |
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

**Contact (interactive quality).** Penalty per vertex against planes, spheres and axis-aligned boxes on the depth
`d = thickness − signed distance`, stiffness `k = contact_stiffness × vertex area`, C² in d: `k d³/(6δ)` for `d < δ =
thickness/2`, quadratic beyond. A quadratic (C¹) penalty made Newton chatter on the contact set (6 step halvings in
120 steps on the demonstration, against 1 with the C² form). No continuous collision detection, no friction, static
colliders. Contact is a separate term in `ClothSystem::ElementEnergies`; barrier contact and friction slot in there.

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
round-off allowance. Converged when every free vertex has `|∇Φ_i|/m_i ≤ newton_tolerance` (m/s²) or is below its own
floating-point floor `8 ε |x|_∞ |H_ii|/m_i` (positions carry `ε|x|` absolute error; this floor grows as 1/h² and is
what limits the finest validation levels). A step that fails is retried as two half steps (variable-step BDF2 keeps its
order), up to four levels. On the first second of the demonstration bake (51 × 51, `newton_tolerance` 1e-3) the
sequence of changes went from 14.3 Newton iterations per step and 10 halvings (clamped projection kept for the rest of
a solve once triggered, quadratic penalty) to 9.9 and 1 (inertia-aware projection, C² penalty) to 5.4 and 0 (exact
Hessian retried on every iteration).

**Linear solvers.** Block CSR with the pattern fixed at start. PCG with block symmetric Gauss–Seidel (about half the
iterations of block Jacobi on the demonstration). Banded Cholesky under the narrower of the u-fastest and v-fastest
orderings, used when `dofs × bandwidth²` is small (≤ 2e7 in dynamics, ≤ 5e9 in static solves, which are too
ill-conditioned for CG), falling back to PCG.

**Static equilibrium.** From the current state, eight pseudo-time stages minimising `Φ + |x − x_k|²_M/(2τ²)`
(τ from 0.01–0.05 s, ×4 per stage), then full Newton on `E − f·x`.

## 3. Deviations from the design note, and why

| Design | Built | Why |
|---|---|---|
| Discrete-shell hinge bending | Line (rod) bending on rows and columns plus diagonal twist hinges; hinges kept as `preview` | Hinges do not converge on structured meshes (§2, §4) |
| Penalty contact (quadratic implied) | C² penalty | Newton chattered on the contact set |
| Projected element Hessians | Exact first, inertia-aware projection on failure | Clamped projection converged linearly in compression (14 Newton iterations per step, halvings) |
| Block-Jacobi PCG | Block symmetric Gauss–Seidel by default (Jacobi available in the library) | About half the CG iterations; CG is ~90% of the run time |
| Material coordinates as UVs in `mesh.v1` | Not published | `mesh.v1` has no UV field; the codec was not changed. u = i·hu, v = j·hv from the vertex index and the effective parameters |
| Material records from Clyde et al. 2017 / Kawabata | Four representative records | No verified measured tables yet; each record says so |

## 4. Validation (`bin/tests/ClothValidationTest`: fast tier 15 s; `--extended` 37 s adds the finer levels)

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

Tolerances are 1.25 × the largest error observed over the four built-in records for that integrator
(`ClothValidationTest`, "Evidence resolutions"; all four records give the same errors to 3 digits because the cases are
scaled by each record's own bending length, tension and period). The midpoint energy tolerance sits at the Newton
tolerance and may need loosening on another platform.

The solver **requests `predictive` only in bake mode without colliders and without air drag**; interactive mode,
penalty contact and drag request `preview`. The host then derives the label from the cases (`EffectiveFidelity`), so
the hinge model (cantilever errors 0.75 and 0.62, plate 0.15) and any material whose cases fail publish `preview`.
The tolerances come from runs of every built-in record with every integrator; `ClothSolverTest` confirms
`predictive` for all four records with BDF2 and for cotton with implicit midpoint. Only solver-generated structured sheets exist in this
pass. The evidence certifies the model and its discretisation family on canonical problems; it does not bound the
error of a particular bake (see open questions).

## 6. Solver, parameters, outputs and ports

`cloth.shell`, version `m3a.1`. Parameters (canonical CBOR/JSON map; unknown keys refused; every value written back
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
| `colliders` | none | array of `{type: plane, point, normal}`, `{type: sphere, center, radius}`, `{type: box, min, max}` |
| `contact_stiffness`, `contact_thickness` | 1e5 N/m³, 2 mm | penalty per unit vertex area per unit depth; offset |
| `pressure` | 0 Pa | on the front face |
| `drag_coefficient`, `air_density` | 0, 1.2 kg/m³ | quadratic normal drag (off by default) |
| `damping` | true | apply the record's Rayleigh damping |
| `newton_tolerance` | 1e-3 (bake), 1e-2 (interactive) m/s² | per-vertex residual acceleration |
| `newton_iterations` | 50 (bake), 3 (interactive) | interactive accepts the iterate after these |
| `cg_tolerance`, `cg_iterations` | 1e-6 / 1e-2, 2000 | relative residual |
| `linear_solver` | `auto` | `pcg`, `direct` |

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

(With the collider the run is `preview`.) Wall time on this machine (Apple M1 Max, one thread) while a GPU fire bake
used about 8 cores (load average about 5–6), so expect ±20%:

| Mesh | Mode | Steps | Wall (4 s simulated, incl. first-Configure evidence ~0.7 s) | Per step | vs real time |
|---|---|---|---|---|---|
| 51 × 51 (2,601 vertices) | interactive (backward Euler, 2 substeps, ≤3 Newton) | 240 | 7.4 s (an earlier build: 16.0 s) | ~30 ms | 0.54× |
| 51 × 51 | bake (BDF2, 8 substeps, tolerance 1e-3 m/s²) | 960 | 75.7 s (an earlier build: 187 s) | ~78 ms | 1/19 |
| 101 × 101 (10,201 vertices) | interactive | 240 | 47.3 s | ~197 ms | 1/12 |
| 101 × 101 | bake | 960 | 572 s | ~595 ms | 1/143 |

The "earlier build" figures predate the Newton (exact Hessian retried per iteration, C² contact) and preconditioner
(symmetric Gauss–Seidel) changes and are kept to show their effect.

Profile: about 90% of the time is in PCG on the stiff membrane (E·Δt²/(σh²) of order 1e3–1e4); assembly is under 10%.

## 8. Limits

- Penalty contact only: no continuous collision detection (fast motion can tunnel), no friction, no self-collision,
  static analytic colliders only, colliders and pins cannot yet follow scene objects (no scene inputs read).
- No Poisson coupling in bending (D12 = 0). Rows and columns also stiffen in-plane bending of yarn lines (small).
- Only solver-generated rectangular structured sheets. Irregular meshes would need a different bending discretisation
  (the design's subdivision Kirchhoff–Love upgrade) and would run as `preview`.
- Air drag is explicit and unvalidated; pressure and traction are frozen per step (first order in time).
- Interactive mode is truncated Newton and below real time at ~10k vertices; the linear solver is the bottleneck.
- Damping is calibrated, not measured. Material records are representative, not measured.
- Bitwise reproducibility is per platform (libm).

## 9. Open questions for the contract freeze

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
