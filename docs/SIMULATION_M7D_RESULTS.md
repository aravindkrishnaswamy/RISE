# M7d results: sheets as moving boundaries, closed-mesh bodies, 256³ (`liquid.mac` m7d.1)

2026-11-26. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §2 (cloth in liquid: a prescribed moving
boundary with a lagged load, `preview`), §5 (cost), §7 step 7. Builds on [M7a](SIMULATION_M7A_RESULTS.md),
[M7b](SIMULATION_M7B_RESULTS.md) (bodies, the owned exchange, the label rule), [M7c](SIMULATION_M7C_RESULTS.md) and
[M6](SIMULATION_M6_RESULTS.md) (the group host, `cloth.shell`'s `surface_state` and `surface_load`). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), one solver-side Appendix C row (no member added or
changed).

**Status.** The last M7 step.

- **Sheets** (part 1, `preview`). A thin open surface (a cloth's `surface_state`, or a prescribed rectangle or open
  mesh) is a moving boundary of the liquid: each grid face whose cell-centre segment the sheet crosses is closed and
  carries the sheet's normal velocity, no volume is removed, and the liquid's pressure impulse on those faces is the
  load on the sheet. A port sheet returns it to the cloth as `surface_load` once per macro step through the group's
  existing staggered exchange (the load lagged one macro step). No leakage was found: no dry-side cell fills in any
  of the five tank cases (§3.2). The paddle force has the right sign and is within 13% of the thin box's (§3.3). The
  lagged exchange is unstable for sheets light against the liquid they move (§6): with the load as offered, the
  measured limit is an areal density of about 200–500 kg/m² for a 0.45 m flap in water, about the flap's added mass
  per area and independent of the cell size. Under-relaxing the load (`relaxation` 0.3) lowers it to about 50 kg/m²
  at the cost of a filtered load (time constant about three macro steps). A cotton cloth (0.2 kg/m²) diverges within
  a few macro steps either way. So cloth itself cannot yet be coupled two-way; only heavy flaps can. One-way use (a
  cloth or a prescribed sheet moving the liquid, no load returned) has no such limit.
- **Closed-mesh bodies** (part 2): `shape: mesh` for `liquid.mac`'s bodies. The signed distance in the body frame is
  built once on a lattice, the mass properties come from the mesh by the divergence theorem, and the body frame is
  the one `rigid.bodies` gives the same mesh. A tessellated sphere reproduces the sphere's added mass and Archimedes
  equilibria within the shape's discretisation (§4). Open and inward meshes are refused.
- **256³** (part 3): the dam break costs 2.33 s per step (2.20 s after the one cheap win) and 6.3 GB peak at 256³
  without bodies. That is inside the design's 1–3 s estimate. With four bodies it costs 4.54 s and 7.5 GB, because
  the coupled solve's PCG iterations triple (§5). Profiling with `sample` showed 29% of the thread time waiting at
  joins. The cheap win taken: the velocity extrapolation visits only its front, bitwise the same, −5.7% per step at
  256³ and nothing at 128³. The larger remedies (active-cell compaction, fused passes, a body-aware preconditioner)
  were not started.

Bakes without sheets and mesh bodies publish frames byte-identical to m7c.1 (§7). The version is `m7d.1`.

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` (new) | `LiquidSheet`, the face crossings, the leak guard, the particle crossing test, the per-vertex pressure |
| `src/Library/Simulation/Liquid/LiquidBodies.{h,cpp}` | `MeshDistance` (the body-frame lattice, a nearest-triangle tree, parity sign), `MakeMeshShape`; `BodyShape::Type::Mesh` |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | Sheets in the step (§2.4), `sheetImpulse`, `sheetStepImpulse`; nothing runs without sheets |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp`, `LiquidParameterSchema.cpp` | Parameters `sheets` and mesh bodies, ports, the exchange, checkpoints, label and scope; chunk fields `sheet`, `body_object` |
| `src/Library/Simulation/Liquid/LiquidValidation.{h,cpp}` | `RunAddedMassImpulse` and `RunFloatingStatics` take an optional body shape |
| `src/Library/Simulation/Liquid/LiquidFlow.cpp` | `ExtrapolateVelocity` visits each layer's front only (§5.2; bitwise the same) |
| `tests/LiquidSheetTest.cpp` (new) | Crossings, leakage, paddle, solver (refusals, threads, stop/restore, rollback), groups with `cloth.shell` (17 s) |
| `tests/LiquidMeshBodyTest.cpp` (new) | Mesh shape, refusals, validation against the sphere, solver, following `rigid.bodies` (7 s; `--extended` adds 48 cells) |
| `scenes/Tests/Simulation/liquid_cloth_flap.RISEscene`, `scripts/simulation_m7d_flap.sh` | Demo (§8) |
| Build lists | `LiquidSheets` in all five: make `Filelist` and both strict object lists, Android `CMakeLists.txt` and `rise_sources.cmake`, VS2022 vcxproj (Strict) and filters, the three Xcode Sources phases (`-fno-fast-math -ffp-contract=off`) and the Headers phase |

No `cloth.shell` change: the liquid offers exactly what cloth's `surface_load` accepts (§2.5).

## 2. Sheets

### 2.1 The treatment chosen, and why

Two ways to make a zero-thickness sheet block flow:

