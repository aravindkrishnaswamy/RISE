# Simulation in RISE: architecture and execution plan

2026-11-22. **Approved direction; nothing here is built yet** except the r275 pieces named under "What exists".
Owner decisions are recorded in §2. The earlier fire-only note,
[FIRE_SMOKE_UI_INTEGRATION_PROPOSAL.md](FIRE_SMOKE_UI_INTEGRATION_PROPOSAL.md), remains as the survey of existing
code; its phases are absorbed here as "fire, the first solver".

## 1. The idea

A simulation is a **first-class scene element that produces time-indexed data**. It reads other scene elements
(geometry, materials, motion, other simulations), advances a state through time, and publishes typed outputs
(volumes, particles, deforming meshes, transforms) into a **cache**. Everything else in RISE consumes those outputs
through ordinary bindings. Rendering never talks to a solver; it talks to the cache or, for live preview, to the
solver's latest published frame through the same reader interface.

## 2. Owner decisions (2026-11-22)

| # | Decision | Consequence |
|---|---|---|
| 1 | Volumes **and geometry**; cloth and liquids matter most | Output types include fixed-topology deforming meshes (cloth), changing-topology meshes and particles (liquids), and transforms. Cloth is the second solver; a liquid solver is the third |
| 2 | Two-way coupling must stay possible | Coupling ports and a co-stepping group are part of the contract from the first version, even though the first solvers use them one-way |
| 3 | Live in-viewport simulation for cheap solvers is wanted | An in-process host is required, not optional; solvers declare whether they can run interactively |
| 4 | Every solver carries a preview / predictive label with evidence | Fidelity label, validation cases and fail-closed loading are part of the contract and the manifest for all solvers |
| 5 | Every solver has a portable CPU path; it may be marked unsupported while in development | Backends carry a status (`supported`, `experimental`, `unsupported`). Fire's CPU path starts as `unsupported` |

## 3. What exists (r275)

- Fire solver: Metal, driven from `tests/FireSequenceTest.cpp` (about 5–6k lines of driver in the test file), case
  hard-coded, options in about 97 `RISE_FIRE_*` environment variables. Certified path and opt-in fast mode.
- `fire_medium { sequence_manifest … }` playback in the command-line renderer; sealed, content-hashed manifest.
- Incremental partial manifest, per-frame preflight option, follow-mode rendering script
  (`tools/fire/r275/follow_render.py`).
- Per-frame render preparation seam (`PrepareMediaForRender`, render freeze) that swaps media between frames.
- No library simulation API, no background-task system beyond the single render slot, no on-disk cache with
  staleness, no fire support in the GUI apps (OpenVDB is only in the make build).

## 4. Layers

### 4.1 Solver contract (library)

