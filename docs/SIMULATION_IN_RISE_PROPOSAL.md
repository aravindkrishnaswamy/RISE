# Simulation in RISE: a general architecture (proposal)

2026-11-22. **Proposal for owner review; nothing here is built.** It supersedes the framing of
[FIRE_SMOKE_UI_INTEGRATION_PROPOSAL.md](FIRE_SMOKE_UI_INTEGRATION_PROPOSAL.md), which treats fire alone; that note's
survey of the existing code still stands and its phases become "fire as the first solver" here.

## The idea in one paragraph

A simulation is a **first-class scene element that produces time-indexed data**. It reads other scene elements
(geometry, materials, motion, other simulations), advances a state through time, and publishes typed outputs
(volumes, particles, deforming meshes, transforms) into a **cache**. Everything else in RISE consumes those outputs
through ordinary bindings: a medium binds a volume, a geometry binds a mesh stream, an instance binds a transform
stream. Rendering never talks to a solver; it talks to the cache. Fire is one solver behind this contract; smoke
from an imported VDB sequence, cloth, rigid bodies, liquids or weathering are others.

## Why not just generalise the fire plan later

The fire-only plan would hard-wire five things that every later simulation needs again: a scene chunk, a cache with
staleness, a background runner with progress and cancel, playback of partial results on the timeline, and a way to
feed scene geometry and motion into a solver. Building those once, with fire as the first client and a second,
deliberately different solver as the proof, costs little more than building them for fire alone.

## Five layers

### 1. Solver contract (library)

One interface every solver implements. Sketch:

| Part | What it says |
|---|---|
| Describe | Parameters, as a normal chunk descriptor, so the editor, properties panel and agent schema come for free |
| Inputs | Which scene elements it reads (colliders, emitters, materials, fields from other simulations) and at what times |
| Outputs | Typed, named channels with units: dense or sparse grids, point sets, mesh sequences, rigid transforms |
| Advance | Step from one output time to the next; internal substeps are the solver's business |
| Checkpoint / restore | Opaque state blobs, so any run can stop and resume |
| Capabilities | Backends (CPU, Metal), deterministic or not, memory and time estimate, fidelity modes (fast, certified) |
| Evidence | What validation the solver carries, so outputs can be labelled preview or predictive as fire does today |

The solver knows nothing about files, UI, threads or the timeline.

### 2. Host (who runs a solver)

The same solver object can be run by two hosts:

- **In-process host** for cheap solvers and interactive preview: a cloth patch or a few hundred rigid bodies can
  step live in the viewport as the user scrubs.
- **Worker-process host** for heavy bakes: a separate executable (`rise-sim`) that loads the scene, runs one or
  more solvers, writes the cache and exits. It survives the app closing, resumes from checkpoints, and owns the GPU
  without contending inside the app. Fire runs here.

This resolves "library or separate tool": the solver lives in the library, and the host decides where it runs.

### 3. Cache (the only thing rendering sees)

A generalisation of today's fire sequence manifest:

- **Manifest:** time map, list of frames, channels with types and units, content hashes, producer identity, fidelity
  label, embedded physical records. Appended to as frames arrive.
- **Frame containers:** a small native format for grids, points and meshes, so playback needs no extra
  dependencies on any platform. OpenVDB and Alembic become import and export formats.
- **Key:** hash of solver id and version, parameters, and the hashes of every input it read. States: missing,
  baking, partial, complete, stale.
- **Imported data is the same thing.** A VDB sequence from Houdini or Blender is a cache with a foreign producer and
  no solver attached. The Blender bridge's temporary slice cache would move onto this.

### 4. Scene binding and time

- **Producer chunks:** `simulation`-category chunks, one keyword per solver (`fire_simulation`, later
  `cloth_simulation`, …), plus `simulation_cache` for imported sequences.