- **Thickened.** Give the sheet a numerical thickness of about a cell (an unsigned distance minus half the
  thickness, as a solid) and let the cut-cell machinery of M7b close and weight faces. It reuses the most code, but
  it removes volume. A thickness that closes the faces reliably at every angle is about two cells (control-volume
  fractions) or more than one (area fractions), so the cloth would displace a slab two cells thick. That slab is
  buoyant, has added mass of its own and puts a fluid gap between the cloth and the liquid on each side.
- **Face crossing** (chosen; Guendelman, Selle, Losasso and Fedkiw 2005 in form; checked against the paper 2026-11-26: it sets a Neumann
  condition on a face whose cell-centre segment a triangle crosses, with the object's effective velocity there; this solver
  imposes the normal component only, see M7_REFERENCES §7. The earlier text here and in M7e/M7f misnamed the authors
  "Guendelman, Bridson and Fedkiw"). Close
  every face whose segment between the two cell centres it separates crosses a sheet triangle. Every path of
  neighbouring cells from one side to the other crosses such a segment, so a sheet spanning a region cannot leak
  through the pressure solve whatever its angle. No volume is removed and the fractions of other faces are
  untouched.

Face crossing was chosen because it is the right physics for a thin sheet and it turned out to be robust (the
crossing test is exact against the plane's sign, §3.1). What is lost:

- **Position.** The sheet is located to the cell-centre lattice. The barrier is a staircase within half a cell of
  the sheet, and a sheet moving across a cell centre moves that cell to its other side within one step. The load is
  noisy as a result: for a prescribed paddle the peak force is 3.1 times the force's fundamental amplitude, against
  1.0 for a thin box (§3.3).
- **Tangential stress.** A closed face carries the normal velocity only, so there is no viscous traction, and
  viscosity with sheets is refused (§2.6).
- **Films.** Liquid films thinner than a cell on a sheet are not resolved.
- **A false interface.** Where a sheet holds liquid on one side only, the level set crosses zero at the sheet. The
  level set's own stencils span the sheet and leaked liquid across it until two guards were added (§2.3).

### 2.2 Crossings and the boundary condition

- For each triangle and axis a, the lattice lines along a through the cell centres of the other two axes are
  intersected with the triangle: barycentric coordinates in the projection, inclusive by 1e-10, so that a line
  through a shared edge or vertex is never missed. A crossing at coordinate s closes the interior face between the
  two cell centres that bracket s.
- A face crossed more than once (shared edges, folds, two sheets) keeps the crossing nearest its centre; ties go by
  sheet and triangle. The list is sorted, so it does not depend on the thread count.
- A closed face's open fraction is set to zero and its fraction before closing is kept. It is restored before the
  next step's bodies rebuild their cut cells, and the faces are closed again at the sheet's new pose.
- The face carries the sheet's normal velocity (v·n)n_a, with v interpolated barycentrically and n the triangle's
  unit normal, as a solid face velocity. The normal component makes the staircase's net flux the sheet's normal
  flux (Σ h²(v·n)n_a ≈ (v·n)A) without pumping liquid along a sheet that slides in its own plane.
- Before the extrapolations at the start of each step, closed faces are set to that velocity and marked known, so
  the transport sees the sheet's motion there.

### 2.3 Two leak guards (measured)

The pressure solve cannot carry liquid across a closed face. The level set and the marker particles can. Without
the guards, a sheet tilted 0.3 rad holding liquid on one side, at 32 cells in 2-D, put 1.3% of the liquid's
smoothed volume on its dry side within 4 s. On inspection, no dry cell had φ < 0. The dry cells beside the sheet
crept towards φ = 0, and the smoothed volume measure counted them as half full, so the volume correction drained
the wet side. In 3-D (24 cells, sloshing), two dry cells six cells from the sheet filled.

1. **Leak guard** (`BlockSheetLeaks`, after the volume correction, with sheets only):
   - A cell beside a closed face that was air at the step's start, and whose φ fell, keeps its old value unless a
     neighbour across an open face was liquid at the start.
   - Anywhere in the grid, an air cell that turned liquid without such a neighbour keeps its old value. On the dry
     side the extrapolated velocity copies the wet side's flow across the sheet and carries the false interface's
     near-zero φ away from it.

   Liquid reaches a cell through its faces at CFL ≤ 1/2, so the rule removes nothing physical except a droplet
   re-seeded from particles alone, which this also prevents near sheets.
2. **Particles.** A marker particle whose cell changed in a step is removed when every axis-by-axis path of
   neighbouring cells between its old and new cell crosses a closed face. In the 3-D sloshing case that removed
   13,367 particles over 1 s.

Tried and dropped: projecting the cell-centred advecting velocity beside the sheet onto the sheet's tangent plane. It
halved the leak, but without the volume correction the wet side then lost 47% of its volume in 4 s, against 10%
without it.

### 2.4 The step

The m7c step with, only when there are sheets:

1. The closed faces get the sheet's normal velocity, marked known, before the extrapolations.
2. Marker particles carried through a sheet are removed after their advection.
3. After the level-set advection, the closed faces open again. Bodies move and rebuild their cut cells. Then each
   sheet moves to the step's end and its faces close: a prescribed sheet follows its motion, and a port sheet drifts
   with the velocity it was accepted with (x += dt v).
4. The projections take the sheets' solid velocities (overlaid on the bodies' when there are bodies).
5. The leak guard runs after the volume correction.
6. After the step, each closed face of axis a between cells L and R adds ρh³(q_L − q_R)e_a to the impulse of the
   crossed triangle's vertices, by the barycentric weights (q is the step's scaled pressure, 0 in air). This is
   M7b's ρh³ J q for a fully closed face, so the liquid's impulse on the sheet is minus the sheet's on the liquid.