| Part | What it says |
|---|---|
| Describe | Parameters as a normal chunk descriptor (editor, properties panel and agent schema follow) |
| Inputs | Scene elements read (colliders, emitters, materials, other simulations' outputs), sampled through the animator at solver times |
| Outputs | Typed, named channels with units: dense and sparse grids, point sets, meshes (fixed or changing topology, with velocities), rigid transforms |
| Advance | From one output time to the next; substeps are internal. Must also support advancing by an externally imposed macro step for co-stepping |
| Ports | Coupling exchange at a macro-step boundary: what the solver offers (for example a velocity field, a collision surface with velocity, heat flux) and accepts (forces, impulses, boundary motion, sources). A port can be fed by a live solver or by another simulation's cached output, so one-way and two-way coupling use the same interface |
| Checkpoint / restore | Opaque state blobs; every run can stop and resume |
| Capabilities | Backends with status, interactive or bake-only, deterministic or not, cost and memory estimate, fidelity modes |
| Evidence | Validation cases and what they establish; yields the `preview` or `predictive` label written into the cache |

The solver knows nothing about files, UI, threads or the timeline.

### 4.2 Hosts

- **In-process host:** steps interactive solvers in the app as the timeline plays or scrubs; publishes frames to
  the viewport; can record to a cache.
- **Worker host:** the `rise-sim` executable. Loads the scene, runs one solver or a co-stepping group, writes the
  cache, reports status, resumes from checkpoints, survives the app closing. Heavy and GPU solvers run here.
- **Co-stepping group:** solvers that are two-way coupled run in one host with a shared macro step and a port
  exchange per step. Exchange order and sub-iteration are the group's policy, not the solvers'.

### 4.3 Cache

- **Manifest (general):** time map, frames, channels with types and units, content hashes, producer identity,
  fidelity label and evidence, embedded physical records. Appended as frames arrive. Today's sealed fire manifests
  stay loadable.
- **Native frame container:** grids, point sets, meshes, transforms. No external dependency at playback. OpenVDB
  and Alembic are import and export formats only.
- **Key and states:** hash of solver id and version, parameters and input hashes; `missing`, `baking`, `partial`,
  `complete`, `stale`. A stale cache still plays, marked stale.
- **Imported data:** an external VDB or mesh sequence is a cache with a foreign producer, labelled `preview`.

### 4.4 Scene binding and time

- **Producers:** one keyword per solver in a new `Simulation` chunk category (`fire_simulation`,
  `cloth_simulation`, `liquid_simulation`, `simulation_cache` for imports) and `simulation_group` for coupled sets.
- **Consumers:** existing chunks name an output instead of a file: a medium `source campfire.density`, a mesh
  geometry `source flag.surface`, an object `transform_source debris.body[3]`.
- **Time:** each simulation declares span and output rate; consumers may offset, scale, hold or loop. Per-frame
  preparation generalises from media to geometry and transforms; velocity channels feed motion blur.
- **Dependencies:** inputs form a graph; the scheduler orders bakes and staleness propagates along it.
- **Fidelity:** a consumer may request `predictive` and fails closed if the cache is not, as `fire_medium` does.

### 4.5 Control surfaces

- **UI:** Simulations panel (state, progress, bake / stop / resume, queue), baked ranges on the timeline, live
  play for interactive solvers.
- **Command line:** `rise-sim bake|resume|status <scene> [simulation]`; follow-mode rendering.
- **Agent:** `sim_bake`, `sim_status`, `sim_wait`, `sim_cancel`.
- **Scheduler rules:** one GPU solver at a time, a memory budget, renders allowed beside a bake.

## 5. Milestones

Each milestone ends with tests green, a warning-free build, and a short results note in `docs/`. The solver
contract is **frozen only after M3**; until then it may change.

| # | Milestone | Acceptance |
|---|---|---|
| M0 | Baseline: reference fire outputs (certified, a short tier-6 and tier-8 window) and reference renders stored outside git; inventory of the driver code to extract | Reference digests recorded; inventory note |
| M1 | **Cache.** General manifest and native frame container (grids first); partial loading and per-frame verification as the default for `preview`; staleness key; VDB import to cache; fire sequences read through the cache; fire playback in the macOS GUI build | Existing fire scenes: voxel data bit-equal through the new reader, renders equal within noise; sealed fire manifests still load; a half-baked cache loads; GUI loads and renders a fire frame |
| M2 | **Contract and hosts.** Solver interface, in-process host, `rise-sim` worker; fire driver extracted from the test file onto them; environment options become parameters (environment still honoured during transition) | Certified fire output byte-identical to M0 references; fast mode within its r275 tolerances; stop and resume works; the test file shrinks to tests |
| M3 | **Cloth, the second solver.** CPU, in-process, interactive. Outputs a fixed-topology mesh stream with velocities; collides with scene geometry; validation cases with stated error | Analytic and convergence cases pass and are documented; live in the viewport at interactive rates on a modest mesh; baked cloth renders with correct motion blur; contract reviewed and frozen |
| M4 | **Scene.** Chunk category, producer chunks, consumer bindings, dependency graph, retiming, per-frame geometry and transform providers | Scenes round-trip through the editor; stale detection and propagation tests; the timeline shows for any time-varying scene |
| M5 | **Control surfaces.** macOS panel and timeline ranges, command line, agent verbs, scheduler; Windows panel for monitoring and playback | End-to-end bake of fire and cloth from the app and from the agent |
| M6 | **Coupling.** Ports implemented on fire and cloth; co-stepping group host; one two-way demonstration (cloth with a small rigid-body solver, or cloth driven by and loading a flow) | A conserved quantity (momentum) balances across the port to stated tolerance; one-way from cache and two-way live give consistent results in the weak-coupling limit |
| M7 | **Liquid, the third solver.** Portable CPU first; particles, changing-topology surface mesh and optional volume outputs; two-way with rigid bodies or cloth through the ports; validation cases | Dam-break front position against published data, hydrostatic rest, volume conservation; renders as a refractive surface |
| M8 | **Portable CPU paths to supported.** Fire's CPU path from `unsupported` to at least `experimental`; simulation on Windows where a backend exists | Cross-platform playback of every cache type; CPU fire agrees with Metal fast mode within tolerance on a coarse case |

### Execution status (2026-11-22)

M0 historical references verified at both tiers; the forced make rebuild passed; the fresh fast tier built 419 executables and passed 416 on a host with no visible Metal device. The full SwiftUI executable now renders legacy and native scenes through its viewport probe. The warning-free macOS 26.2 release build remains open. The [build and capture status](SIMULATION_M0_RESULTS.md) and
[M2 driver inventory](SIMULATION_M0_DRIVER_INVENTORY.md) are recorded.
The [M1 cache design](SIMULATION_M1_CACHE_DESIGN.md) completed independent adversarial review with no open P1/P2 findings.
On 2026-11-23 the owner authorized deferring the Metal fast-tier and macOS GUI build checks to M1 acceptance. The [M1 verification record](SIMULATION_M1_RESULTS.md) records voxel equality on six historical frames, sealed and partial playback, render noise comparisons and remaining integration gates. M1 is open.

2026-11-23: **M1 accepted.** With a Metal device present the make build is warning-free, all 419 fast tests pass, and
the certified fire path is byte-identical to the r274 reference (five checkpoints over 25 steps). Carried forward as
a release item, not an M1 gate: a warning-free macOS 26.2 release build of the app needs an OpenVDB build that
targets 26.2 for the import helper. M2 design: [SIMULATION_M2_DESIGN.md](SIMULATION_M2_DESIGN.md); execution moved
from Codex to Claude.

2026-11-23: **M4 implemented** (in review): producer chunks (`cloth_simulation`, `fire_simulation`,
`simulation_cache`) in `ChunkCategory::Simulation`, `source <simulation>.<output>` consumers, scene inputs, the
dependency graph with keys and states, `rise-sim bake|resume|status <scene>`, live runs from the scene without a
pre-bake. [M4 results](SIMULATION_M4_RESULTS.md).

2026-11-23: **M5a implemented** (library half of M5, in review): the process-wide bake manager (rise-sim
processes on the scene file, dependency queue, machine-wide GPU lock, CPU cap, memory budget, monitoring,
adoption, stop/resume), its C API for the GUI panels, and the agent verbs `sim_list`, `sim_bake`, `sim_status`,
`sim_wait`, `sim_cancel` and live control. [M5a results](SIMULATION_M5A_RESULTS.md).

2026-11-23: **M2 accepted.** A certified tier-8 fire baked from zero through `rise-sim` (6,457 steps, 5.95 h on a
loaded machine) reproduced the three M0 reference frames at 10.466, 10.529 and 10.591 s byte for byte; the reference
run predates r274. All 12 fire test programs pass on the tree at `524d42eba` plus in-progress M5/M6 edits.
**M3 (cloth, deforming-geometry rendering, live viewport, contract v1), M4 (scene chunks, bindings, staleness) and
M5 (bake manager, agent verbs, macOS UI) are built**; open within them: measured-fabric comparisons (M3 cases 7–8),
cloth self-collision evidence, simulation fields in the properties panel, the worker in the app bundle, the Windows
mirror. M6 (coupling) in progress.

2026-11-25: **M6 implemented** (in review): the co-stepping group host (staggered Jacobi / Gauss–Seidel, implicit
sub-iteration with Aitken relaxation and step halving, group checkpoints), five port adapters, the `transform.v1`
codec, the rigid-body solver `rigid.bodies`, two-way cloth ↔ rigid coupling with momentum balanced across the port
(implicit: 5e-8 relative), one-way fire → cloth from a fire cache, `simulation_group` / `port_source` /
`rigid_simulation` / `transform_source` scene syntax, group bakes as one job. [M6 results](SIMULATION_M6_RESULTS.md).

2026-11-26: **M7 in progress.** M7a (in review): `liquid.mac`, a free-surface liquid on a staggered grid with
cut-cell static solids, predictive from computed evidence on canonical problems ([M7a results](SIMULATION_M7A_RESULTS.md)).
M7b (in review): rigid bodies in the liquid's pressure solve (each body's six degrees of freedom solved with the
pressure in one symmetric positive-definite system), dynamic, prescribed or following `rigid.bodies` through an
owned implicit group exchange; validated by added mass in concentric containers, Archimedes at density ratios 0.1,
0.5 and 0.9 (second order), a neutral body at rest, and a bobbing frequency against an independent potential-flow
solve; stable at light density ratios where the staggered exchange diverges; contact inside the liquid, thin gaps
and small bodies stay preview ([M7b results](SIMULATION_M7B_RESULTS.md)). M7c (in review): constant viscosity (implicit,
the variational strain-rate form with a traction-free surface, no or free slip, bodies coupled implicitly in the
viscous solve) and surface tension (ghost-fluid Laplace jump), each off by default and certified separately; inviscid
surface tension and contact lines at solids stay preview ([M7c results](SIMULATION_M7C_RESULTS.md)). M7d (in review):
thin sheets as moving boundaries of the liquid (preview: grid faces crossed by the sheet closed, no volume removed, no
leakage found), prescribed or following a cloth's `surface_state` with the pressure load returned as its
`surface_load`, lagged one macro step; that exchange is stable only for sheets of about 500 kg/m² and up (about 50
with under-relaxation), so real cloth is one-way only; closed-mesh rigid bodies; the dam break at 256³ costs 2.3 s per
step and 6.3 GB ([M7d results](SIMULATION_M7D_RESULTS.md)). **M7 is complete as planned** (design §7, steps 1–7).
M7e (in review): cloth coupled implicitly with the liquid (preview): the cloth's linearised response (masses, force,
projected Hessian, damping) is solved with the pressure in every step, the liquid on the faces the sheet closes moves
with it, and the cloth takes the liquid's own steps with the same linear model; stable for areal densities 0.05–100
kg/m² at 24–64 cells and macro steps 1/30 and 1/120 s where the staggered exchange diverges, cotton in a dam-break surge
in the demo; checked against a piston (exact), a plate's added mass, a membrane's static deflection and natural
frequency under a layer; a light sheet holding liquid on one side that sweeps across cell centres under gravity still
leaks ([M7e results](SIMULATION_M7E_RESULTS.md)). M7f (in review): that leak fixed (preview). In 2-D the test strip
had left the one-cell slab; in 3-D the level set read across the sheet and the volume correction drained the wet side.
The sheet's faces are now walls or boundaries of the level set's stencils, and the level set is the signed distance to
a sheet holding liquid on one side. A pinned bag passes no liquid through the sheet at 24–64 cells over 3 s, the
membrane at large sag is within 0.8% of its reference, and the stability sweep still passes. A cloth catches a block of
water in the demo. An implicit sheet's load without its steps is refused, and the rotation stretch of the cloth's
linear step is reported. Viscosity with sheets stays refused ([M7f results](SIMULATION_M7F_RESULTS.md)). Not done
within M7: spray and thin films, contact inside the liquid, liquid layers on a sheet thinner than about four cells, and
the multigrid weakness with large bodies at 256³ (PCG iterations triple).

