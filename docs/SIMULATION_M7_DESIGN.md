# M7 design: liquid, the third solver

2026-11-25. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §5 M7. Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md). Based on a literature survey; items marked
*to verify* come from recollection and must be checked against the sources before they become pass criteria.
*Checked 2026-11-26:* every reference value, formula and paper description is listed with its source and verdict in
[SIMULATION_M7_REFERENCES.md](SIMULATION_M7_REFERENCES.md).
**The method choice below is a recommendation awaiting the owner's confirmation; only the method-independent
prerequisites (§6) are being built now.**

## 1. Method

| Choice | Recommendation | Why |
|---|---|---|
| Discretisation | Eulerian staggered (MAC) grid | Formal convergence statements; deterministic; reuses the cloth code's multigrid and conjugate gradient |
| Interface | Level set, advected with a fifth-order WENO scheme and third-order Runge–Kutta, corrected by marker particles, with a global volume correction per step | A smooth surface for refraction and for meshing; the known volume loss is measured and corrected, and reported |
| Momentum advection | A limited high-order conservative scheme, chosen by measurement | Semi-Lagrangian advection is too dissipative to claim accuracy |
| Free surface | Ghost-fluid pressure condition at the sub-cell interface position | Second-order pressure boundary; carries surface tension when enabled |
| Solids | Variational cut-cell pressure solve (Batty, Bertails and Bridson 2007) with face-area weights | One symmetric system; sub-cell boundaries; fluid may separate from solids |
| Pressure solver | Multigrid-preconditioned conjugate gradient with fixed-order reductions | Bit-reproducible at any thread count |
| Time step | Equal substeps per output segment under the advective, gravity, capillary and viscous limits | Macro-step runs stay byte-equal to free runs, as cloth's do |
| Optional physics | Viscosity and surface tension, each off by default and validated separately | |

**Runner-up:** particle-in-cell transfer (FLIP/APIC) on the same grid and pressure solve. Better splashes and
free particles; it loses because interface position from particle density has no clean convergence statement and
volume drifts. It can be added later as a second mode with the same outputs.

**Not chosen:** smoothed-particle methods (weak accuracy at boundaries and free surfaces, hard to make bit
reproducible); volume-of-fluid with interface reconstruction (best conservation, but hard to combine with cut-cell
solids from scratch and gives no smooth surface); material-point and lattice methods.

## 2. Coupling: the port mechanism is not enough for light bodies

- A staggered exchange between an incompressible liquid and a solid is unstable when the solid is light relative
  to the liquid it displaces, and refining the time step does not cure it (the added-mass instability: Causin,
  Gerbeau and Nobile 2005; Förster, Wall and Ramm 2007). Checked: Causin et al. (Prop. 3) prove their explicit scheme
  unconditionally unstable, whatever the step, when ρ_s h_s < ρ_f μ_max (μ_max the largest eigenvalue of the
  added-mass operator; geometry-dependent), and Förster et al. that every sequential staggered scheme has a limiting
  density ratio. Neither paper gives a number for a sphere. The "near 0.5" is this design's reading of the same
  criterion for a rigid body (m_body < m_added = ρV/2 for a sphere in unbounded liquid); it is an analogy to their
  thin-tube model, measured in M7b §5, not a result of the papers. It rules out anything that floats. Cloth in water is the extreme case.
- The M6 group host's implicit sub-iteration would work for one rigid body but repeats a full liquid step per
  iteration, which is unaffordable at bake resolution.
- **Design:** the liquid's pressure solve includes the rigid bodies' six degrees of freedom (the variational
  formulation does this in one symmetric system). The liquid member of a group takes the body's mass, inertia and
  external load from `rigid.bodies` through a port, solves the coupled pressure, and returns the net hydrodynamic
  force and torque; the rigid solver still integrates the body and owns contact. This needs one additive contract
  feature: a port payload carrying mass properties, and a group policy in which the liquid's step is the single
  implicit exchange for those bodies.
