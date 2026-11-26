# M7 design: liquid, the third solver

2026-11-25. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §5 M7. Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md). Based on a literature survey; items marked
*to verify* come from recollection and must be checked against the sources before they become pass criteria.
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
  Gerbeau and Nobile 2005; Förster, Wall and Ramm 2007). For a sphere the limit is a density ratio near 0.5
  (*to verify*), which rules out anything that floats. Cloth in water is the extreme case.
- The M6 group host's implicit sub-iteration would work for one rigid body but repeats a full liquid step per
  iteration, which is unaffordable at bake resolution.
- **Design:** the liquid's pressure solve includes the rigid bodies' six degrees of freedom (the variational
  formulation does this in one symmetric system). The liquid member of a group takes the body's mass, inertia and
  external load from `rigid.bodies` through a port, solves the coupled pressure, and returns the net hydrodynamic
  force and torque; the rigid solver still integrates the body and owns contact. This needs one additive contract
  feature: a port payload carrying mass properties, and a group policy in which the liquid's step is the single
  implicit exchange for those bodies.
- **Cloth in liquid:** first version treats cloth as a prescribed moving boundary with a lagged load, labelled
  `preview`. A stable two-way treatment (fully implicit, Robinson-Mosher et al. 2008) is a later step.

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
| 3 | Standing gravity wave | ω² = gk·tanh(kh); viscous decay 2νk² (*to verify*) | Dynamics, dissipation |
| 4 | Sloshing tank | Linear natural frequencies | Walls, free surface |
| 5 | Rayleigh–Taylor growth | Linear growth rate | Instability growth |
| 6 | Volume conservation over a long run | Drift bound | Volume correction |
| 7 | Refinement study on 2–4 | Observed order | Convergence |
| 8 | Dam break | Martin and Moyce 1952 front position; Lobovský et al. 2014 ensemble (100 runs per depth) | End to end against experiment |
| 9 | Floating and neutral rigid body | Archimedes equilibrium depth; momentum audit | Rigid coupling |
| 10 | Falling sphere in viscous liquid | Stokes terminal velocity | Viscosity, coupling |
| 11 | Oscillating droplet, rising bubble | Lamb frequency; Hysing et al. 2009 case 1 | Surface tension |

Cases 1–7 gate `predictive` for single-phase flow with static or prescribed solids. Case 8 is reported against the
experimental spread. Cases 9–11 gate the label for rigid coupling, viscosity and surface tension respectively.
The dam-break tables must be transcribed from the papers, and the two papers normalise time differently.

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
5. Viscosity and surface tension; cases 10–11.
6. Checkpoint, rollback and thread-invariance tests; scene chunk `liquid_simulation`.
7. Cloth as a moving boundary (`preview`).
