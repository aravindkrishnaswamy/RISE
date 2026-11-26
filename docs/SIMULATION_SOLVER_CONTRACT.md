# Simulation solver contract (frozen, version 1)

2026-11-23. The specification that closes M3 ([plan](SIMULATION_IN_RISE_PROPOSAL.md) §4.1, §5). It is written so
that a third solver can be implemented from it alone; [Appendix A](#appendix-a-a-liquid-solver-against-the-contract)
walks a liquid solver through it. Code: `src/Library/Simulation/SimulationSolver.h` (contract),
`SimulationHost.h` (in-process host, publisher, group-host and adapter interfaces), `SimulationWorker.h`
(`rise-sim`), `SimulationCache.h` (cache, location policy), and the codecs `SimulationGrid.h`, `SimulationMesh.h`,
`SimulationField.h`. History: [M2 design](SIMULATION_M2_DESIGN.md), [M3a](SIMULATION_M3A_RESULTS.md) §9 and
[M3b](SIMULATION_M3B_RESULTS.md) §8 (the gaps this freeze closes; [Appendix B](#appendix-b-what-the-freeze-changed)).

Conforming solvers today: `fire.methane` (Metal, grid output, worker only), `cloth.shell` (CPU, mesh output,
interactive, ports, colliders), and the test solvers `test.advection_diffusion` and `test.membrane`
(`tests/SimulationReferenceSolvers.h`).

## 1. Roles

- A **solver** advances a state through time and publishes typed, encoded frames. It owns no files, threads,
  clocks, UI or timeline. It is created by id from a process-wide registry (`RegisterSolver`, `CreateSolver`).
- A **host** owns everything else: it configures the solver, completes its description, steps it, receives its
  frames into a cache or an in-memory publisher, checkpoints it, stops it. Hosts: `InProcessHost` (in the app),
  the worker `rise-sim`, and (interface only, M6) the co-stepping group host.
- The **cache** is the only thing consumers (renderer, viewport, other solvers' ports) read; a live run is read
  through the same `IReadOnlyCacheSnapshot` interface as a disk cache.
- Nothing in the contract, the hosts or the cache is solver-specific; anything fire- or cloth-shaped there is a
  defect.

## 2. Versioning

`kSolverContractVersion = 1`. It is bumped only for a change an existing solver or host could observe: removing
or renaming a member, changing a signature, a default value, or the meaning of a field or a call, or a stricter
rule a conforming solver could fail. It is **not** bumped for additive, defaulted changes: a new struct member
whose default reproduces the old behaviour, a new virtual member with a default implementation (as
`SampleTransform`, `ResolveStatic`, `PublishStatic`, `OfferStatic`, `SaveState` were added), a new codec, a new
optional manifest member that is absent when unused, a new capability flag defaulting to "not claimed". Codec
versions (`mesh.v1`/`mesh.v2`) and solver versions (`SolverDescription::version`, which enters the cache key) are
independent of the contract version.

## 3. Lifecycle, threads and errors

Order: `Configure`, then `Start` or `Restore`, then any number of `Advance`. `Configure` may be called again (it
resets the solver; a host then starts or restores again). `Checkpoint`, `SaveState`, `RestoreState` and
`Position` are valid between `Advance` calls, including after one that returned `Stopped`.

- **Threads.** All members are called from one thread at a time (the host serialises them), except
  `RequestStop`, which is thread-safe and may be called at any time. A solver may run its own threads inside a
  call; it must not call the sink, ports or progress from another thread after the call returned, and must not
  call them concurrently.
- **RequestStop is sticky.** The `Advance` in progress, or the next one if none is running, returns `Stopped` at
  its next safe point. The request is cleared only when an `Advance` consumes it by returning `Stopped`. (A request
  is not consumed by an `Advance` that returns `Advanced` or `Finished` before reaching a safe point.) `IProgress::
  Report` returning false also stops the call, without touching the request.
- **Errors.** Every fallible member returns `bool` and fills `error` with a human-readable sentence. After a
  failed `Configure` the solver is unconfigured; after a failed `Start`/`Restore` it is not started; after a failed
  `Advance` its state is unspecified and the host must `Restore`, `RestoreState` or discard it. A failed call never
  leaves a half-published frame: a payload either reached the sink or not. Codecs and cache calls leave their
  outputs untouched on failure.

## 4. Configure and the description

`Configure(parameters, inputs, description, error)`: `parameters` is a canonical CBOR map (the worker accepts JSON,
integral numbers becoming integers). Unknown keys are refused. The solver fills `SolverDescription`:

| Field | Rule |
|---|---|
| `solverId`, `version` | Registry id and solver version; the version changes whenever output bytes for the same parameters change |
| `effectiveParameters` | Canonical: defaults applied, values that cannot change results removed (pace, thread count when `threadCountInvariant`). `Configure(effectiveParameters)` reproduces the same description |
| `cache` | The solver fills time support (`firstIndex`, `count`, `startTime`, `interval`, `hold`), `outputs` (name, codec type, channels with units and representation) and `physicalRecords` (opaque, e.g. a material record). The host owns and overwrites `key`, `epoch`, `producer`, `producerVersion`, `fidelity`, `evidence`, `evidenceConditions`, `evidenceScope` |
| `inputs` | Names of every scene input read (payloads and transforms), unique, in the order their identities enter the key |
| `ports` | See §8 |
| `capabilities` | See §5 |
| `evidence` | See §6 |
| `sideOutput` | A directory leaf for solver-owned exports beside the cache (`ValidSideOutputName`), or empty. Not keyed |
| `notes` | Human-readable configuration notes. Not keyed |

**Cache key** = `MakeCacheKey(solverId, version, effectiveParameters, [(name, Identity(name)) for name in inputs])`
= SHA-256 of canonical CBOR `["rise.simulation.key.v1", id, version, params, [[name, hash]...]]`. **Epoch** =
`DeriveEpoch(key, parent manifest digest)`: a rerun of the same configuration into a fresh cache is
reproducible, a rebake always differs. `PrepareCacheDescription` (every host calls it) validates ports, inputs
and side output, computes key and epoch, writes the producer identity, the effective fidelity and the scoped
evidence digests.

Output times: index `i` is published at `startTime + (i - firstIndex) × interval`, exactly that double. The last
output is at `startTime + (count - 1) × interval`; this is the "end time" everywhere (progress, worker status).

## 5. Capabilities

| Field | Meaning |
|---|---|
| `backends` | Each with name (`cpu`, `metal`, …), status (`supported`/`experimental`/`unsupported`), availability now, detail. Every solver lists a portable CPU path (possibly `unsupported`) |
| `interactive` | Cheap enough, in this configuration, to step live in an app |
| `deterministic` | Identical inputs give identical payload bytes (hosts then verify replays byte for byte) |
| `resumable` | `Checkpoint`/`Restore` reproduce an uninterrupted run |
| `externalMacroStep` | Honours `AdvanceRequest::Mode::MacroStep` (required for co-stepping) |
| `rollback` | `SaveState`/`RestoreState` are implemented (required for sub-iterated coupling) |
| `threadCountInvariant` | Payload bytes do not depend on the solver's thread count (a thread parameter is then unkeyed) |
| `modes` | Operating modes the solver offers (`certified`/`fast` for fire, `bake`/`interactive` for cloth); the configured one is a parameter and an evidence condition |
| `earnableFidelity` | Labels this configuration can earn (`preview`, and `predictive` only if its conditions allow) |
| `fidelityLimit` | One sentence: why `predictive` is not earnable here (empty when it is) |
| `costEstimate`, `memoryEstimate` | Seconds per simulated second on the reference machine, peak bytes; 0 = unknown |
| `backend` | (Additive, Appendix C.) The backend, by name from `backends`, this configuration will run on (`cpu` for cloth, `metal` for fire); empty = not stated. A scheduler applies "one GPU solver at a time" from it |

## 6. Fidelity and evidence

- A solver **requests** a label (`Evidence::fidelity`) and supplies `cases`, each with a computed `error`, a
  `tolerance`, `passed`, and a digest `EvidenceDigest(id, configuration, error, tolerance, passed)` binding the case
  to the configuration that produced it.
- It states what the label **covers**: `conditions` (machine-readable: strictly increasing names, each with a
  nonempty strictly increasing set of values; e.g. cloth: `bending`, `colliders`, `contact`, `drag`, `integrator`,
  `material`, `mesh`, `mode`, `motion`, `self_collision`, `triangulation`) and `scope` (one sentence, including
  what it does **not** cover).
- The host derives the label: `EffectiveFidelity` is `predictive` only when requested, a scope is stated, the
  conditions are valid and every case is present, finite and within tolerance with a valid digest; else `preview`.
  Hosts write that label, never the requested one.
- The host records `conditions` and `scope` in the cache description and writes, per case,
  `ScopedEvidenceDigest(case digest, conditions, scope)` (SHA-256 of canonical CBOR
  `["rise.simulation.evidence.scope.v1", digest, conditions, scope]`; the case digest itself when both are empty).
  Changing what a label claims to cover therefore changes its evidence digests.
- Consumers that request `predictive` fail closed (`PreflightPredictive`: complete, current key, predictive
  label, consumer-supplied qualification of the snapshot and of every verified frame, every static payload
  verified).
- Evidence certifies a model family on canonical problems; it does not bound a given bake's error unless a case
  says so. A solver that cannot earn `predictive` says why (`fidelityLimit`).

## 7. Scene inputs

`ISceneInputs` (host-implemented, `const`, callable concurrently from the solver's own threads; the object passed
to `Configure` stays valid until the solver is configured again or destroyed):

- `Identity(name)` — content hash of the input over the whole simulated span (payload and motion). It enters the
  cache key, so a change of the scene element makes the cache stale.
- `Sample(name, time)` — the encoded payload (codec named in `codec`) at a solver time; `ResolveStatic(digest)`
  serves static payloads such a payload references (a `mesh.v2` collider).
- `SampleTransform(name, time)` — a rigid transform (`R x + t`, metres) at any solver time, including inside a
  step; this is how pins and colliders follow scene objects (cloth: motion `{type: scene_transform, input: name}`).
- `NoSceneInputs` / `EmptySceneInputs()` — the empty scene (`rise-sim bake <solver id>`). Since M4 a scene supplies
  real inputs (`Simulation/SceneInputs.h`: detached object snapshots, world-space `mesh.v1` samples, rigid parts of
  keyframed transforms) to `rise-sim bake <scene>` and to live sessions; identities are defined in
  [M4 results](SIMULATION_M4_RESULTS.md) §3.

## 8. Ports and coupling

A `PortDeclaration` names a port, whether it **offers** or **accepts**, its codec, physical quantity and units, the
list of `channels` (name, quantity, units, components) its payloads carry, an optional `domain` (the index-space
digest of a `field.v1` port when fixed by the configuration, e.g. cloth's topology digest), and `output`: the
cache output carrying the same payloads. **An empty `output` means live-only**: the port works between live
solvers but cannot be fed from a cache.

- **Compatibility.** `PortsCompatible(offer, accept)` requires the same codec, quantity, units and channel list,
  and equal domains when both are stated. Solvers require exact matches. Unit conversion, resampling between index
  spaces or lattices and channel mapping are the group host's job through registered `IPortAdapter`s (interface
  and registry now, implementation in M6); adapters are pure functions of the input bytes.
- **Timing (baseline: explicit, staggered).** A solver reads accepted ports at the start of each macro step
  (`Accept(port, step start)`, returning the latest value at or before that time) and holds them over the whole
  step. Free-running (`NextOutput`), each segment between output times is a step. It offers at the end of each
  step, stamped with the step-end time (also at every output time it publishes). Exchange order (Jacobi or
  Gauss–Seidel) is the group's policy, never the solver's.
- **Implicit coupling.** A group host that sub-iterates calls `SaveState` before the step and `RestoreState` before
  each repeat; the solver then re-reads its ports at the step start and repeats exactly what an uninterrupted run
  with those values would do. Frames published during a repeated step are superseded; the group host holds them
  back until the step is accepted. `GroupPolicy::subIterations > 1` requires `rollback` on every live member.
- **Owned implicit exchange** (additive, M7 prerequisites). A member that solves a partner's motion inside its own
  step (a liquid whose pressure solve includes a rigid body's six degrees of freedom) declares the ports of that
  exchange in `Capabilities::ownedImplicitPorts`; the group lists it in `GroupPolicy::ownedExchanges` (owner and
  partners). The group then exchanges those connections once per macro step (Gauss-Seidel, the owner first: it
  reads the partner's state at the step start, the partner integrates the owner's end-of-step offer) and does not
  sub-iterate them; the label treats them as implicit when every owned port is declared and the owner's own
  evidence, which must cover the joint solve, is predictive. `rigid.bodies` offers what an owner needs:
  `body_mass_properties` (mass, body-frame inertia, centre of mass; constant, offered at the start time) beside
  `body_state`, and integrates `body_load`. Sub-iterating other members beside an owned exchange is refused.
- **Static payloads through ports.** A port payload may reference static payloads (a `mesh.v2` surface). The
  offering solver calls `OfferStatic` before the first `Offer` that references it; the accepting solver resolves
  through `ResolveStatic` (`IPortExchange::Resolver()`). `PortRelay` keeps offered statics; `CachedPortFeed`
  resolves them from its snapshot.
- **Status.** `Accept` returns `Ready`, `Unbound` (not connected), `Unavailable` (nothing at or before the time) or
  `Failed`. A solver treats `Unbound`/`Unavailable` as "no load" and `Failed` as an error.

## 9. Stepping

`AdvanceRequest{NextOutput}` advances until the next output index has been published (or the end).
`AdvanceRequest{MacroStep, macroEnd}` advances exactly to `macroEnd`, publishing every output index reached on the
way; `macroEnd` in the past is refused; the solver chooses its own substeps (cloth and the reference solvers split
each segment into `ceil(length / nominal substep)` equal substeps, so macro ends on the output grid reproduce free
stepping byte for byte). `Advance` may return `Stopped` at any safe point; the state is then valid for `Checkpoint`
and a later `Advance` continues the same sequence of substeps. `AdvanceResult` reports the status, the position
(`time`, `nextIndex`, `finished`) and the number of payloads published. An `Advance` after the end returns
`Finished` and publishes nothing. `IProgress::Report(simulated time, end time = last output time, detail)` may be
called inside long steps; returning false stops at the next safe point.

## 10. Frames and static payloads

- `IFrameSink::Publish(index, output, time, payload, receipt)` hands over an immutable shared payload in the
  output's codec; `receipt` is opaque per-frame provenance. Each (index, output) is published once per run; a
  resumed or replayed run may publish an already committed pair again and must then produce identical bytes
  (non-deterministic solvers: the worker keeps the committed frame).
- **Static payloads**: immutable, content-addressed (SHA-256) blobs of a static codec type
  (`mesh.topology.v1`), published with `IFrameSink::PublishStatic(type, payload, digest)` and referenced by frames.
  A frame may reference a static payload only after it was published to the same sink in the same run (after
  `Start` or `Restore`); publishing it again is a no-op. Sinks refuse a native frame whose references are not
  published. Fixed-topology streams publish their connectivity once; changing-topology streams carry or reference
  a different connectivity per frame.

## 11. Checkpoint, restore, rollback

- `Checkpoint` returns an opaque, solver-versioned byte string (solvers use canonical CBOR). `Restore(state)` on a
  freshly configured solver with the same effective parameters continues exactly as the uninterrupted run would,
  including from the middle of a segment and with held port values. A solver refuses a malformed or foreign
  state. Hosts tie checkpoints to the cache key (worker: also solver id and version, and the position, verified
  after `Restore`).
- `SaveState`/`RestoreState`: an in-memory `SolverStateToken` that may be restored any number of times on the
  instance that made it (others refuse it). It captures everything `Checkpoint` does; cloth rebuilds its system
  from it exactly as from a checkpoint, so a rollback equals a re-run (`ClothSolverTest`, `SimulationHostTest`).
  Fire reports `rollback = false` (default implementation).
- **Streamed checkpoints** (additive, M7 prerequisites; Appendix C). `CheckpointTo(ICheckpointSink&)` writes the
  checkpoint in chunks (`Write(data, size)`), `RestoreFrom(ICheckpointSource&)` reads it back (`Read` fills exactly
  the bytes asked; `Remaining()` is what is left) instead of `Restore`, and must consume the whole stream. The
  stream may have another byte layout than `Checkpoint`, but both must resume identically. The defaults write
  `Checkpoint`'s buffer in one call and read the stream into one buffer for `Restore`, so a solver that does not
  override them (fire, cloth, rigid) behaves as before. A solver with a multi-gigabyte state overrides them and
  never holds a second copy. `MemoryCheckpointSink` / `MemoryCheckpointSource` keep a stream in memory.

## 12. Determinism

A `deterministic` solver produces identical payload bytes for identical effective parameters and input
identities on any host (in-process and worker hosts are tested byte for byte), for stop/resume, for
`threadCountInvariant` thread counts, and for macro steps on the output grid. Different macro grids agree only to
discretisation error. Non-deterministic solvers (fire fast mode) say so; the worker then does not verify replays.

## 13. What hosts guarantee to solvers, and vice versa

Hosts guarantee: one calling thread at a time; scene inputs alive for the configuration's lifetime; a sink that
accepts static payloads; `PrepareCacheDescription`; a private, owner-only side-output directory when one is
declared and the run is on disk (`SetSideOutputDirectory` before `Start`/`Restore`, emptied for a new epoch); stop
requests through `RequestStop` only; checkpoint bytes returned unchanged.

Solvers guarantee: no file, thread, clock or global-state side effects beyond the side-output directory (fire, one
per process, is the documented exception: it keeps process-wide state and refuses a second live instance);
honest capabilities and evidence; payloads valid in their declared codecs; statics before references; the
timing rules of §8; exact checkpoints when `resumable`.

**InProcessHost**: caller-thread (`Step`, `StepToIndex`) or worker-thread (`Play`, `Pause`, `WaitIdle`) stepping;
frames go to a `MemoryPublisher` and optionally to a recording `CacheWriter` (`Record`); generations share storage
(a persistent 32-way vector: a publish costs O(log n), not a copy of the entry list; `Entries()` materialises once
per generation, so a reader following a live run should use `FindEntry`, `EntryCount` and `Span`); snapshots are immutable. `Pause`
interrupts the current step; a stop that misses it is consumed by the host's next step, never leaked into a later
caller step. `RestoreCheckpoint` discards frames at or after the checkpoint's next index from later snapshots (a
recording keeps its committed frames and refuses conflicting republication). `SetSideOutputDirectory` lets a
side-output solver run in-process; a recording host uses `<cache>/<sideOutput>`.

**Worker** (`rise-sim bake|resume|status`): a cache writer per run, sidecars `rise-sim.job.cbor`,
`rise-sim.checkpoint` (since the M7 prerequisites, version 2: header, the solver's `CheckpointTo` stream, and a
trailer with the stream's length and SHA-256, written through a staged file, fsync and rename; resume verifies the
digest before the solver reads a byte and re-hashes what it reads; version-1 files, header + state, are still
resumed. A streaming solver's 1 GB state peaks at the state's size, writing or resuming; through the default
`CheckpointTo` it peaks at the state plus the solver's own checkpoint copy, as before), `rise-sim.status.cbor` (simulated time /
end = last output time); checkpoints at a wall-time cadence and on SIGINT/SIGTERM through one sticky
`RequestStop`; resume verifies job, key, epoch, version and position.

## 14. Cache

- **Manifest** `manifest.cbor`, canonical CBOR, replaced durably per generation. Format
  `rise.simulation.cache.v1` (4 members: format, description, frames, complete) is written whenever there are no
  static payloads and no evidence scope, byte-identical to caches written before the freeze; otherwise
  `rise.simulation.cache.v2` adds `statics` (`[digest, type, size]`, sorted by digest) and the description may carry
  `conditions` and `scope`. Readers accept both.
- **Frames** `<sha256>.frame`, **static payloads** `<sha256>.static`: immutable, written once (`linkat`), owner-only
  (0600), verified on every read against the manifest digest and size, read with a caller budget
  (`BudgetExceeded` before allocating). A native frame may be appended only when its referenced statics are
  declared; resume verifies every frame and static.
- **Snapshots**: `IReadOnlyCacheSnapshot` (`Description`, `Entries`, `FindEntry`, `EntryCount`, `Span`, `Statics`, `Read`, `ReadStatic`,
  `Resolver`, `MapTime`, `PreflightPredictive`) is served by disk (`CacheSnapshot`) and memory
  (`MemoryCacheSnapshot`) alike.
- **States**: missing, baking (writer lock held), partial, complete, corrupt; a stale cache is one whose key no
  longer matches (`Stale`).

### Cache location policy

The writer (POSIX) verifies the cache directory through one chain of directory descriptors from `/`, never
following a symlink below the canonicalised existing prefix; it refuses an ancestor that another user could
modify (owner neither this user nor root, group/world-writable without the sticky bit, or an ACL that grants
rights) and a cache directory that is a symlink, not owned by this user or writable by anyone else; it creates
every missing directory 0700, files 0600, and uses the canonical path for everything that follows.

*What the refusal protects against:* another local user (or any member of a shared group) who can rename entries
in an ancestor could swap a directory between the check and the writes, redirecting payloads, manifests and
sidecars elsewhere, or substitute a cache whose frames a later render (even a `predictive` one) trusts.

*Trusted roots* (`RISE_SIMULATION_TRUSTED_ROOTS`, colon-separated absolute paths; relative or missing entries are
ignored; and `AddTrustedCacheRoot` for the GUI) stop the ancestor check at the root: only the components below the
root are checked, the cache directory and everything created below it are still owner-only, and the descriptor
walk, `O_NOFOLLOW` opens, writer lock and digest verification all stay in force. *What trusting a root gives up:*
protection against whoever can write the root or its ancestors (for a 0775 external drive, the drive's group) —
they could still swap the first directory below the root. Trust a root only when everyone who can write it is
trusted with the caches. Windows keeps its previous checks (no ancestor policy).

## 15. Codecs

| Type | Kind | Status | Spec |
|---|---|---|---|
| `grid.v1` | frame | implemented | [grid](SIMULATION_GRID_FORMAT.md): sparse blocked binary32 grids, channels with units |
| `mesh.v1` | frame | implemented, read forever | [mesh](SIMULATION_MESH_FORMAT.md): triangles, positions, velocities, normals |
| `mesh.v2` | frame | implemented | [mesh §v2](SIMULATION_MESH_FORMAT.md#mesh-v2): + UV sets with units, named attributes, static topology reference |
| `mesh.topology.v1` | static | implemented | [mesh §v2](SIMULATION_MESH_FORMAT.md#mesh-v2): connectivity + static UV sets |
| `field.v1` | frame / port | implemented | [field](SIMULATION_FIELD_FORMAT.md): channels over a domain digest |
| `points.v1` | frame | implemented (M7 prerequisites) | [points](SIMULATION_POINTS_FORMAT.md): positions, optional velocities, uniform or per-point radius, u64 ids, named attributes with units |
| `transform.v1` | frame / port | implemented (M6) | [transform](SIMULATION_TRANSFORM_FORMAT.md): named rigid bodies, position, unit quaternion, linear and angular velocity, binary64 |

## Appendix A. A liquid solver against the contract

### A.1 Walk-through

A hypothetical `liquid.flip`: FLIP particles, a reconstructed surface mesh whose topology changes every frame, an
optional density/velocity grid, two-way coupled to `cloth.shell`.

| Step | How it maps | Works today? |
|---|---|---|
| Registry, parameters | `RegisterSolver("liquid.flip")`; canonical CBOR map; effective parameters fixed point | yes |
| Backends, modes | `cpu` supported, `metal` experimental; `modes {bake, interactive}` | yes |
| Inputs | Colliders by `Sample` (`mesh.v1`/`mesh.v2` through `ResolveStatic`) at substep times; emitters and rigid boundaries by `SampleTransform`; all named in `inputs` so their identities key the cache | yes (M4: `rise-sim bake <scene>` feeds scene objects; the scene syntax for a liquid is not written yet) |
| Outputs | `particles` as `points.v1`; `surface` as `mesh.v2` with inline indices (or a fresh `mesh.topology.v1` per frame) and velocities of the current frame's vertices; `volume` as `grid.v1` | yes (M7 prerequisites: `points.v1` implemented; `simulated_mesh_geometry` renders a changing-topology or empty `mesh.v2`) |
| Evidence | Dam-break front against published data, hydrostatic rest, volume conservation; conditions (resolution, boundary kinds, surface-tension on/off) and a scope sentence | yes |
| Ports to cloth | Accept cloth's `surface_state` (`mesh.v2`, channels normal/position/velocity, resolved via `ResolveStatic`), declared with an identical channel list; offer `cloth_load` as `field.v1` over the cloth's topology digest (read from the accepted payload with `PeekMeshTopology`), channels `pressure` [Pa] x1 and `traction` [Pa] x3, `domain` left empty in the declaration (decided per payload; cloth's is stated, so `PortsCompatible` passes) | yes; the offered field is live-only unless the liquid also declares a `field.v1` output for it |
| Timing | Accept cloth at each macro-step start, offer the load at its end; cloth does the same | yes (solver side); the group host that runs the exchange is M6 |
| Implicit coupling | `SaveState`/`RestoreState` on both | yes (solver side); sub-iteration is M6 |
| Macro steps | Adaptive CFL substeps inside each macro step, ending exactly at `macroEnd` | yes |
| Determinism | Parallel particle-to-grid transfer with atomics is not bit-reproducible: `deterministic = false` (worker keeps committed frames), or a deterministic reduction and `threadCountInvariant` | yes |
| Checkpoints | Particles + grid, streamed (`CheckpointTo` / `RestoreFrom`) | yes (M7 prerequisites: the worker holds a streamed state once) |
| Rendering | Refractive surface from `surface`, spray from `particles` | yes (`simulated_mesh_geometry` per-frame rebuild; `simulated_points_geometry`) |
| Rigid bodies in the pressure solve | Accept `body_state` and `body_mass_properties` from `rigid.bodies`, offer `body_load`; the group marks the exchange as owned by the liquid (`GroupPolicy::ownedExchanges`), the liquid declares `Capabilities::ownedImplicitPorts` | yes (M7 prerequisites) |

### A.2 `points.v1` (implemented)

Specified here at the freeze as a channel list; implemented in the M7 prerequisites with typed optional records
instead (uniform or per-point radius, velocity units, u64 ids, named attributes): [points format](SIMULATION_POINTS_FORMAT.md),
which records the differences. The per-point `field.v1` domain is `PointsDomain`: SHA-256 of the id words when ids
are present, else of the position words.

### A.3 What would not work, and what this freeze fixed for it

Fixed now (cheap): static payloads in scene inputs (`ISceneInputs::ResolveStatic`) so a `mesh.v2` collider can be
sampled; static payloads through ports (`OfferStatic`/`ResolveStatic`); per-vertex port payloads on another
solver's mesh (`field.v1` with a domain); undeclared `domain` decided per payload; `points.v1` specified.

Not working yet (each belongs to a later milestone):
1. ~~`points.v1` encoder/decoder and its renderer path (M7).~~ Done in the M7 prerequisites (`SimulationPoints.h`, `simulated_points_geometry`).
2. ~~Rendering a changing-topology or empty mesh (`SimulatedMeshSource` refuses both; M3b §8).~~ Done in the M7
   prerequisites (per-frame rebuild when the topology digest changes; [results](SIMULATION_M7_PREREQ_RESULTS.md)).
3. ~~The co-stepping group host, port adapters and sub-iteration (interfaces only; M6).~~ Done in M6 (`SimulationGroup.h`, `SimulationPortAdapters.h`; [M6 results](SIMULATION_M6_RESULTS.md)).
4. ~~Scene inputs in the worker (M4).~~ Done in M4 (`rise-sim bake <scene>`).
5. ~~Streaming or chunked checkpoints for very large states (all of `Checkpoint` is one in-memory `Bytes`).~~ Done
   in the M7 prerequisites (`CheckpointTo` / `RestoreFrom`, Appendix C).
6. ~~No `Capabilities` field says which backend a configuration will actually run on.~~ M4 added
   `Capabilities::backend` (Appendix C).
7. ~~(M4) A one-way coupling fed from a cached output through a port (`CachedPortFeed`) is not keyed~~ (fixed in M6: the
   group host adds the key input `rise.coupling`, covering the upstream cache key and the adapter identity): the key covers
   `inputs` (scene inputs) only, so a downstream simulation fed through a port would not go stale when its upstream
   changes. M4 binds an upstream simulation through a scene element instead (an object showing its output), whose
   identity embeds the upstream key; keying port feeds (an additive key input) belongs with M6's group host.

## Appendix B. What the freeze changed

| Gap (M3a §9 / M3b §8) | Decision | Where |
|---|---|---|
| Mesh attributes, UVs, connectivity re-sent every frame | `mesh.v2` (UV sets, attributes, length units), static payloads as a generic cache feature; `mesh.v1` stays readable | `SimulationMesh`, `SimulationCache`, `SimulationHost` (memory), `SimulatedMeshSource`, `SimulatedMeshGeometry` |
| Per-vertex port payloads | `field.v1`; ports declare channels and domain; cloth `surface_load` is per-vertex | `SimulationField`, `PortDeclaration`, `ClothSolver` |
| Port timing, macro steps | Accept at macro-step start and hold; offer at step end stamped with its time; rollback for implicit coupling; adapters for non-identical ports; live-only ports | `SimulationSolver.h`, `SimulationHost.h`, cloth, reference solvers |
| Stop between calls ignored | Sticky `RequestStop` | all solvers, `InProcessHost::Pause`, worker watcher |
| `fidelityModes` used two ways | `modes` and `earnableFidelity` (+ `fidelityLimit`) | all solvers |
| Collider and pin motion inside a step | `SampleTransform`; cloth motion `scene_transform` | `ISceneInputs`, `ClothModel` (`Motion::Track`), `ClothSolver` |
| Label conditions, thread count | `Evidence::conditions`, `Evidence::scope` (recorded, in the digests); `threadCountInvariant` | `SimulationSolver`, `PrepareCacheDescription`, cloth |
| Host gaps | In-process side output; restore discards later frames; shared snapshot storage; worker end time = last output; checkpoint without an extra copy | `SimulationHost`, `SimulationWorker` |
| Cache on 0775 drives | Ancestor policy kept (now enforced by the native writer) with trusted roots | `SimulationCache` |
| Progress end time `1 / 1.0333` | End time = last output time | cloth, reference solvers, worker |

## Appendix C. Additive changes after the freeze (contract version stays 1)

| Date | Change | Default / compatibility |
|---|---|---|
| 2026-11-23 | `IReadOnlyCacheSnapshot::EntryCount()` and `Span(output)` (`OutputSpan`: frame count, lowest and highest index of one output). Readers that follow a live run use them (and `FindEntry`) instead of `Entries()`; the disk snapshot indexes its spans once per generation, the memory snapshot keeps them per generation in O(outputs). | Virtual with defaults computed from `Entries()`; third-party snapshots keep working unchanged. |
| 2026-11-23 | `EncodeMeshV2` overload taking a prepared `MeshStaticRecord` (`PrepareMeshStatic`): a fixed-topology stream encodes and hashes its static record once; frames are byte-identical to the existing call. Cloth uses it. | New functions only; no codec or solver version change. |
| 2026-11-23 | `PortRelay::OfferStatic` skips the hash when it is offered the same shared payload again. | Same relay contents. |
| 2026-11-23 | (M4) `Capabilities::backend`: the backend this configuration runs on (cloth `cpu`, fire `metal`). | Empty string = not stated; not keyed; existing caches and keys unchanged. |
| 2026-11-23 | (M4, host side) `rise-sim bake\|resume\|status <scene.RISEscene>`; the worker no longer refuses a configuration with scene inputs (it bakes it from its scene; `bake <solver id>` keeps the empty scene). `WorkerBakeConfiguration`, `WorkerResumeConfiguration`, `ReadWorkerJobRecord`. | Same sidecars, key and epoch rules; `bake <solver id>` behaves as before. |
| 2026-11-23 | (M4, not part of the contract) Solver parameter schemas (`SimulationParameterSchema.h`, `RegisterSolverSchema` beside `RegisterSolver`): names, types, defaults, ranges, units, docs; scene producer chunks are generated from them. | Optional; a solver without one still works through the contract. |
| 2026-11-23 | `FindTrustedCacheRoot(path, root)`: the one decision of which trusted root a location falls under (`SecureCacheDirectory` and `DiskFileWriteBuffer`'s staging-ancestor check both use it; opt-in, nothing trusted by default). | New function; the cache writer's behaviour is unchanged. |
| 2026-11-23 | (M5, host side, not part of the solver contract; [M5a results](SIMULATION_M5A_RESULTS.md)) Worker: `rise-sim.status.cbor` gains the optional members `pid`, `pid_start`, `host` and the state `waiting-gpu`; a configuration whose `Capabilities::backend` is stated and not `cpu` takes an advisory GPU lock (`--gpu-lock`, `RISE_SIM_GPU_LOCK`, per-user default) before `Start`/`Restore`; a `rise-sim.stop` file in the cache directory stops the worker like SIGINT; scene bakes take `--no-upstream` and `--expect-key NAME=KEY`. `WorkerStatus` and `WorkerOptions` gain defaulted members. `SceneSimulationStatus` gains `memoryEstimate`/`costEstimate` (from `Capabilities`). | Readers ignore the new status members (older records have none); a solver that states no backend or `cpu` never touches the lock; keys, epochs, payloads, manifests and every other sidecar are unchanged. |
| 2026-11-23 | (M5c; [M5c results](SIMULATION_M5C_RESULTS.md)) Cloth and fire fill the existing `Capabilities::costEstimate` and `memoryEstimate` (cloth from vertices, substeps and mode, measured; fire from the driver's host-memory admission reserve for the tier's grid and the r275 s/step). Host side, not part of the contract: `ParameterSchema::describeUnkeyed` (a predicted solver states its unkeyed time support and estimates), `SceneSimulationStatus::staleReason` / `wallEstimate`, `FormatSimulationEstimate`, `PlaybackClock` time scale. | No new contract field; estimates are not keyed, so keys, epochs, payloads, manifests and outputs are unchanged (fire's predicted key checked equal in `SimulationSceneTest` M). `describeUnkeyed` null = the previous behaviour. |
| 2026-11-25 | (M6; [M6 results](SIMULATION_M6_RESULTS.md)) `IPortAdapter` gains `Identity()`, `Fidelity()`, `Scope()` and `ConvertWithAuxiliary(...)` (payloads of other offered ports an adapter reads); `PortConnection` gains `adapter`, `adapterParameters`, `auxiliary`; `GroupPolicy` gains `relaxation`, `aitken`, `absoluteTolerance`, `onNonConvergence` (`Fail`/`Halve`), `minimumMacroStep`. | Virtual members with defaults (identity empty = the registry id, fidelity `preview`, auxiliary ignored); new struct members default to the behaviour of the fields before them. |
| 2026-11-25 | (M6) A group host may append the key input `rise.coupling` to a coupled member's `SolverDescription::inputs`; its identity covers the policy, every member upstream of it (solver, version, effective parameters, input identities; a cached member's key, plus its manifest digest when foreign, plus a time offset when set) and every connection into them with its adapter's identity. A member nothing feeds keeps its standalone key. | Absent for every uncoupled configuration: their keys, epochs and caches are unchanged. |
| 2026-11-25 | (M6) Codec `transform.v1` ([spec](SIMULATION_TRANSFORM_FORMAT.md)) is a native payload type; `TransformBodyDomain` names per-body `field.v1` data. | New codec; existing caches unaffected. |
| 2026-11-25 | (M6, host side) `SimulationGroup` implements `ISimulationGroupHost`; `RegisterPortAdapterFactory` / `CreatePortAdapter` beside `RegisterPortAdapter`; `RegisterCouplingEvidence`; group manifest sidecar `rise-sim.group.cbor` in each recorded member cache and `rise-sim.group.checkpoint` in the group directory; `WriteWorkerStatus`. Solver side (additive, no version change for existing parameters): `cloth.shell` mesh colliders take `motion: {type: port, port: <name>}` and then offer `collider_reaction`; new solver `rigid.bodies`. | Existing parameters, keys and payloads unchanged; the cloth checkpoint gains `port_colliders` only when a port-driven collider exists. |
| 2026-11-25 | (M7 prerequisites; [results](SIMULATION_M7_PREREQ_RESULTS.md)) Codec `points.v1` implemented ([spec](SIMULATION_POINTS_FORMAT.md); refines the Appendix A.2 sketch: typed records, uniform or per-point radius, u64 ids) and a native payload type of the disk cache and the in-memory publisher. | New codec; existing caches unaffected. |
| 2026-11-25 | (M7 prerequisites) Streamed checkpoints: `ICheckpointSink` / `ICheckpointSource`, `MemoryCheckpointSink` / `MemoryCheckpointSource`, `ISimulationSolver::CheckpointTo` / `RestoreFrom`, `ISimulationGroupHost::CheckpointTo` / `RestoreFrom` (the group's: an index of member stream lengths, then the members' streams). Host side: `InProcessHost::SaveCheckpointTo` / `RestoreCheckpointFrom`, `DurableSidecarWriter`, `SidecarReader`, `WriteStreamedCheckpoint` / `ReadStreamedCheckpoint`; the worker and the scene group worker write version-2 checkpoint files and still resume version 1. | Defaulted virtuals reproduce `Checkpoint` / `Restore`; no solver version, key or payload changes; an older worker cannot resume a version-2 file. |
| 2026-11-25 | (M7 prerequisites) `Capabilities::ownedImplicitPorts` (ports a solver couples implicitly inside its own step) and `GroupPolicy::ownedExchanges` (connections between an owner and its partners exchanged once per macro step, Gauss-Seidel with the owner first; implicit for the label when the owner declares the ports and its own evidence is predictive). `rigid.bodies` offers `body_mass_properties` (`field.v1` over the body set: centre of mass, body-frame inertia, mass; offered once at the start time). | Empty by default: group identities, keys and labels of every existing group are unchanged (the policy member enters the identity only when used). The new rigid port is live-only; the rigid key and payloads are unchanged. |
| 2026-11-25 | (M7 prerequisites, host side, not part of the contract) `simulated_mesh_geometry` renders a changing-topology or empty mesh (full rebuild when the frame's topology digest differs from the installed one; refit otherwise, unchanged); `simulated_points_geometry` (IJob tail `AddSimulatedPointsGeometry`); the BVH builder's opt-in parallel build (same tree as serial); recycled mesh mailbox ids. | Fixed-topology streams install exactly as before; scenes without the new chunk are unchanged. |