- **Consumer bindings:** existing chunks gain a way to name a simulation output instead of a file:
  a medium takes `source campfire.density`, a mesh geometry takes `source cloth.surface`, an object takes
  `transform_source debris.body[3]`.
- **Time:** every simulation declares its span and output rate; consumers may retime (offset, scale, hold, loop).
  The renderer already has the right seam: per-frame preparation (`PrepareMediaForRender`, render freeze) swaps
  time-varying media between frames. That generalises to "time-varying asset providers" for geometry and
  transforms too. Sub-frame motion (velocity channels, the fire advection offset) feeds motion blur.
- **Dependencies:** inputs form a graph. A fire that reads a moving collider depends on the animation; smoke that
  reads a rigid-body simulation depends on that bake. The bake scheduler orders them and staleness propagates.
- **Coupling:** one-way coupling (scene → simulation, simulation → simulation) comes from the graph. True two-way
  coupling is a group of solvers stepped together inside one host; the contract allows it, but it is not in the
  first build.

### 5. Control surfaces

One implementation, three faces:

- **UI:** a Simulations panel (list, state, progress, bake / stop / resume, queue), baked ranges drawn on the
  timeline, and the timeline shown for any scene with time-varying data.
- **Command line:** `rise-sim bake|resume|status <scene> [simulation]`, and follow-mode rendering as a flag.
- **Agent:** `sim_bake`, `sim_status`, `sim_wait`, `sim_cancel`, mirroring the render verbs.

Resource rules live in the scheduler: one GPU solver at a time, a memory budget, and whether a render may run
alongside a bake.

## What fire becomes

- The stepping loop now inside `tests/FireSequenceTest.cpp` is extracted behind the solver contract. Its options
  (soot yield, radiation closure, fast mode, step cap) become parameters instead of environment variables.
- The fire manifest becomes the general manifest with fire's records embedded; certified mode keeps its seals.
- `fire_medium` binds `source <simulation>.<channels>`; `sequence_manifest <path>` keeps working.
- Follow-mode rendering, the incremental manifest and per-frame verification (all built in r275) are the first
  pieces of layers 3 and 5.

## Order of work

| Step | Work | Proves |
|---|---|---|
| 1 | Cache: general manifest, native frame container, partial loading, staleness key. Fire sequences and imported VDB both read through it | Layer 3; the macOS app can play a fire |
| 2 | Solver contract and worker host; fire extracted onto them, output byte-identical to today | Layers 1–2 for a heavy GPU solver |
| 3 | A second, small CPU solver on the same contract, run in-process (candidate: rigid bodies or cloth, producing transforms or a mesh stream) | The contract is not fire-shaped; geometry outputs; live preview |
| 4 | Scene chunks, bindings, dependency graph, retiming | Layer 4 |
| 5 | UI panel and timeline, agent verbs, scheduler rules | Layer 5 |

The interface is frozen only after step 3. Two unlike solvers are the minimum to trust it.

## Decisions for the owner

1. **Which simulations come next.** This shapes the output types and the second solver. Volumes only (smoke,
   clouds, explosions) is a much narrower contract than volumes plus geometry (cloth, rigid bodies, liquids).
2. **How far two-way coupling matters** (fire heating and moving objects, liquid pushing bodies). It decides
   whether co-stepping is designed in from the start or left as an allowed extension.
3. **Native frame format versus shipping OpenVDB and Alembic** with every app.
4. **Interactive ambition:** bake-only, or live in-viewport simulation for cheap solvers.
5. **Certification as a general notion:** should every solver carry a preview / predictive label and evidence, as
   fire does, or is that fire-specific rigour?
6. **Platforms:** playback everywhere and simulation where a backend exists, or a requirement that every solver
   has a portable CPU path.

## Not known yet

- Whether the interactive viewport rasterizer renders time-varying media and honours the prepare-and-freeze path.
- How deforming geometry would enter the acceleration structures per frame, and what that costs.
- What a render running beside a GPU bake costs each of them (the r275 follow run gives a first number).