7. The sheet's speed bound joins the time-step limit.

### 2.5 Ports and the exchange

- `sheets: [{name, motion: port, port (default sheet_state), load_port (default sheet_load), relaxation (default 1)}]`.
  The accepted port is declared exactly as `cloth.shell` offers `surface_state` (`mesh.v2`; normal, position,
  velocity). The offered one is declared exactly as cloth accepts `surface_load` (`field.v1`; pressure, traction;
  domain stated per payload). `PortsCompatible` passes both ways (`LiquidSheetTest` D).
- At each macro step's start (each segment's when free-running with a `port_source` cached feed), the liquid
  accepts the surface. It fixes the topology at the first accept and refuses a change, takes positions and
  velocities, and closes the sheet's faces. At the end it offers, per vertex, the pressure p_q = −(F_q · a_q)/|a_q|².
  Here F_q is the vertex's impulse over the macro step divided by its length, and a_q is a third of the adjacent
  triangles' area vectors, from the drifted positions. Cloth applies −p_q a_q on its own current positions, so the
  normal force is exactly what the liquid measured and the tangential part is dropped. The payload carries the
  `pressure` channel only; an absent traction is zero to cloth.
- `relaxation` w < 1 offers w p_new + (1 − w) p_last instead (§6).
- **Member order.** The liquid must read the sheet's state at the macro step's start: put it first in Gauss–Seidel
  `member_order`, or use Jacobi. With cloth first, the liquid reads cloth's end-of-step state and drifts it a step
  further (not detected; stated in the notes and the README).
- **One-way.** Leave `sheet_load` unconnected, or feed `sheet_state` from a cloth cache with `port_source`. Cloth's
  frames are then those of the cloth alone (tested).
- **Prescribed sheets**: a rectangle `size` [sx, sy] in its frame's xy plane (normal +z) with `divisions` [nx, ny], or
  an open mesh `vertices`/`triangles`, moving as a prescribed body moves (`position`, `orientation`, `velocity`,
  `angular_velocity`, `amplitude`, `frequency`, `phase`). Their total force is `sheetStepImpulse` (model level). There
  is no output for them; nothing renders them.

### 2.6 Refused, and preview

- Refused:
  - viscosity with sheets (the viscous solve does not see a sheet, so viscous stress would cross it and no
    traction would be returned);
  - duplicate sheet names;
  - port names that collide with the liquid's other ports;
  - a non-positive size;
  - `relaxation` outside (0, 1];
  - unknown keys;
  - a port sheet with no port, at its first step.