- **Cloth in liquid:** first version treats cloth as a prescribed moving boundary with a lagged load, labelled
  `preview`. A stable two-way treatment (fully implicit, Robinson-Mosher et al. 2008) is a later step. *Done as M7e
  (2026-11-26, [results](SIMULATION_M7E_RESULTS.md); preview): the cloth offers its linearised response (lumped masses,
  pins, force, projected elastic Hessian, Rayleigh coefficients) through two ports; the liquid solves it with the
  pressure in one symmetric positive definite system in every step (S = M(1 + dt alpha) + (dt beta + dt²)K in place of
  a body's mass matrix, plus the liquid on the faces the sheet closes) and offers the impulse of each of its steps; the
  cloth takes those steps with the same linear model (an owned exchange). A scheme in which the cloth integrated on
  its own nonlinear model with the liquid's impulses diverged slowly for light cloth; this one is stable from 0.05 to
  100 kg/m². One-sided sheets that sweep cell centres under gravity still leak.* *M7f (2026-11-26,
  [results](SIMULATION_M7F_RESULTS.md); preview): the leak had two causes. In 2-D the test strip left the one-cell
  slab (fixed: a 2-D grid's sheets push only in its plane). In 3-D the level set crossed the sheet and the volume
  correction drained the wet side (fixed: the sheet's faces are walls or boundaries of the level set's stencils, and
  the level set is the signed distance to a sheet holding liquid on one side, set before the volume is measured). A
  pinned bag at 24–64 cells passes nothing through the sheet, and the membrane at large sag is within 0.8% of its
  reference. Layers thinner than about four cells on a sheet are not handled.* *M7g (2026-11-26,
  [results](SIMULATION_M7G_RESULTS.md); preview): the dry-corner values were a false interface spreading along the
  domain walls from the reinitialisation's linear wall ghosts (constant with coupled sheets now; a bag at 24–64 cells
  keeps no liquid-like values below the sheet); a light flap's fast motion came from the face liquid driving the
  sheet at 1/n_a times its tangential speed and from load across an air gap (both fixed); a through-sheet measure is
  reported. Thin layers on a sheet stay under-resolved below about 4.8 cells (sheet-specific, located, not fixed);
  rigid contact inside liquid stays preview (no body–body contact).*

## 3. Outputs

| Output | Codec | Notes |
|---|---|---|
| `surface` | `mesh.v2`, changing topology | Marching cubes on the level set; per-vertex velocity sampled from the grid for motion blur |
| `particles` | `points.v1` | Marker particles and spray; the codec is specified but not implemented |
| `fields` | `grid.v1` | Level set and velocity, for refraction-aware rendering options and for coupling |

A level set cannot hold features thinner than about one cell: thin sheets break and sub-cell droplets vanish from
the mesh. Particles carry them. This is stated in the solver's evidence scope.

## 4. Validation suite

| # | Case | Reference | Establishes |
|---|---|---|---|
| 1 | Zalesak disk, single-vortex deformation | Standard interface-transport tests with area loss and shape error | Interface advection |
| 2 | Hydrostatic rest | p = ρgz, zero velocity | Pressure boundary, spurious currents |
| 3 | Standing gravity wave | ω² = gk·tanh(kh); viscous decay of the *amplitude* 2νk² (energy 4νk²) (checked, M7_REFERENCES §2) | Dynamics, dissipation |
| 4 | Sloshing tank | Linear natural frequencies | Walls, free surface |
| 5 | Rayleigh–Taylor growth | Linear growth rate | Instability growth (replaced by the early-time column collapse of M7a §4, which has a closed form; no Rayleigh–Taylor case was built) |
| 6 | Volume conservation over a long run | Drift bound | Volume correction |
| 7 | Refinement study on 2–4 | Observed order | Convergence |
| 8 | Dam break | Martin and Moyce 1952 front position; Lobovský et al. 2014 ensemble (100 runs per depth) | End to end against experiment |
| 9 | Floating and neutral rigid body | Archimedes equilibrium depth; momentum audit | Rigid coupling |
| 10 | Falling sphere in viscous liquid | Stokes terminal velocity | Viscosity, coupling |
| 11 | Oscillating droplet, rising bubble | Lamb frequency; Hysing et al. 2009 case 1 | Surface tension |

Cases 1–7 gate `predictive` for single-phase flow with static or prescribed solids. Case 8 is reported against the
experimental spread. Cases 9–11 gate the label for rigid coupling, viscosity and surface tension respectively.
The dam-break tables must be transcribed from the papers, and the two papers normalise time differently. Martin and Moyce's
table was checked against the scan (all 19 values); Lobovský's front positions exist only as a figure, so their average
front speeds and impact-pressure statistics are the second reference (M7_REFERENCES §1).

**Stays `preview`:** spray and thin films, cloth coupling, rigid contact inside liquid, air entrainment, anything
below the resolved scale.

## 5. Cost (estimate, not measured)

About 1–3 s per step at 256³ on this machine, and roughly 1,200 steps per simulated second at 4 mm cells, so a 10 s
bake is on the order of 7 hours; 128³ is about 16 times cheaper. A first prototype replaces this estimate.

## 6. Prerequisites that do not depend on the method (done: [results](SIMULATION_M7_PREREQ_RESULTS.md))

| Item | Why |
|---|---|
| `points.v1` codec, cache support, and a renderable particle geometry | Spray and markers; also any imported particle data |
| Changing-topology and empty meshes in `simulated_mesh_geometry` | Per-frame rebuild instead of refit; bounded per-geometry mailbox table; light tables |
| Streaming or chunked checkpoints | A 256³ state is too large to hold twice in memory |
| Rigid mass properties over a port | The coupled pressure solve |

## 7. Build order after the prerequisites

1. Grid, level set, advection; case 1.
2. Free-surface pressure solve; cases 2–7.
3. Static cut-cell solids; outputs; dam break (case 8).
4. Rigid bodies in the pressure solve; case 9. *Done (M7b, 2026-11-26; [results](SIMULATION_M7B_RESULTS.md)): the
   coupled solve, the owned exchange with `rigid.bodies`, case 9 with added-mass and bobbing references. The
   density-ratio limit of §2's staggered exchange was not measured as a threshold; a ratio-0.5 floating cylinder
   diverges with it and is stable in the coupled solve. Face fractions beside bodies are control-volume
   fractions (area fractions gave a staircase buoyancy).*
5. Viscosity and surface tension; cases 10–11. *Done (M7c, 2026-11-26; [results](SIMULATION_M7C_RESULTS.md)):
   implicit viscosity in the variational strain-rate form (Batty and Bridson 2008; derived here, then compared with the paper 2026-11-26: same energy and weights, M7_REFERENCES §7) with an incremental
   pressure and bodies coupled in the viscous solve, and the ghost-fluid Laplace jump; case 10 against Stokes with
   the Haberman–Sayre wall factor, case 11 against Lamb's frequency and the Laplace pressure. Hysing's rising bubble is
   two-phase and was replaced (a capillary-gravity wave). Viscous splitting needs ν dt / h² ≤ 4 (a time-step limit);
   inviscid surface tension stays preview (parasitic currents grow).*
6. Checkpoint, rollback and thread-invariance tests; scene chunk `liquid_simulation`.
7. Cloth as a moving boundary (`preview`). *Done (M7d, 2026-11-26; [results](SIMULATION_M7D_RESULTS.md)): sheets close
   the grid faces whose cell-centre segment they cross (no volume removed), carry their normal velocity there and
   receive the pressure impulse on those faces as a per-vertex load; prescribed or following a cloth's `surface_state`,
   the load returned as its `surface_load` once per macro step. The lagged exchange is stable only for sheets far
   heavier than cloth (measured in §6 of the results); a stable two-way treatment is still the later step. Also in
   M7d: closed-mesh bodies, and the dam break at 256³.* *M7e (2026-11-26; [results](SIMULATION_M7E_RESULTS.md)): the
   implicit coupling of §2, stable for real cloth.* *M7f (2026-11-26; [results](SIMULATION_M7F_RESULTS.md)): sheets
   that hold liquid on one side no longer leak; a cloth catches and holds a block of water in the demo.* *M7g (2026-11-26; [results](SIMULATION_M7G_RESULTS.md)): second round of sheet defects (§2).*
