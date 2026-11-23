# M3 design: cloth, the second solver

2026-11-23. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §5 M3. Starts after M2. Two surveys
feed this note: a literature survey of cloth models (mostly from abstracts and recollection; numbers below marked
*to verify* must be checked against the sources before they become pass criteria) and a code survey of how the
renderer handles meshes, motion blur and the viewport.

## 1. Model

| Choice | Decision | Why |
|---|---|---|
| Membrane | Orthotropic hyperelastic constant-strain triangles: warp and weft stretch, in-plane shear, Poisson coupling, in material coordinates | Parameterised by measurable continuum quantities; mesh-independent in stretch |
| Bending | Discrete-shell hinge energy with bending rigidity per material direction | Cheap; maps directly to measured flexural rigidity |
| Mass | Lumped, from areal density | |
| Damping | Rayleigh, calibrated and labelled as such; quadratic air drag as an external load | Measured internal loss data is rare; do not pretend otherwise |
| Interactive mode | Backward Euler, 1–3 Newton iterations, penalty contact against analytic shapes and meshes, no self-collision | Viewport rates on a few thousand vertices |
| Bake mode | BDF2, Newton to tolerance, barrier contact with continuous collision detection and thickness offset, smoothed Coulomb friction | Second order, intersection-free, convergent contact |
| Linear algebra | In-house 3×3 block sparse matrix and preconditioned conjugate gradient | No dependencies; portable |

Both modes integrate the same energy, so a bake refines what the preview showed.

**Known limit:** hinge bending does not converge on irregular meshes and constant-strain triangles lock in
bending-dominated states. The `predictive` label is therefore restricted to regular (structured) cloth meshes, which
the solver generates itself from a rectangle or a simple pattern. Imported irregular meshes run as `preview`.
The upgrade path is a subdivision-surface Kirchhoff–Love shell element with the same material parameters.

**Not captured:** hysteresis, yarn-level effects, wrinkles below mesh resolution.

## 2. Material data

- Ship parameters transcribed from Clyde, Teran and Tamstorf 2017 (five woven fabrics) after reading the paper's
  tables, plus a small set built from Kawabata-style literature values. *To verify:* the tables themselves.
- The Wang, O'Brien and Ramamoorthi 2011 dataset is used only if its licence allows redistribution in a renderer;
  the site shows a non-commercial licence and whether that covers the data is unconfirmed.
- Each material record carries its source and units, like the fire fuel records.

## 3. Validation suite (evidence for the label)

| # | Case | Reference | Establishes |
|---|---|---|---|
| 1 | Hanging strip between two supports | Catenary | Membrane stretch, gravity load |
| 2 | Cantilever strip under gravity | Elastica by shooting; bending length per the standard cantilever test (*to verify:* exact convention and units) | Bending rigidity |
| 3 | Rectangular membrane under tension | Mode frequencies f_mn = ½·√(T/σ)·√((m/a)²+(n/b)²) | Dynamics, mass |
| 4 | Clamped and simply supported square plate | Plate fundamental frequency (*to verify:* constants) | Bending dynamics |
| 5 | Undamped free vibration | Energy drift per period, by integrator | Time integration |
| 6 | Refinement study on 2 and 3 | Observed order | Convergence |
| 7 | Picture-frame shear | The fitted shear curve | Shear |
| 8 | Drape over a disc | Published drape coefficient for the same fabric, with stated uncertainty | End-to-end, contact |

Cases 1–6 are analytic and gate the `predictive` label. Cases 7–8 are comparisons with measurements and are reported.

## 4. Solver outputs and ports

- Output `surface`, codec `mesh.v1`: positions, normals, velocities, fixed topology, material coordinates as UVs.
- Inputs: collider geometry and its motion sampled through the scene inputs interface; pinned vertices that follow
  a scene object's transform.
- Ports: accepts `surface_load` (pressure and traction per vertex); offers `surface_state` (position and velocity).
  Honours an imposed macro step.
- Backends: portable CPU, `supported`.

## 5. Rendering deforming geometry

What exists: `ITriangleMeshGeometryIndexed::UpdateVertices` overwrites positions and normals in place and refits the
BVH (full rebuild when quality degrades); the per-frame prepare-and-freeze seam swaps fire media between frames;
motion blur is transform-only, with per-sample scene time held per thread and no time on the ray.

| Piece | Design |
|---|---|
| Mesh provider | Generalise the prepare seam from "time-varying media" to "time-varying assets": a provider installs the frame's mesh into its geometry between frames, under the render freeze. Triggers the top-level structure and light-table refresh that today only keyframed objects trigger |
| Geometry type | A new `simulated_mesh_geometry` bound to a simulation output, built on the indexed mesh. Static meshes and their hot path are untouched |
| Update cost | A lean update path: positions, normals, refit. The watertightness weld, area recomputation and logging in `UpdateVertices` are measured first and skipped or deferred where they are not needed per frame |
| Deformation blur | Two position keys per frame (shutter open and close, from positions and velocities). BVH boxes bound both keys. Intersection and shading interpolate vertices at the sample's time, read from the same per-thread sample time the transform blur uses. A `MotionBlurTimeAverageTest` row with a deforming mesh is the check |
| Emissive cloth | Light tables refreshed per frame; reported as unsupported until tested |

## 6. Live viewport

- A new entry on the scene edit controller for an external frame provider: park the render, install the frame,
  invalidate, kick. It reuses the cancel–wait–mutate–kick protocol time scrubbing already follows.
- The in-process host steps the cloth on its own thread and offers the latest frame; the viewport takes it at the
  next park. Playback follows the timeline clock; scrubbing backwards reads recorded frames.

## 7. Order and acceptance

| Step | Work | Check |
|---|---|---|
| M3a | Solver, interactive and bake modes, material records, validation cases 1–6 | Cases pass with stated error; contract conformance through both hosts |
| M3b | Mesh provider, `simulated_mesh_geometry`, lean update, deformation blur | Baked cloth renders; blur row passes; static-mesh renders unchanged |
| M3c | Live viewport playback | A few-thousand-vertex cloth plays in the viewport at interactive rates; measured numbers recorded |
| M3d | Cases 7–8, contract review and freeze | Results note; contract frozen |

## 8. Risks

- Deformation blur is a new subsystem in the intersection path; it must cost nothing for static meshes.
- Refit quality under large deformation, and the per-frame cost of the existing update path.
- Barrier contact with continuous collision detection is the largest single piece of new numerical code.
- Liquids (M7) will need per-frame topology change, which this design does not cover: geometry rebuild per frame,
  top-level rebuild, and a per-geometry mailbox table that currently grows with every new geometry.