2026-11-26: **M8 partial** (in review; [M8 results](SIMULATION_M8_RESULTS.md)). Cross-platform playback: every codec and the cache container were already byte-defined little-endian; a committed golden cache (all six frame codecs, a static topology record, changing and empty topology) is decoded value by value and re-encoded to identical bytes by a fast-tier test. The library, `rise`, `rise-sim` and all tests build warning-free with Metal compiled out and the non-fire fast tier passes there. Reading the VS2022 and Android builds found seven certain compile errors (four broke the fire stack on every non-Apple build; `liquid.mac` did not compile with MSVC), all fixed; neither platform was compiled, and neither has a `rise-sim` target. Fire on the CPU: a complete one-step CPU projected-Heun owner exists as a calibration reference, but it is single-threaded, capped at 2 GiB, not callable by the run driver, and lacks r264's centred momentum carrier; on a 64-cell fixture it differs from the Metal owner by 1–3e-6 of field maximum, 4–10× more than rounding explains, cause not isolated. **The fire CPU backend stays `unsupported`; the M8 exit criterion for fire is not met.** 2026-11-26, later: **M8 part A done** ([M8 results §10](SIMULATION_M8_RESULTS.md)): the gap was r264's centred momentum carrier, absent on the CPU side (with it the one-step burner fixture is bit-identical in 7 of 9 rows, velocity within 2 ulps). The CPU owner is now a run-driver backend (`backend: "cpu"`), runs with Metal compiled out, is threaded and bit-identical at any worker count, resumes from checkpoints bit for bit, and agrees with Metal fast mode on the tier-4 coarse case to rounding at step 1 (with matched V-cycle counts) and within 2e-4 (species), 3e-4 (temperature), 1e-2 (velocity) of the field scale over 24 steps, where a localised burner-surface difference from step 2 is not yet explained. **Fire `cpu` is `experimental`; the M8 exit criterion for fire is met.** It is about 15× slower than Metal at tier 8. Windows/Android remain uncompiled.

## 6. Rules that hold throughout

- The certified fire path stays byte-identical unless a change is explicitly a physics change with its own note.
- Every solver ships with validation cases and an honest fidelity label; none is `predictive` by assertion.
- No solver-specific code in the cache, hosts, scheduler or UI; anything fire-shaped found there is a defect.
- New scene keywords follow the existing chunk checklist (`AGENTS.md`, `src/Library/Parsers/README.md`), including
  the five build lists and the append-only `IJob` and `ChunkCategory` rules.
- Bulk outputs (caches, references, movies) stay outside git.

## 7. Open questions to settle inside the milestones

- Whether the interactive viewport rasterizer renders time-varying media and honours prepare-and-freeze (M1).
- How deforming and changing-topology geometry enters the acceleration structures per frame, and the cost (M3, M7).
- Cloth model choice: it must be defensible physically (a continuum shell or membrane model with measured material
  parameters), not only stable (M3).
- Liquid method and surface reconstruction (M7).
- What a render beside a GPU bake costs each (first number from the r275 follow run).