- With sheets the label is `preview`, with `fidelityLimit` "sheets: a thin surface as a face-crossing moving
  boundary with a lagged pressure load (preview, not certified)", the condition `sheets` {prescribed, port}, and a
  scope sentence that names what is not covered. In a group with cloth, the staggered exchange is preview anyway
  (M6's rule).
- No evidence case was added: nothing earns predictive.

### 2.7 Checkpoints

Only with sheets: the header holds each sheet's sizes, topology digest and the open exchange. The stream holds, after
the particles, each sheet's triangles, positions, velocities, accumulated vertex impulse and last offered pressure. A
restore reapplies the faces from the restored positions before the bodies' geometry. Streams without sheets are
m7c's.

## 3. Sheet tests (`LiquidSheetTest`, 17 s)

### 3.1 Crossings

| Check | Result |
|---|---|
| A plane across a 32³ grid at x = 0.5 + 0.3h, vertical and tilted 0.3 rad: closed faces against the plane's sign over every interior face | 1024 and 1312 closed, 0 differ |
| 1 and 8 threads | identical lists |
| A half plane: only faces whose segment it crosses | 16 × 32 faces, all below its edge |

### 3.2 Leakage: a sheet held across a tank, liquid on one side

Unit tank, sheet through (0.5 + 0.3h, 0.5) facing +x, tilted about z. "Rest": liquid up to y = 0.5 on the wet side,
from a level set set by hand. "Sloshing": a column 0.6 high released two cells from the sheet, which runs up and
sloshes against it. Dry-side volume with the cell-fraction Heaviside clamp(½ − φ/h); "smoothed" is the solver's
LiquidVolume measure (ε = 1.5h). Default numerics (particles, volume correction).

| Case | Duration | Dry cells with φ < 0 | Dry-side volume / liquid (start → max) | Smoothed dry-side tail | Wet side Δ | Load / ρgH²/(2 cos θ) − 1 (mean, 2nd half) |
|---|---|---|---|---|---|---|
| 2-D, 32, vertical, rest | 2 s | 0 | 1.88e-2 → 1.87e-2 | 2.4e-2 | +4.6e-3 | +0.7% |
| 2-D, 32, tilted 0.3, rest | 2 s | 0 | 7.72e-3 → 7.72e-3 | 1.4e-2 | +3.1e-3 | +0.3% |
| 3-D, 24, tilted 0.3, rest | 1 s | 0 | 9.09e-3 → 9.08e-3 | 1.7e-2 | +2.5e-3 | +1.2% |
| 2-D, 32, tilted 0.3, sloshing | 2 s | 0 | 0 → 0 | 1.3e-3 | −1.5e-2 | (dynamic) |
| 3-D, 24, tilted 0.3, sloshing | 1 s | 0 | 0 → 6.9e-4 | 2.4e-3 | −1.5e-2 | (dynamic) |

- **Leak rate.** No cell on the dry side filled in any case, and the dry side's volume never grew beyond its start
  in the rest cases. The resolution of this check is one cell's fraction, and nothing crossed. The rest cases'
  nonzero dry share is the half cell the interface at the sheet straddles: φ is the distance to the plane, so a dry
  cell centre within h/2 of it counts as partly full.
- **Wet-side volume.** +0.25% to +0.46% at rest and −1.5% sloshing in 1–2 s. The smoothed tail of the false interface
  on the dry side (1–2% of the volume) is counted by the global volume correction as liquid. That is not leakage but
  a volume error of the wet side of that size.
- **Load.** The hydrostatic thrust on the sheet is within 1.2% at rest, tilted or not.

### 3.3 Paddle against the thin box of M7b

A 0.3 × 0.3 m flat sheet at the centre of a closed unit tank full of liquid, without gravity, oscillating normal to
itself (x = 0.02 sin 2πt). Against it, a prescribed M7b box of the same motion, two cells thick. The force's Fourier
sine coefficient over two periods gives the added mass, F = m_a A ω² sin ωt.

| Grid | Sheet m_a | Thin box m_a | Ratio | Peak force / fundamental: sheet, box |
|---|---|---|---|---|
| 32³ | 15.78 kg | 13.96 kg | 1.130 | 3.08, 1.00 |
| 48³ | 13.68 kg | 14.20 kg | 0.963 | 2.9, 1.0 |

The sign is right (the force opposes the acceleration), and the order is that of a disk of equal area in unbounded
liquid, (8/3)ρR³ = 12.9 kg (the tank adds some). The box is 4 cm thick at 32 cells and displaces liquid, so it is not
the sheet's exact equal. The sheet's force has an in-phase part of −4.9 to −5.5 N against the box's −1.7: the energy
exchanged as cell centres change side. The test gates the ratio within 20% at 32 cells.

### 3.4 Solver and group

| Check | Result |
|---|---|
| Label, condition, scope, version | preview, `sheets: [prescribed]`, the M7d scope sentence, m7d.1; without sheets no `sheets` key and the m7c label |
| Ports against `cloth.shell`'s | `PortsCompatible` both ways |
| 1, 3 and 8 threads and a twin (prescribed sheet, dam break, particles) | identical bytes |
| Stop mid-segment and restore (in memory, streamed); rollback | identical to the uninterrupted run |
| Refusals (§2.6), the chunk field `sheet` | as listed |
| Group cloth (500 kg/m², 9 × 9 vertices, pinned top edge) + liquid (24 cells), staggered Gauss–Seidel, 0.6 s | completes; twins identical; liquid 1 thread and cloth 1 thread give the bytes of 8 and 4; both members preview; the curtain moves 0.19 m, finite |
| Group checkpoint after six macro steps | resumes byte for byte (both members) |
| One-way (no load connected) | cloth's frames are the cloth's alone |

"Conserves what it should":

- The liquid's volume is held by its volume correction, with the error of §3.2.
- The force cloth applies is exactly the normal part of the impulse the liquid measured over the macro step, by
  construction of p_q (§2.5).
- The liquid's impulse on the sheet is minus the sheet's on the liquid, by the same telescoping as M7b's bodies.

A momentum audit of liquid plus cloth across the lagged exchange was not built.

## 4. Closed-mesh bodies

### 4.1 Method

- `bodies: [{shape: mesh, vertices, triangles | scene_input, …}]`, and the chunk field `body_object <object> [options]`.
- **Mass properties.** `Rigid::ShapeMass` applies the divergence theorem over the mesh's tetrahedra: volume,
  centroid, inertia. It refuses an open or inconsistently oriented mesh (edges welded at 1e-6 of the extent) and an
  inward one (no positive volume). The body frame is the centroid and `Rigid::PrincipalAxes`, the frame
  `rigid.bodies` gives the same mesh: a body at authoring pose (p, Ra) has its centre of mass at p + Ra c and its
  orientation at Ra P. Mass is density × the mesh's volume, or `mass`. A scene-input mesh takes its pose from the scene
  (`at` and `orientation` refused).
- **Distance.** A body-frame lattice of spacing h/2 covering the mesh's box plus 6h. At each node, the exact distance
  to the nearest triangle (a bounding-volume tree, median splits, deterministic). The sign comes from the parity of
  crossings along +x, offset off the lattice. Between nodes it is trilinear (O(h²κ)). Beyond the lattice it returns
  6h plus the distance to the lattice, a lower bound of the true distance. Building it takes 0.01–0.03 s for 1280 to
  5120 triangles at 32 cells. The rest of M7b (cut cells, control-volume fractions, coupling, wall contact through the
  mesh's vertices as support points) is unchanged.

### 4.2 Validation (`LiquidMeshBodyTest`)

An icosphere of radius 0.15 with 1280 triangles (volume 0.86% below the sphere's) against the analytic sphere, same
grids and offsets as M7b's 9a and 9d. The added-mass errors are each against the potential-flow value for the
body's own volume. Tolerances are 1.25 × observed.

| Check | Sphere | Mesh | Difference | Tolerance |
|---|---|---|---|---|
| Distance near the surface, max over 4000 points within 0.1 m | — | — | 0.049 h | 0.0625 h |
| Moments / 0.4 m R² − 1 | — | < 2% | — | 2% |
| 9a added mass, 32 cells | −2.21e-2 | −2.93e-2 | −7.2e-3 | 9.0e-3 |
| 9a, 48 cells (`--extended`) | −1.13e-2 | −1.56e-2 | −4.4e-3 | 5.5e-3 |
| 9d Archimedes offset / R, ratio 0.1, 32 | +1.11e-2 | +7.00e-3 | −4.1e-3 | 5.1e-3 |
| ratio 0.5, 32 | +3.95e-4 | −3.26e-3 | −3.7e-3 | 4.6e-3 |
| ratio 0.9, 32 | −9.53e-3 | −1.78e-2 | −8.3e-3 | 1.04e-2 |
| ratio 0.1 / 0.5 / 0.9, 48 (`--extended`) | 4.77e-3 / 1.96e-4 / −4.07e-3 | 1.99e-3 / −1.48e-3 / −6.41e-3 | −2.8e-3 / −1.7e-3 / −2.3e-3 | 3.5e-3 / 2.1e-3 / 3.0e-3 |

- **What the difference is.** At 32 cells the added-mass difference does not depend on the tessellation: −7.2e-3
  with 1280 triangles and −7.0e-3 with 5120. That is the distance lattice's interpolation (spacing h/2). At 48 cells
  it falls to −4.4e-3 with 1280 triangles and −3.2e-3 with 5120, so there the tessellation shows (the 1280-triangle
  polyhedron's surface lies up to 0.6% of R inside the sphere). The Archimedes differences fall the same way at
  ratio 0.9 (−8.3e-3 → −2.3e-3 R), but not at ratio 0.1 (−4.1e-3 → −2.8e-3). At 32 cells they are within 1.6 times
  the largest distance deviation (0.049 h = 1.0e-2 R). The mesh body is a sphere to within its discretisation (the
  lattice and the tessellation) at the liquid's resolution.
- **Refusals.** An icosphere with one triangle removed ("closed"); with every triangle reversed ("inward"); through
  Configure the error names the body.
- **Solver.** An inline mesh body offset from its authoring origin and rotated 0.4 rad has its centre of mass at
  position + R c to 1e-9. Its effective parameters reproduce themselves, and 1 and 8 threads publish identical bytes.
- **Following `rigid.bodies`.** A port body and `rigid.bodies`' body built from the same off-centre, anisotropic mesh
  pass the liquid's initial-pose check (position, orientation, velocity within 1e-6) and run the owned exchange.

## 5. 256³ and performance

### 5.1 Measurements (Apple M4 Max, 16 threads)

The dam break of M7a §8 (1 m cube, column 0.3 × 0.6 × 1 m, a box obstacle, particles on), measured 0.05 s into the
run (81 steps; 0.15 s, as at 64³ and 128³, would have taken another 7–14 minutes per case), mean of 8 steps. The
bodies are four dynamic bodies of density 500 (two spheres of radius 8 cm, two 16 cm cubes) inside the column.

| Grid | Bodies | s/step | Extrapolation | Level set | Particles | Momentum | Bodies | Pressure (PCG iterations / step) | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|
| 64³ | 0 | 0.073 | 0.007 | 0.007 | 0.004 | 0.014 | — | 0.040 (23.9) | 239 MB |
| 256³ | 0 | 2.332 | 0.219 | 0.248 | 0.062 | 0.445 | — | 1.359 (31.8) | 6.27 GB |
| 256³ | 4 | 4.544 | 0.219 | 0.248 | 0.062 | 0.447 | 0.116 | 3.453 (95.9) | 7.48 GB |

- **Against the estimate.** 2.3 s per step at 256³ without bodies is inside the design's 1–3 s (§5). The step is
  0.62 ms (wave limit), so a 10 s bake is about 16,000 steps, about 10.4 hours. The estimate of about 7 hours
  assumed 1,200 steps per second.
- **Memory.** 6.3 GB peak is 375 bytes per cell. `memoryEstimate` (450 B/cell + particles) declares 10.0 GB for this
  configuration.
- **Scaling with the grid.** 256³ / 64³ = 32× the time for 64× the cells.
- **Bodies at 256³.** The coupled pressure solve's iterations triple (32 → 96 per step). At 64³ and 128³ M7b
  measured about four more per solve. The rank-6 body term weakens the multigrid preconditioner more as the bodies
  span more cells (41 cells across here). That makes bodies cost +95% per step at 256³, against +35–79% at 128³.

### 5.2 Profile (`sample`, 15 s at 256³ during the timed steps)

16 threads × 12 s, 85,365 samples (top of stack):

| Where | Share |
|---|---|
| Multigrid smoother (red-black half sweeps) | 22% |
| Threads waiting at the pool's joins (`__psynch_cvwait` 19%, `swtch_pri` 10%) | 29% |
| WENO5 one-sided derivatives (level set and momentum) | 13% |
| Velocity extrapolation | 11% |
| Multigrid operator, restriction, prolongation | 8% |
| Face advecting velocities, trilinear sampling, the limited stage | 5% |
| Everything else (assembly, PCG vectors, reinitialisation, particles, the surface) | 12% |

- **The joins.** Nearly a third of the thread time is spent waiting. Many passes touch only the liquid's rows (the
  active spans of M7a) and the chunks are uneven, or the passes are short and memory bound, so threads wait at each
  join. The fix is the active-cell compaction and fewer, fused passes that M7a suggested. That is a refactor of most
  passes (each must stay bitwise the same at any thread count), so it was not started.
- **The extrapolation** (taken, a cheap win). Each layer scanned every face of the axis and copied the known mask:
  ten layers × three axes × 16.8M faces per step at 256³, with most air faces having no known neighbour. A layer
  can change only the unknown faces with a known neighbour, so `ExtrapolateVelocity` now visits that front alone:
  the first from one scan, the next as the unknown neighbours of the faces just filled. Each value is the same
  expression of the same layer-start values, so the bytes do not change (§5.3).

  | Grid | Before | After | Extrapolation phase |
  |---|---|---|---|
  | 128³ | 0.310 s/step | 0.304 | 0.030 → 0.030 s |
  | 256³ | 2.332 | 2.199 (−5.7%) | 0.219 → 0.138 s (momentum, which holds the second stage's extrapolation, 0.445 → 0.394) |

  The 0.05 s warm-up at 256³ took 183 s instead of 194 s. At 128³ the liquid fills a larger share of the box and the
  front is as costly as the scan.
- **Bodies.** With bodies the pressure solve dominates (3.45 of 4.54 s per step). The remedy is a Woodbury
  correction of the multigrid preconditioner for the rank-6 body terms: (B − BU(C⁻¹ + UᵀBU)⁻¹UᵀB), with 6 V-cycles
  per body per projection. It could save most of the 64 extra iterations per step at 256³. M7b judged it not worth
  it at 64–128³, where it would save about four per solve. It changes the iterates and hence the published bytes of
  every bake with bodies, so it was not built.

The m7d binary's step costs without the change matched m7c's: the 256³ warm-up took 194.0 s against 194.1 s.

### 5.3 Bit-identity

The extrapolation change leaves the published bytes unchanged:

- §7's two baselines (a dam break with fields and particles, a tank with a dynamic and a prescribed body) were
  baked again after it, and every content-addressed frame file name matches m7c.1's;
- the validation tests (`LiquidValidationTest`, `LiquidRigidValidationTest`, `LiquidPhysicsValidationTest`) pass
  with their tolerances of 1.25 × observed;
- the thread-count tests pass (`LiquidSolverTest` C, `LiquidRigidTest` C, `LiquidSheetTest` D).

## 6. The lagged exchange: where it works and where it fails

**Setup.** A flap 0.44 × 0.45 m (12 × 12 vertices, `cotton-plain-weave` with its areal density overridden), pinned
along its top edge at y = 0.55. It hangs in a flume 1 × 0.6 × 0.4 m filled to 0.4 m and starts tilted 17° from the
vertical at rest. The liquid steps first, Gauss–Seidel, staggered, 1 s. "Blew up": a cloth vertex faster than
20 m/s (the run stopped). The load's roughness is the second difference of the flap's mean offered pressure over
the macro steps, Σ|F_{k+1} − 2F_k + F_{k−1}| / Σ|F_k|: about 0.3–0.6 for a smooth load, 1–2 for the noise of cell
centres changing side (a prescribed paddle's level, §3.3), above 2 for an alternating load.

| Areal density (kg/m²) | 40 cells, 1/30 s, w 1 | 1/30 s, w 0.3 | 1/120 s, w 1 | 1/120 s, w 0.3 | 24 cells, 1/30, w 1 | 64 cells, 1/30, w 1 |
|---|---|---|---|---|---|---|
| 0.2 (cotton) | blew up at step 2 | blew up at step 2 | blew up at step 4 | blew up at step 4 | — | — |
| 2 | blew up (3) | blew up (3) | blew up (4) | blew up (4) | — | — |
| 20 | blew up (4) | blew up (12) | blew up (8) | blew up (16) | — | — |
| 50 | blew up (5) | stable, 1.04 | blew up (8) | stable, 0.28 | blew up (5) | blew up (5) |
| 100 | blew up (13) | stable, 0.46 | blew up (16) | stable, 0.34 | blew up (8) | bounded, 3.46 (max 9.2 m/s) |
| 200 | bounded, 3.52 (max 5.6 m/s) | stable, 0.54 | blew up (28) | stable, 0.42 | bounded, 3.66 | bounded, 3.84 (max 5.0 m/s) |
| 300 | bounded, 3.66 | stable, 0.43 | blew up (32) | stable, 0.46 | bounded, 3.66 | bounded, 3.79 |
| 500 | stable, 1.02 | stable, 0.33 | bounded, 2.20 | stable, 0.25 | bounded, 2.13 | stable, 1.46 |
| 1000 | stable, 1.01 | stable, 0.31 | stable, 1.48 | stable, 0.30 | stable, 1.91 | stable, 0.88 |
| 2000 | stable, 1.08 | stable, 0.35 | stable, 1.50 | stable, 0.33 | stable, 1.38 | stable, 0.94 |

**Reading.**

- **The amplification.** The staggered exchange is the classic partitioned scheme. The liquid moves the sheet at
  the velocity it was given and returns the impulse that took. That impulse is set by the sheet's velocity change
  against the liquid's added mass m_a, so in one exchange the sheet's velocity error is multiplied by about
  −m_a/m_s (Causin, Gerbeau and Nobile 2005; M7b §5). It diverges when the areal density is below the added mass
  per area.
- **The added mass.** For a plate of submerged length L ≈ 0.3 m moving normal to itself in 2-D flow,
  m_a ≈ ρπL/4 ≈ 240 kg/m²; a pinned plate rotating about its top has less. The measured threshold is between 100
  and 500 kg/m² at w = 1.
- **Not the cell size.** The threshold is the same at 24, 40 and 64 cells (h = 4.2, 2.5, 1.6 cm; ρh = 42, 25,
  16 kg/m²). The sheet's global mode decides, not the grid-scale modes. In the task's terms:
  ρ_s t_s / (ρ h) must exceed about 10–30 at these resolutions, because ρ_s t_s / (ρ L) must exceed about 0.5–1.5.
- **Not the macro step.** Shortening it does not help and makes it worse (200–300 kg/m² are bounded at 1/30 s and
  blow up at 1/120 s): the classic property of the added-mass instability. A longer step averages the impulsive
  pressure over more of the flap's own (stabilising) dynamics.
- **Under-relaxation.** The offered load becomes w p_new + (1 − w) p_last. The error recurrence becomes
  e_{k+1} = (1 − w(1 + r)) e_k with r = m_a/m_s, which is stable for w < 2/(1 + r). With w = 0.3 that is r < 5.7:
  m_s > about 40 kg/m² for m_a ≈ 200–240 kg/m². That is what was measured (50 stable, 20 blew up).
- **The cost of relaxing.** The offered load is an exponential average of the measured one, with time constant
  H/w (3.3 macro steps at w = 0.3), on top of the one-step lag. The flap's response is delayed and smoothed, and the
  force cloth receives no longer equals the impulse the liquid measured over the same step (it does over long
  times). For a light sheet the required w ≈ 2m_s/m_a (0.002 for cotton) would delay the load by hundreds of macro
  steps, so it is not offered as a remedy.
- **Added-mass damping** (adding an estimate of the added mass to the structure's inertia and the same term to its
  load) needs the cloth solver to accept an extra mass per vertex. That is not a minimal change, and it was not
  built.

**Stable range, stated.** For this exchange with the load as offered (w = 1), a sheet in water must be at least about
as heavy per area as the liquid's added mass per area for its size: m_s ≳ ρL/2 with L its wetted extent (the
stable rows above were 500 kg/m² for L ≈ 0.3–0.45 m). With w = 0.3 the limit is about ρL/8. Any real cloth (0.1–1
kg/m²) is two to three orders of magnitude below either limit. Two-way cloth-in-water coupling needs the implicit
treatment (Robinson-Mosher et al. 2008), which remains the later step of design §2. One-way use is unaffected.

## 7. Bakes without sheets

The step, the projections, the checkpoints and the effective parameters take m7c's code paths when there are no
sheets: every new branch is guarded by an empty sheet list, and `sheets` is written back only when present. The two
baselines of M7c §9, baked with m7b.1 and m7c.1 before, were baked again with m7d.1 in a private directory (`rise_sim
bake <scene> pool`). Every content-addressed frame file name matches:

- the dam break (24 cells, a box obstacle, fields and particles; 36 payloads);
- the tank with a dynamic sphere and a prescribed spinning, oscillating box (32 cells, fields; 30 payloads).

Mesh bodies do not touch the analytic shapes' arithmetic. The scope sentence with bodies now names closed meshes, so
the evidence digest of any configuration with bodies changes. The version changes every key anyway.

## 8. Demo

`scenes/Tests/Simulation/liquid_cloth_flap.RISEscene` and `scripts/simulation_m7d_flap.sh [resolution]` (scene and
caches under `~/.cache/rise-m7d`, frames to `rendered/simulation_m7/flap_<res>*.png`).

- **Scene.** A flume 1 × 0.6 × 0.4 m with a dam-break column 0.3 × 0.45 m against its −x wall. A red flap 0.3 m
  wide and 0.45 m long (13 × 19 vertices) hangs from a rod at x = 0.61, y = 0.55, pinned along its top edge, with
  5 cm gaps to the side walls. The flap is `cloth.shell` with `cotton-plain-weave` and `material_density 50`: cotton
  diverges (§6), so the flap stands for a thick rubber mat. The liquid carries it as `sheet flap port relaxation
  0.3`. `simulation_group flume`: staggered Gauss–Seidel, liquid first, macro step 1/120 s.
- **Run.** 64 cells along x (1.56 cm). The group bake took 62 s for 61 frames (2 s) on 16 threads, and both members
  label preview. Six frames (t = 0, 0.4, …, 2.0 s) path traced at 640 × 400 with 16 samples took 13 s; denoised
  copies are written beside them.
- **Macro step.** It is written as `0.008333333333333333`, which parses to the double nearest 1/120 so that four
  steps make a frame interval exactly. With `0.00833333333333333` (one digit fewer) the group produced sliver
  macro steps at every frame boundary. The sheet's load is the step's impulse divided by the step's length, so the
  impulsive part over a sliver became enormous and the bake slowed about a hundredfold (2 frames in 4 minutes).
  This is not guarded: a macro step that does not divide the frame interval exactly must not be used with sheets
  (§10).

**What the frames look like** (looked at; the water is drawn as a block without tank walls, as in M7b's demo):

- t = 0: the column and the flap hanging straight.
- t = 0.4: the front has run under the flap (its bottom edge is 10 cm above the floor) and up the far wall. The
  flap's lower edge is pushed slightly downstream.
- t = 0.8: the run-up has come back. The water reaches the rod on the far side, the flap is submerged and bowed
  upstream, and there is a ragged splash along the top.
- t = 1.2: the returning surge lifts the flap. Its lower half swings upstream nearly to the horizontal, lying on the
  surface.
- t = 1.6–2.0: the flap swings back and hangs at an angle in the sloshing water, water on both sides of it (around
  and under it).

The flap moves with the water, plausibly and without visible oscillation from macro step to macro step. No liquid
appears where the flap blocks it, and the sheet shows no visible gap. The water's surface near the flap is coarse,
and the half-cell staircase is not visible at this distance. As physics it is a heavy mat, not cloth, and the load is
filtered by the relaxation (§6). The flow under the flap and through the side gaps is the bulk of what moves it.

## 9. Pins and change log

- `liquid.mac` version `m7c.1` → `m7d.1`. `LiquidRigidTest`'s version check is updated; the keys of liquid caches
  change, but frames without sheets do not.
- New tests: `LiquidSheetTest` (wide:4, 17 s) and `LiquidMeshBodyTest` (wide:3, 7 s; dual mode in
  `run_all_tests.{sh,ps1}`); `tests/test_schedule.tsv`.
- `CstDeriveGoldenTest`: one hand-added line for `liquid_cloth_flap.RISEscene` (2505 bytes, 41 lines), from `--dump`
  with the `Job::` log lines stripped (the method reproduces `liquid_rigid_bodies`' committed line exactly). Until the
  scene is tracked, the test reports it STALE. No existing line drifted (0 DRIFT).
- `SceneEditorSuggestionsTest` (keyword count 191) and `RewireConnectionTest`: unchanged. There is no new chunk
  keyword, and `body_object` and `sheet` are text fields: `body_object` is text as `rigid_simulation`'s is, because
  options follow the object. An object-reference field would have moved the rewire count and refused the options.
- `IJob`, `ChunkCategory`: unchanged. Contract: one solver-side Appendix C row; no member added or changed.
- Docs: `SIMULATION_IN_RISE_PROPOSAL.md` status, `SIMULATION_M7_DESIGN.md` §7 step 7, `Parsers/README.md`
  "Liquid (M7)".

## 10. Open, and not verified

- **Two-way cloth in water** is unstable for any real cloth (§6). The implicit coupling (the liquid's pressure solve
  including the cloth's normal degrees of freedom, or Robinson-Mosher et al. 2008) is not built. Neither is an
  added-mass damping term, which needs a cloth-side change.
- **Sheet accuracy** is not certified:
  - no case bounds the load's error;
  - the staircase locates the sheet to half a cell;
  - the load is noisy (peak 3× the fundamental for a paddle), with an in-phase part as cell centres change side;
  - no tangential traction; viscosity with sheets refused.
- **Gauss–Seidel with cloth first** is accepted but drifts the sheet one macro step too far. It is not detected.
- **A macro step that does not divide the frame interval** gives sliver steps. A sheet's load over a sliver is its
  impulse over a vanishing span: in the demo the bake slowed a hundredfold. It is not detected or guarded (§8).
- **The false interface** at a sheet holding liquid on one side puts 1–2% of the liquid's smoothed volume on the dry
  side of the volume measure. The volume correction then misplaces that much of the wet side's volume. A side-aware
  volume measure was not built.
- **Leak guard side effects.** A droplet re-seeded by particles alone cannot appear anywhere in a run with sheets
  (the guard is global). The effect on splashes was not measured.
- **Rendering prescribed sheets**: there is no output for them.
- **Sheets and bodies on the same face**: the sheet's velocity wins and the body's J still counts the face. Untested.
- **A momentum audit** across the cloth exchange was not built.
- **Mesh bodies**:
  - the distance lattice is not refined with the body (h/2 of the liquid's grid);
  - a very large body at 256³ would need up to 2^28 nodes, the limit;
  - meshes with self-intersections are not detected (ShapeMass checks closedness and orientation only);
  - rotating mesh bodies have no rotational reference case (as for M7b's shapes).
- **256³**:
  - measured over 8 steps 0.05 s into the run, not over a bake;
  - one 12 s profile;
  - the joins (29% of the thread time), the active-cell compaction and the Woodbury preconditioner correction for
    bodies (§5.2) are not addressed;
  - the 256³ step with bodies was not re-measured after the extrapolation change.
- **Not run**:
  - the VS2022, Android and Xcode builds (lists edited by script; the make build compiles everything);
  - the GUI;
  - a full 2 s bake at 128³ with a flap.
- **Not verified**:
  - (checked 2026-11-26: Guendelman, Selle, Losasso and Fedkiw 2005, M7_REFERENCES §7);
  - the plate added-mass estimate ρπL/4 (standard: the added mass of a 2-D flat plate of width L is ρπL²/4 per unit length, i.e. a
    cylinder of diameter L; not re-derived here, not found on a fetched page; see M7_REFERENCES §4);
  - bitwise reproducibility across platforms (per platform, as M7a).
