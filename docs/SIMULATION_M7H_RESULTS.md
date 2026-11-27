# M7h results: corotational sheet coupling, body–body contact, barrier check (`liquid.mac` m7h.1)

2026-11-26. Follows [M7g](SIMULATION_M7G_RESULTS.md), [M7f](SIMULATION_M7F_RESULTS.md), [M7e](SIMULATION_M7E_RESULTS.md)
and [M6](SIMULATION_M6_RESULTS.md) (rigid contact). References: [SIMULATION_M7_REFERENCES.md](SIMULATION_M7_REFERENCES.md).
Contract: one Appendix C row (ports added; no member changed).

**Status.** Two models changed: the cloth's response inside a macro step is now corotational (task 1), and
`rigid.bodies` resolves body–body contact and resolves a box's or cylinder's plane contact at all its support points
together (task 5). Versions: `liquid.mac` `m7g.1` → `m7h.1`, `cloth.shell` `m3a.4` → `m3a.5`, `rigid.bodies` `m6.1` →
`m6.2`. Sheets and rigid contact stay `preview`.

| Task | Outcome |
|---|---|
| 1. Rotation stretch | **Fixed** (§1): a corotational linearisation shared by the cloth and the liquid's copy. Rigid rotation at 10 rad/s, macro 1/30 s: 8.5e-5 edge strain (m7g 5.4e-2; the cloth's nonlinear model 8.4e-5). Catch demo: indicator 0.54% (below the warning). Wave demo: the indicator still warns (6.8%), from the free edge whipping (§1.5), not the macro step. Sweep: 60/60 stable; at 1/30 s the flap's strain no longer depends on the macro step |
| 2. Thin layers on a sheet | **Not fixed** (§2): unchanged by task 1 (3 cm bag at 24 cells −8.4% sharp with the correction, −41% without). One single-valued variant tried and reverted |
| 3. Coarse free-edge stair-step | **Partly** (§3): the 24-cell dam-break peak of 14.9 m/s (1/30 s) is now 2.06 and 8.64 (1/120 s) is 5.29, as a consequence of task 1; the 0.05 kg/m² flap at 1/120 s rose to 13.6 m/s (m7g 6.82). The free edge's coverage effect is located and a design given; not built |
| 4. Underside streams, watertightness | **Measured** (§4): flood-fill watertightness check built and tested (0 holes on an intact pinned sheet, a removed quad found). Catch at 64 and 128 cells: 0 holes; every attempted crossing held (419 and 10,080 cells), none passed; seen from below at 128 cells the streams leave the cloth at its pinned rim, not through it |
| 5. Body–body contact | **Built** (§5): spheres, boxes, cylinders (meshes refused among other bodies), same impulse model and parameters as the planes; elastic two-sphere collision keeps momentum and energy to 1e-15; a three-box stack rests 5 s (drift 8e-7 m, tilt 2e-6 rad); two submerged spheres collide with the owned exchange stable and the x account exact (9e-17 N s). The liquid takes rigid.bodies' planes and parameters through an additive port |
| 6. Second-order velocity at cut cells | **Assessed, not built** (§6): design and effort stated |
| 7. Viscosity with sheets; 3-D momentum audit | Not done (§7) |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Cloth/ClothLinearAlgebra.{h,cpp}` | `CorotatedElements`: per-element g0 and K0, rotations by a polar fit of each element's frame, assembly R K R^T, gradient, energy |
| `src/Library/Simulation/Cloth/ClothModel.{h,cpp}` | The linear model inside a macro step is corotational (`SetCorotated`, default on); `LinearElements`; `RotationLag` |
| `src/Library/Simulation/Cloth/ClothSolver.cpp` | Version m3a.5; port `surface_elements` |
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `SheetDynamics`: the elements, `Rotate`, `ResetElements`, the step's stiffness and elastic force from them; `LiquidSheet::open` (validation hook) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Rotations per step, the rotation lag; `SheetBarrierHoles` (flood fill); `BuildBarrier` waits for the level set |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | Version m7h.1; `sheet_elements` (sheet option `elements_port`), required with `response stiffness`; `body_contact`; the new rotation indicator and the barrier check in the progress detail; checkpoints |
| `src/Library/Simulation/Rigid/RigidBody.{h,cpp}` | `ContactShape`, `ShapeDistance`, `ResolvePairs`, `SolveContacts` (sequential impulses with accumulated, clamped impulses); plane contact of non-sphere bodies at all support points |
| `src/Library/Simulation/Rigid/RigidSolver.cpp`, `RigidParameterSchema.cpp` | Version m6.2; body options `restitution`, `friction`, `contact`; port `body_contact`; labels |
| `tests/LiquidClothCouplingTest.cpp` | L extended (corotational), M (watertightness), `--probe` reports the edge strain, connections |
| `tests/RigidSolverTest.cpp` | H (two spheres, stacks, contact with restore, untouched bodies, mesh refusal) |
| `tests/LiquidRigidTest.cpp` | Two spheres colliding under water (and `--collide`); port pins |
| `tests/ClothSolverTest.cpp`, `tests/LiquidSheetTest.cpp` | Version and port-count pins |
| `scenes/Tests/Simulation/liquid_cloth_{wave,catch}.RISEscene`, `liquid_rigid_bodies.RISEscene` | The new connections; `tests/data/cst_derive_golden.txt`: their three lines |
| `src/Library/Parsers/README.md`, `docs/SIMULATION_SOLVER_CONTRACT.md`, design and proposal | Notes |

No source file added: the five build lists are unchanged. No chunk keyword added (count 191; the new options are
sub-options of `sheet` and `body`).

## 1. Rotation within a macro step (task 1)

### 1.1 Model

Inside a macro step the cloth and the liquid's copy of it integrated the cloth's quadratic model about the step's start
x0 (M7e §2.4): E = g0·d + dᵀK0 d/2, d = x − x0. A rigid rotation is not a null motion of that model beyond first order,
so a sheet turning by θ moved its vertices along their tangents and was stretched by θ²/2 (M7f §7).

The model is now corotational, element by element (`CorotatedElements`). Each element e (membrane triangle, bending
hinge, bending rod) keeps its own gradient g_e and projected Hessian K_e at x0 (binary32, as they cross the port) and a
rotation R_e; with d_e = R_eᵀ p_e − x0_e per vertex,

    E_e = g_e · d_e + d_eᵀ K_e d_e / 2,   ∇E_e = R_e (g_e + K_e d_e),   ∇²E_e = R_e K_e R_eᵀ.

The rotations are held over a step, so within a step the energy is quadratic and the step stays the linear solve of
M7e: S = M(1 + dt α) + (dt β + dt²) Σ R_e K_e R_eᵀ, symmetric positive definite (each K_e is positive semidefinite, the
rotation a congruence). The elastic right-hand side is the model's force at the step's start, −Σ R_e(g_e + K_e d_e);
the non-elastic part of the accepted force (gravity, drag, contact at x0) is kept. A rigid motion of an element changes
neither its strain nor its stress (its rotated prestress −R_e g_e follows it).

- **Rotation per step.** R_e is the polar factor of Σ_k (p_k − p̄)(x0_k − x̄0)ᵀ + s n n0ᵀ over the element's frame
  vertices (s their mean squared radius; n, n0 the frame's unit fan normals), by Higham's scaled Newton iteration;
  the identity for a degenerate or inverted frame. Frames: a triangle's vertices; a hinge's (x0, x2, x1, x3); a rod's
  three vertices and the third vertex of the first triangle on its first edge. The rotation is taken at the
  **predicted** positions p = x_n + dt v_n (both sides have them), which for a rigid rotation turns the frame by
  atan(ω dt) of the step's ω dt; the straight-line stretch of the predictor then is strain in that frame and the
  implicit solve removes it.
- **Exact agreement.** The cloth (`ClothModel` in its linear mode) and the liquid (`SheetDynamics`) call the same
  functions on the same binary32 values. The cloth takes its rotations from its own state, the liquid from its copy;
  the two differ by the binary32 rounding of the exchanged state, as K0 did in m7e–m7g.
- **Ports.** `cloth.shell` offers `surface_elements` (field.v1 over each element's upper 3×3 blocks: element, kind,
  row, column, block, and the gradient on the diagonal items; domain: a digest of the block list). `liquid.mac`
  accepts it on `sheet_elements` (`elements_port`) and refuses an implicit sheet with `response stiffness` that does not
  connect it (the cloth's model inside the macro step is corotational; a linear copy would no longer match it). The
  committed wave and catch scenes connect it.
- **Not chosen.** The Newton step of backward Euler with the cloth's own nonlinear force at each liquid step would need
  the cloth's material model inside the liquid (or an exchange per liquid step); the corotational form needs only
  values the cloth already computes.

### 1.2 The rotating sheet (measured, `--rotate`, test L)

Free cotton square 0.3 m, 13 × 13 vertices, no gravity or damping, spinning rigidly; backward Euler, 4 steps per macro
step; the largest edge strain over 0.5 s.

| Axis | ω (rad/s) | H | Linear (m7g) | Corotational (m7h) | Nonlinear | (ωH)²/2 |
|---|---|---|---|---|---|---|
| in-plane | 1 | 1/120 | 3.6e-5 | 9.1e-7 | 9.1e-7 | 3.5e-5 |
| in-plane | 10 | 1/120 | 3.5e-3 | 9.1e-5 | 9.1e-5 | 3.5e-3 |
| in-plane | 30 | 1/120 | 3.1e-2 | 8.1e-4 | 8.1e-4 | 3.1e-2 |
| in-plane | 3 | 1/30 | 5.0e-3 | 7.6e-6 | 7.6e-6 | 5.0e-3 |
| in-plane | **10** | **1/30** | 5.4e-2 | **8.5e-5** | 8.4e-5 | 5.6e-2 |
| in-plane | 30 | 1/30 | 0.41 | 8.1e-4 | 7.1e-4 | 0.50 |
| normal | 10 | 1/30 | 5.4e-2 | 8.0e-5 | 8.0e-5 | 5.6e-2 |
| normal | 30 | 1/30 | 0.41 | 6.7e-4 | 6.7e-4 | 0.50 |

The corotational step's strain equals the nonlinear model's (the physical stretch of a spinning sheet) to 2% up to
ωH = 0.33, and to 15% at ωH = 1. Acceptance (under 0.5% at 10 rad/s, 1/30 s): 0.0085%. Test L now checks both.

### 1.3 Accuracy, stability, momentum

`LiquidClothCouplingTest` passes (all checks):

| Check | m7g.1 | m7h.1 |
|---|---|---|
| A operator symmetry, Cholesky | 1.6e-16, succeeds | same |
| B flap, 0.2 kg/m², 24 cells, 1/30 s: max \|v\| (submerged); roughness | 0.0972 (0.0764); 0.0319 | 0.0786 (0.0687); 0.0184 |
| C plate added mass | 134.9, 138.2 kg/m² | same |
| D piston errors; momentum audits | −4.41e-5, −1.29e-5; 5.75e-4, 1.46e-4 | same |
| E membrane frequency | −0.0554 | same |
| F cloth momentum change vs liquid impulses + weight | 1.75e-5 of H m g | 4.83e-5 (gate 1e-4) |
| G twins, threads, group checkpoint | identical | identical |
| J membrane at large sag | −0.00288 | −0.00283 |
| K bag | +3.30e-4 | same |

The momentum audit (D) is unchanged. F, the agreement between the cloth's momentum change and the impulses the liquid
reports, grew from 1.75e-5 to 4.8e-5 of H m g: with per-element rounding to binary32 each element's gradient no longer
sums exactly to zero (its translation invariance holds to rounding), where m7g rounded the assembled gradient once.
Inferred from the arithmetic, not isolated.

**The 60-run sweep** (M7e §4 flap, 0.05–100 kg/m², 24/40/64 cells, 1/30 and 1/120 s, still water and dam break, 2 s):
all 60 stable. The probe now also reports the largest edge strain against the unstretched placement (macro starts).
The same binary with the m7g model switched in (a temporary switch, removed) reproduces M7g's table to the digit
(14.9, 6.82, 8.64, …). Selected rows (max \|v\| m/s; strain):

| Case | m7g model | m7h |
|---|---|---|
| dam, 24 cells, 1/30, 0.05 | 14.9; 5.95 | 2.06; 0.145 |
| dam, 24, 1/30, 1 | 5.01; 3.02 | 3.21; 0.218 |
| dam, 24, 1/120, 0.05 | 6.82; 1.99 | **13.6**; 1.45 |
| dam, 24, 1/120, 0.2 | 8.64; 0.676 | 5.29; 0.268 |
| dam, 40, 1/30, 0.05 | 4.32; 0.929 | 2.70; 0.040 |
| dam, 64, 1/30, 0.2 | 1.80; 0.461 | 1.86; 0.049 |
| dam, 64, 1/120, 0.2 | 2.10; 0.094 | 1.99; 0.060 |
| still, 64, 1/30, 0.05 | 0.102; 9.7e-4 | 0.076; 1.9e-4 |
| still, 24, 1/30, 1 | 0.096; 4.6e-3 | 0.167; 4.8e-3 |
| still, any, 100 | 0.69–0.77; 0.25–0.27 | same (the heavy flap's own stretch) |

Reading: with the linear model the dam-break flap's strain at 1/30 s was 3–10 times its strain at 1/120 s (0.35/0.12,
0.46/0.09, 0.30/0.09 at 64 cells): the rotation artefact. With the corotational model the two agree (0.038/0.073,
0.049/0.060, 0.10/0.10): the strain no longer depends on the macro step. Above still water every light flap is
0.076–0.17 m/s. The one row that got worse, 24 cells, 0.05 kg/m², 1/120 s (13.6 m/s), is §3's free edge.

### 1.4 Cost

The rotations, the assembly and one more fit per element per step (the indicator) add to each liquid step; the step's
factorisation was already per step. Wave demo (64 cells, 61 frames, 931–980 liquid steps, one process, with the fire
agent's jobs on the machine, both binaries at once): m7g.1 74.5 s for 980 liquid steps (0.076 s per step), m7h.1 72.4 s for 931 steps (0.078 s per step), +2% per step, within the machine's noise. Catch at 128 cells: 434 s for 902 steps (0.48 s per step; M7g's run 0.56 s on a busier machine), 2.3 GB. Peak memory 0.49 GB (wave), 0.57 GB (catch). `surface_elements` carries 6–10
blocks per element (about 2.2 MB per macro step at 33 × 33 vertices).

### 1.5 The demos and the indicator

The m7f indicator (the largest edge turning rate times the macro step) measured the linear model's error. With the
corotational model what remains is the angle by which an element turned beyond its predicted rotation in one liquid
step (δ, measured after each step against the rotation at the step's end); δ²/2 bounds that step's spurious strain,
which the next step's rotation removes (it does not accumulate over the macro step). The progress detail reports
"sheet rotation stretch per step (corotational) up to x%" and warns above 1%.

| Demo (64 cells, committed) | m7g indicator (per macro step) | m7h indicator (per step) |
|---|---|---|
| `liquid_cloth_catch` | 5.2% (warning) | **0.54%** |
| `liquid_cloth_wave` | 62% (warning) | **6.8% (warning)** |
| catch, 128 cells, 33 × 33 (0.8 s) | 25% | 1.5% (warning) |

**The wave still warns.** A temporary per-element trace showed every element with δ > 0.1 rad on the flap's two
bottom rows (the free edge, vertices 221–246), in the surge (t ≈ 1.0–1.1 and 1.9–2.0 s), with vertex speeds 0.4–2.9
m/s: the largest, δ = 0.37 rad in one 2.1 ms step (t = 1.04 s), is an angular acceleration of about 8e4 rad/s². That
is the free edge being kicked by single-step pressure impulses (§3), not the macro step's rotation; shortening the
macro step does not change it (the liquid step is CFL-limited). Within the stated acceptance the catch passes and the
wave does not.

## 2. Thin layers on a sheet (task 2): not fixed

The 3 cm bag (2.4 cells at 24 cells, cotton, 1/120 s, 2 s, `--cup depth 0.03 cells 24 T 2`):

| | Sharp volume above, with correction | Without correction |
|---|---|---|
| m7g.1 (M7g §2.2) | −8.4% | −41% |
| m7h.1 | −8.4% (correction +27% of V0) | −41% (smoothed −33%) |
| m7h.1, interface reach 1 (tried, reverted) | −6.7% (correction +33%) | −48% (smoothed −40%) |

The variant made the sheet-adjacent value single-valued for one more layer: a cell beside the sheet counted as having
an interface of its own only when a cell of the other sign is its direct neighbour (reach 1 instead of 2), so in a
layer two or more cells deep the cells at the sheet keep the sheet's distance every step. The loss without the
correction grew (−41% → −48%) and the correction's work grew too: the alternation of M7g §2.3 is not the whole loss.
Remaining gap to the rigid floor (−0.07% at 2.4 cells, M7g §2.1): about 8 points with the correction, cause as stated in
M7g (pin and reinitialisation alternate; the advection loses volume in between), not resolved. A layer on a sheet
still needs about 4.8 cells.

## 3. Coarse free-edge stair-step (task 3)

What changed (from task 1): the 24-cell dam-break peaks of M7g (14.9 m/s at 0.05 kg/m², 1/30 s; 8.64 at 0.2, 1/120 s)
are 2.06 and 5.29 m/s; the strains fell from 5.95 and 0.68 to 0.15 and 0.27. Part of what M7g traced was the linear
model stretching the crumpled edge within a macro step. What did not: 0.05 kg/m² at 1/120 s, 6.82 → 13.6 m/s (strain
1.99 → 1.45), and the wave indicator of §1.5. At 40 and 64 cells every dam-break peak is 1.4–3.4 m/s.

Mechanism (located, inferred from M7g's trace and the per-element trace of §1.5): a face is closed when the segment
between two cell centres crosses a triangle, and then carries the whole face's pressure jump ρh³Δq to the triangle's
vertices. At the free edge the sheet covers only part of a face it closes (at 24 cells the face is 4.2 cm, the vertex
spacing 3.3 cm), so the light edge vertices receive the load of a whole face, and the face's flux constraint ties a
whole face's flux to them. Design (not built): give a closed face at a free edge a covered fraction c (the fraction of
the face's square on the sheet's side of the free edge, from the edge's line in the face plane); the face is then
split as a body's cut face is (M7b): fraction A(1 − c) open liquid, the closed part c moving with the sheet (J and the
face mass scaled by c), the level set's barrier closed for c ≥ 1/2. Liquid then passes the free edge within a face as
it physically does around it. Effort: the projection's fractions, `SheetCoupling`, `SheetFaceMass`, the barrier,
`BlockSheetLeaks` and the sweeps all assume a sheet face is fully closed; about two to three days with the M7e–M7g
cases rerun. Pinned sheets (the catch, the bags) have no free edge and would not change.

## 4. Underside streams and watertightness (task 4)

### 4.1 The watertightness check (built)

`LiquidModel::SheetBarrierHoles`, for implicitly coupled sheets whose boundary is pinned all round: the cells on the
two sides of every closed face of an interior triangle (no rim vertex) are labelled front or back by the triangle's
normal (a cell given both, at a fold, is left out); a flood fill from the front cells through open faces, allowed only
through those cells and their face neighbours, counts the back cells it reaches. A gap in the closed faces lies between
such cells, so a path of open faces through the sheet is found; a path around the rim is excluded (the rim triangles are
left out). Reported per run in the progress detail ("barrier holes …"), computed at each macro start; it never changes
results.

Test M: a 0.3 m sheet, 8 × 8 quads, tilted 10°, pinned all round, 32 cells: 0 holes; with one quad's two triangles
closing no face (a validation hook, `LiquidSheet::open`), 63 back cells reached. In the solver a crossing-based
barrier is watertight by construction (every segment that crosses a triangle is closed); the check guards the
implementation (the inclusive tests, the deduplication of crossings, faces opened within a step are closed again before
it runs).

### 4.2 The demos

The committed catch at 64 cells (1.6 s) and the catch at 128 cells with a 33 × 33 cloth (0.8 s, 1/120 s, as M7g §5),
both with m7h.1:

| | 64 cells | 128 cells |
|---|---|---|
| Liquid steps | 778 | 902 |
| Barrier holes (largest at any macro start) | 0 | 0 |
| Cells wet across the sheet, attempted and held (summed volume) | 419 (1.8e-4 m³) | 10,080 (5.3e-4 m³) |
| Wet without a wet neighbour (held) | 25,128 | 39,670 |
| Rotation indicator (per step) | 0.54% | 1.5% (warning) |
| Cloth | 778 Newton steps, direct | 1,199 Newton steps, 5,198 CG iterations, 2 halvings |
| Wall time, peak memory | 83 s, 0.57 GB | 434 s, 2.3 GB |

M7g at 128 cells (linear model): 12,394 crossings held, indicator 25%.

**From below** (`rendered/simulation_m7/catch_m7h_128_below_0000.png`, camera at y = 0.04 m under the cloth, t = 0.8 s,
looked at): the cloth's underside is a clean red bowl with a fine speckle of small light fragments on it; two streams
fall from the left and right rims, each starting at the rim where the water runs over the pinned edge, and drops lie on
the floor below the centre. No stream starts from the underside away from the rim. Whether the speckle is liquid
beside the underside (held droplets) or noise of the path tracer at this sample count was not determined; the
measure says no cell below the sheet turned wet through it. **Conclusion:** water runs round the edge; nothing passes
through the barrier (0 holes, every crossing held). Nothing to fix.

## 5. Body–body contact (task 5)

### 5.1 Model (`RigidBody.h`, preview)

- **Geometry.** Spheres, boxes and cylinders (body frame = authoring frame). Exact signed distances; sample points: a
  box's 8 corners and 12 edge midpoints, a cylinder's 64 rim points, a sphere's centre (sphere–sphere is analytic).
  Penetrating points: one body's samples inside the other's shape, both ways. Box–box: one normal for the pair, the axis
  of least overlap among the 15 separating axes (face axes preferred unless an edge axis overlaps 5% less), each
  point's depth measured along it (a box stacked flush on another has its corners on the other's side planes, where the
  signed distance's normal points sideways: the stack fell through without this). Other pairs: each point's distance
  normal, ties between faces at an edge broken towards the other body. Edge–edge contact between samples is not
  detected (stated in the notes).
- **Impulses.** Sequential impulses with accumulated, clamped impulses (`SolveContacts`, 16 passes in point order):
  Newton restitution of each point's approach speed (0 below the planes' resting speed 2|g|h), Coulomb friction over two
  tangents capped by μ times the normal impulse; equal and opposite at each point, so linear and angular momentum are
  exchanged exactly. Pair parameters: restitution min(e_a, e_b), friction √(μ_a μ_b); per body options `restitution`
  and `friction` (default 0.5, the planes' defaults) and `contact` (default on). Points within the slop 2|g|h² count as
  touching (held at the velocity level).
- **Order.** After every body's step and plane contact: up to 16 sweeps of (all touching pairs in one set of rows, then
  each pair moved apart along its deepest normal in inverse proportion to the masses, then the planes again for the
  bodies in a pair). Solving the planes inside the same set of rows converged worse on a stack (drift 1.3 mm, measured)
  and was not kept. Nothing at all happens while no two bounding spheres overlap (`PairsNear`), bit for bit.
- **Plane contact changed for boxes and cylinders.** m6.1 applied each plane's impulse at the deepest support point
  alone (eight times): a single box resting flat on a plane took its support at one corner per substep and in 5 s
  tilted 0.21 rad and walked 1.0 cm (measured). m6.2 resolves all support points below a plane together (within the
  slop) with `SolveContacts`; the box rests (drift 6e-8 m, tilt 5e-7 rad). Spheres keep m6.1's arithmetic.
- **Meshes.** Not covered: rigid.bodies has no mesh distance, so a mesh body among other bodies is refused unless it
  says `contact false` (scene: `contact off`) and then passes through them. The liquid's mesh distance
  (`LiquidBodies.h`) could serve; not done.
- **Determinism and resume.** Stateless (no warm starting), fixed order: twins and stop/restore equal the uninterrupted
  run (test H2b).

### 5.2 Measured (`RigidSolverTest` H)

| Case | Result |
|---|---|
| Two spheres, e = 1, μ = 0, no gravity, head-on (R 0.1 and 0.07 m, 800 and 2000 kg/m³) | \|Δp\| 4.7e-16 of p0, \|ΔE\|/E 7.3e-16; velocities equal the 1-D elastic formulas to 1e-12 |
| Same, oblique (offset 0.09 m) | \|Δp\| 4.0e-16 of p0, \|ΔE\|/E 3.6e-16 |
| One box (0.2 m, 500 kg/m³) on a plane, 5 s | drift 5.8e-8 m, tilt 4.7e-7 rad; after 1 s speed 2.3e-8 m/s, height range 4.4e-8 m |
| Two boxes stacked | drift 9.8e-9 m, tilt 0; speed 6.2e-7 m/s, range 3.5e-9 m |
| Three boxes stacked | drift 8.3e-7 m, tilt 1.8e-6 rad, height change 3.5e-6 m; speed 6.2e-5 m/s, range 2.2e-7 m |
| A sphere and a tilted cylinder dropped onto two stacked boxes | deepest penetration of the sphere into a box 1.0e-5 m; twins and stop/restore byte-identical |
| Two bodies whose bounding spheres never meet | untouched (velocity exact) |

### 5.3 Two bodies colliding under water (`LiquidRigidTest`)

Tank 0.32 m, water to 0.28 m, two spheres R 0.035 m, 1500 kg/m³, at mid depth moving towards each other at 0.4 m/s
each, 24 cells, 0.6 s, `rigid.bodies` owned by the liquid (implicit), a floor plane (restitution 0.3, friction 0.3),
`body_contact` connected:

- stable (no failed step), largest speed 0.57 m/s (the spheres sinking after the collision), least centre distance
  0.0700 m = 2R (no overlap at the frames);
- the x account per macro step, Σ m Δv − H Σ F (F the liquid's offered force), while both are clear of the floor:
  8.7e-17 N s, against a body's own non-liquid x impulse of 0.138 N s (the collision): the contact impulses are equal
  and opposite, and the rest of each body's momentum change is the liquid's;
- with `body_contact` not connected the frames show the same least distance (rigid.bodies resolves the contact at its
  substeps); the liquid's own copies then pass into each other within the macro step.

### 5.4 The liquid's wall stop

`rigid.bodies` offers `body_contact` (field.v1 at t = 0: its planes, then per body the contact flag, restitution and
friction). When a liquid connects it (`connect drop.body_contact -> pool.body_contact`, an owned implicit port), the
liquid's own copies of the coupled bodies stop at rigid.bodies' planes with their restitution and friction and at each
other (`ResolvePairs`, with the non-coupled bodies held fixed), instead of at every domain wall with restitution 0 and
friction 0.3. Unconnected, the m7g wall stop is kept (its bytes). The committed `liquid_rigid_bodies` connects it.
Checkpoints carry the accepted values.

## 6. Second-order velocity at cut-cell solids (task 6): assessed, not built

Today (M7g §6): u_f = u*_f − (q_R − q_L) across a full cell for any open fraction A_f; A_f weights the divergence and the
kinetic energy (Batty, Bertails and Bridson 2007's variational form). The pressure converges at about second order and the
velocity near small cut faces at first (M7a §13.4).

Options:

1. **Cut-cell finite-volume gradient with face-centroid correction.** The gradient at a cut face is taken at the
   centroid of its open part, with a correction from the tangential gradient (q at the centroid offset by
   (x_centroid − x_face) · ∇_t q). The correction couples a face to its tangential neighbours: the operator loses its
   symmetry (DᵀA G no longer DᵀA D), so PCG with the multigrid preconditioner no longer applies; a symmetric version
   needs the correction in both D and G (a wider, still symmetric stencil whose definiteness is not guaranteed for
   small fractions). Not contained.
2. **Ghost-fluid pressure at the solid with a Neumann condition (Ng, Min and Gibou 2009, in form).** Their
   discretisation keeps the symmetric fraction-weighted Laplacian (it is close to what the code has) and reaches second
   order in the pressure; the velocity gradient near the boundary is improved by a separate second-order reconstruction
   of ∇q after the solve (a least-squares fit over the neighbours on the liquid side, with the boundary's normal
   condition), used for u = u* − ∇q on cut faces only. The solve stays SPD and multigrid-PCG; the coupled rigid solve
   (J q, impulses ρh³ J q) must use the same reconstructed gradient on the body's faces, or the exchange no longer
   conserves momentum: the body's impulse would be computed from the variational gradient while the liquid moves with
   the reconstructed one. That mismatch is the crux: either accept a momentum error of the size of the correction, or
   put the reconstruction into J (non-symmetric again).
3. **Keep first order** and leave the worst faces out of transport (`min_transport_fraction`, as now).

Assessment: option 2 is the contained one for static solids (a post-solve reconstruction on cut faces of static solids
only, bodies keeping the variational gradient); it keeps the system SPD and the coupled rigid solve unchanged, at the
cost of the gradient used for static walls and for bodies differing. Effort: about two days for the reconstruction and
its tests, plus M7a case 5s, the M7b added-mass and Archimedes tables and the momentum audit rerun (another day); the
validation module belongs to another agent. Not built in this session; no before/after table. The references document
was not extended for Ng–Min–Gibou in this session (cited from memory; check before relying on it).

## 7. Viscosity with sheets; 3-D momentum audit (task 7)

Not done. Viscosity with sheets stays refused. The 3-D audit with wall impulses was not built; §5.3's x account is a
partial audit for the rigid exchange (it closes to 1e-16 while no wall acts).

## 8. Renders (looked at)

Path traced, 640 × 400, the scenes' cameras unless stated, `rendered/simulation_m7/` (outside git):

- **Wave, t = 1.0 s** (`wave_m7h_0000.png`): the surge has run up the far wall and is breaking back over the flap; the
  flap is bent along the surge in one smooth curve, no kinks or stretched triangles visible; the water surface is broken
  into sheets and droplets around it.
- **Wave, t = 2.0 s** (`wave_m7h_0001.png`): the flap hangs almost straight from its pinned top edge; at the water line
  its bottom edge is ragged, a small zigzag of a few triangles (the free edge of §3); the pool is settling with ripples.
- **Catch 64 cells, t = 0.4 s** (`catch_m7h_64_0000.png`): the cloth sags as a shallow tray holding water; water spills
  over its left and right edges and curls under them in short streams.
- **Catch 64 cells, t = 1.6 s** (`catch_m7h_64_0001.png`): the cloth is almost flat with a thin film; nothing falls.
- **Catch 128 cells, t = 0.8 s** (`catch_m7h_128_0000.png`): thin streams fall from the front corners and edges to a
  splash on the floor, finer than at 64 cells; the cloth is smooth. From below: §4.2.

All renders are noisy at the default sample count; none shows a jagged or stretched cloth except the wave's free edge.

## 9. Bytes: which reference scenes changed

The m7g.1 / m3a.4 / m6.1 binary (built from the tree before any change) and the final binary baked reduced copies,
each in its own private cache directory; content-addressed frame names compared:

| Scene (reduced) | Result |
|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | **6 of 39 differ** (4 pool, 2 drop): the scene now connects `body_contact`; the same scene without that connection bakes identical bytes with the new binary (no body–body overlap and no box below a plane in these frames) |
| `liquid_cloth_flap` (staggered) | identical (27) |
| `liquid_cloth_flap`, one-way (the load connection removed) | identical (27) |
| `liquid_honey_water` (24 cells, 7 frames) | identical (7) |
| `cloth_flag_scene` (31 frames) | identical (32) |
| `rigid_cloth_trampoline` (31 frames) | identical (63) |
| `liquid_cloth_wave` (24 cells, 13 frames; implicit) | **16 of 27 differ**, as intended |
| `liquid_cloth_catch` (24 cells, 13 frames; implicit) | **24 of 27 differ**, as intended |

Changed: the scenes with implicitly coupled sheets (task 1) and `liquid_rigid_bodies` (its new connection). A rigid
scene changes only where bodies meet or a box or cylinder lies below a plane.

## 10. Tests, pins

- **Pins.** `liquid.mac` m7g.1 → m7h.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` m3a.4 → m3a.5 and six
  ports (`ClothSolverTest`); the liquid's port body declares four ports and owned ports body_contact, body_load,
  body_mass_properties, body_state (`LiquidRigidTest`); an implicit sheet six owned ports (`LiquidClothCouplingTest` I);
  `rigid.bodies` has no version pin in its tests. Keyword count 191 and `RewireConnectionTest` 6 unchanged; the cst
  golden lines of the three edited scenes regenerated (no other line changed); no scene and no test added (schedule
  unchanged).
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the fire agent's uncommitted changes in the tree): 452 built warning-free, 451 passed, 1 failed: `SimulationViewportTest` L (a live cloth playback's displayed time under load, frame 79 of 88 shown in phase 2); it passed twice run alone and once through the runner alone. Fire tests were not run.

## 11. Verified, inferred, preview, not run

- **Verified (measured).** §1.2's table; the sweep and the coupling suite; the demos' indicators; the per-element trace
  of the wave; §2's table; §4.1's test; §5's cases; the byte comparison; twins, thread counts and checkpoints (G, H2b,
  LiquidRigidTest's determinism and stop/restore).
- **Inferred.** That the wave's remaining indicator and §3's remaining 24-cell peak come from the free edge's full-face
  load (from the traces, not from a fix); F's growth from per-element rounding; §6's assessment of the options.
- **Preview.** Sheets (thin layers, the free edge, no reference for cloth in a surge); rigid contact (sampled points,
  edge–edge between samples undetected, meshes excluded, sequential impulses not converged to a tolerance).
- **Not run / not done.** Task 3's coverage fraction; task 6's reconstruction; task 7; the VS2022, Android and Xcode
  builds (no file added); the GUI; fire tests.
