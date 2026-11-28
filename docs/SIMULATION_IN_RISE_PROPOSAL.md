# Simulation in RISE: architecture and execution plan

**Status (2026-11-27):** **M0–M8 substantially shipped** on `master` (solver contract v1 frozen, `fire.methane` version 4 [Metal + experimental CPU], `cloth.shell` m3a.8, `rigid.bodies` m6.2, `liquid.mac` m7m.1, scene chunks, `rise-sim`, portable CBOR caches, macOS `SimulationsPanel`, 11 `sim_*` agent verbs, and clean macOS/Windows/Android builds). See [§5 Execution status](#execution-status-2026-11-27) below, [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md), and [SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md). The earlier fire-only status doc, [FIRE_SMOKE_STATUS.md](FIRE_SMOKE_STATUS.md), remains the entry point for fire solver physics details.

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

### Execution status (2026-11-27)

All planned milestones **M0–M8** have shipped on `master`:
- **M0–M2 (Cache, Contract, Hosts & Fire Extraction):** General CBOR cache container (`SimulationCache`), native frame codecs (`grid`, `field`, `mesh`, `points`, `transform`), frozen v1 solver contract ([SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md)), `InProcessHost`, `rise-sim` worker, and `fire.methane` extraction (subsequently bumped to **version 3** in `895950cd7`, requiring a `1e-6` product-fraction ignition witness, and to **version 4** in `f7d0b2f4b`: fire-adjacent sources strict IEEE so fire bytes are LTO-independent, make default thin LTO, chemistry light from the local reaction; [FIRE_SMOKE_STATUS.md](FIRE_SMOKE_STATUS.md)).
- **M3 (Cloth — `cloth.shell` m3a.8):** CPU continuum cloth solver with RCM `EnvelopeCholesky` factorization (`cc071c604`), persisted plan-time evidence, well-posed self-collision evidence, deforming-mesh rendering, and live interactive viewport playback.
- **M4–M5 (Scene Chunks, Bake Manager, Agent Verbs & macOS GUI):** `ChunkCategory::Simulation` (`cloth_simulation`, `rigid_simulation`, `liquid_simulation`, `fire_simulation`, `simulation_group`, `simulation_cache`, `simulated_mesh_geometry`, `simulated_points_geometry`), dependency staleness graph, process-wide `SimulationBakeManager`, 11 `sim_*` agent verbs, and macOS `SimulationsPanel` ([SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md)). Remaining UI gap: Windows Qt6 mirror for `SimulationsPanel` and a Windows `rise-sim` VS2022 project.
- **M6 (Coupling & Rigid Bodies — `rigid.bodies` m6.2):** `SimulationGroup` co-stepping host (staggered + implicit Aitken sub-iteration), 5 port adapters, `rigid.bodies` with body–body contact and plane resting, two-way cloth↔rigid coupling, and one-way fire→cloth playback.
- **M7 (Liquid — `liquid.mac` m7m.1):** MAC staggered-grid free-surface liquid with cut-cell static solids, implicitly coupled rigid bodies, constant viscosity, ghost-fluid surface tension, corotational two-way cloth/sheet coupling, free-edge coverage (`m7i`), gradual sheet contact (`m7k`), two-valued level set at sheet-cut cells (`m7l`), and **viscosity with sheets** (`m7m`, `d0256a0a4`). Known limits: spray/thin films, mesh bodies in body–body contact, and coarse-grid free-edge artifacts.
- **M8 (Cross-Platform Playback & CPU Fire):** Portable little-endian cache playback across all codecs, threaded `fire.methane` `cpu` backend (`experimental`, `c3385bfbc`/`d85a0f742`), clean Android arm64-v8a build (`00e1e9455`), and clean Windows MSVC v145 build across `Library`, `RISE-CLI`, Qt6 `RISE-GUI`, and all 503 CMake tests (`b6663f1af`).

After M8, from the showcase round's engine requests ([SIMULATION_SHOWCASE.md](SIMULATION_SHOWCASE.md)):
- **M9 (Inflow, drains, analytic bodies on cloth; `cc746ac42`, preview, no version change):** `liquid.mac` inflow `source`s (box, cylinder nozzle, sphere; window, schedule, rain drops) and `drain`s with exact volume accounting ([Parsers README](../src/Library/Parsers/README.md), "Liquid inflow sources and drains"; `LiquidSourceTest`, `scenes/Tests/Simulation/liquid_inflow.RISEscene`); `rigid.bodies` analytic bodies with a mesh coupling surface (`surface <object>`) so they drive a cloth collider and keep body-body contact (`ClothRigidPrimitiveTest`); the liquid's barrier watertightness check no longer misjudges faces a sagging sheet crosses twice (the `monsoon` proof's 862 false holes → 0). **Assessed, not built:** cloth-body contact inside a liquid group. Missing: (1) a second load port on `rigid.bodies` (or a summing adapter), since the liquid already owns `body_load`; (2) the collider contact in the liquid's linearised copy of an implicitly coupled cloth (the copies part where a body touches the cloth); (3) the cloth as a contact partner of the liquid's own copy of each body; (4) sub-iterated cloth-body coupling beside the liquid's owned exchange, which the group refuses (it would run staggered, one macro step lagged). It changes the liquid's implicit sheet solve and the group policy, not only coupling code.

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
