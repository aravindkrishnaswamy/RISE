# M4 results: simulations as scene elements

2026-11-23. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §2, §4.3–4.5, §5 (M4).
Contract: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (v1, unchanged; additive entries in its
Appendix C). Builds on [M3b](SIMULATION_M3B_RESULTS.md) (`simulated_mesh_geometry`) and [M3c](SIMULATION_M3C_RESULTS.md)
(viewport playback, live attach).

**Status.** A scene file now fully describes its simulations:

- three producer chunks in a new `ChunkCategory::Simulation`;
- consumers bound to a producer's output by `source <simulation>.<output>`;
- scene objects read as solver inputs (identity, world-space samples, rigid transforms);
- a dependency graph with a cache key, a state and propagation per simulation;
- `rise-sim bake|resume|status <scene>`;
- live viewport runs from the scene with no pre-bake.

All six stages of the brief were done. What was not done or not verified is listed in §9.

## 1. Syntax

Scene syntax is documented in [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) ("Simulation producers").
There are three sample scenes in `scenes/Tests/Simulation/`, with comments:

- `cloth_flag_scene` — a pin follows a keyframed pole;
- `cloth_chain_scene_inputs` — a moving ball collider, and a second cloth that collides with the first, which makes a
  dependency;
- `fire_campfire_scene` — a fire read by `fire_medium`.

```text
cloth_simulation
{
	name			flag                 # unique, no `.`
	cache			caches/flag          # M4 default caches/<name> beside the scene; since 2026-11-26 the per-user cache folder (SOLVER_CONTRACT, cache location policy)
	mode			interactive          # any cloth.shell parameter, by its solver name
	vertices_u		31
	vertices_v		31
	frames			91
	origin			-0.5 1.2 0
	pin				v0 follow pole       # <u0v0|u1v0|u0v1|u1v1|u0|u1|v0|v1|boundary> [depth N]
	                                     #   [fixed | translate vx vy vz | oscillate ax ay az hz |
	                                     #    rotate cx cy cz ax ay az rad/s | follow <object>]
	follow_object	pole                 # scene input: the object's keyframed transform
	collider		sphere 0 0.3 0 0.3   # plane p n | sphere c r | box min max | cylinder p axis r (world)
	collider_object	ball                 # scene input: the object's geometry as a mesh collider moving with it
	material		denim-twill          # plus material_density, material_stretch_warp, ... overrides
	contact			barrier
}

simulated_mesh_geometry
{
	name			geo_flag
	source			flag.surface         # or: source flag + output surface
	time_offset		0                    # retiming as in M3b: time_offset, time_scale, end_policy, missing_frame
	double_sided	TRUE
}

fire_simulation
{
	name			campfire             # fire.methane: tier, mode, frame_origin_s, end_time_s, the 20 options
	tier			6
	mode			fast
	frame_origin_s	1.0
	end_time_s		3.0
}

fire_medium
{
	name				flame
	fidelity_mode		preview
	source				campfire.volume      # instead of sequence_manifest (exactly one of the two)
	channel_carbon		carbon
	channel_temperature	temperature
	chem_model			none
}

simulation_cache
{
	name			fabric               # a foreign producer: never baked, never stale, preview
	cache			~/.cache/rise/fabric
}
```

**Common fields.** `name` and `cache` are common to all three producers. Duration and output rate are the solver's
own names: cloth's `frames` and `frame_rate`, fire's `end_time_s` and `frame_cadence_s`.

**Simulation time is scene time.** A producer has no offset of its own. Consumers retime:

- `simulated_mesh_geometry` keeps M3b's `time_offset`, `time_scale`, `end_policy cache|hold|loop|error` and
  `missing_frame hold|hide`, unchanged.
- `fire_medium` takes its time map from the fire manifest, as before. There is no consumer retiming there.

**Object transforms from a simulation (specified, not built).** The syntax is
`standard_object { transform_source <simulation>.<output>[<body>] }`. It is a `Reference` to `{Simulation}` with a
body index or name. It needs the transforms codec (contract §15, not specified), which does not exist, so the
parameter is not registered.

**Reference legality.**

- `source` is a `Reference` to `{Simulation}`. The CST reference graph strips the `.output` suffix, so the editor
  draws the producer→consumer edge.
- `follow_object` and `collider_object` are repeatable `Reference`s to `{Object}`.
- `simulated_mesh_geometry` accepts a mesh output only (mesh.v1/mesh.v2).
- `fire_medium` accepts a fire grid output only. That means grid.v1, plus `carbon` and `temperature` for a
  `simulation_cache`.

Diagnostics name the producer, the output and its codec.

**Declaration order.**

- A producer is declared before its consumers.
- The editor's insert tier (`Job.cpp` and `AgentSession.cpp`) puts producers with geometry.
- Scene inputs resolve after the derive, so a producer may name objects declared after it.

## 2. The parameter schema (not part of the contract)

`Simulation/SimulationParameterSchema.h` is a registry beside the solver registry: `RegisterSolverSchema(id, fn)`
and `FindSolverSchema(id)`. Each solver states its parameters there:

- name, type (bool, count, real, text, enum, vector3), default, mode-dependent defaults, inclusive range, enum
  values, units and doc;
- whether the parameter is keyed;
- for text, the solver's own validator;
- chunk-only structured fields, with a `translate` function from chunk text to the solver's map;
- the outputs, by name and codec.

`RegisterBuiltinSolvers` (and `rise-sim`) registers cloth's and fire's schemas beside their factories.

**What uses the schema.** The producer chunk descriptors are generated from it (`SimulationChunks::Build` in
`ChunkParserRegistry.cpp`):

- each schema parameter becomes a chunk parameter of the same name, value kind, `defaultValueHint`, `unitLabel` and
  `SoftRange`, with its doc as the description;
- an enum keeps its values;
- an object reference becomes a `Reference` to `{Object}`.

So the parser, the properties panel, the suggestion engine, the syntax highlighters and the agent schema all read
one table.

**Cloth** (`Cloth/ClothParameterSchema.{h,cpp}`).

- The schema states the defaults that `ClothSolver::Configure` applies in its own reader. Configure was not
  refactored to read the schema: the other agent was editing `ClothSolver.cpp` at the same time.
- Agreement is enforced by `SimulationSceneTest` A:
  - every keyed default, in bake and in interactive mode, equals Configure's effective value;
  - every effective parameter is described by the schema;
  - every enum value configures, and a bogus value is refused;
  - ranges are checked.
- The chunk-only fields are `material`, `material_*`, `pin`, `follow_object`, `collider` and `collider_object`.

**Fire** (`Fire/FireParameterSchema.{h,cpp}`).

- The case fields are Configure's. The 20 result-affecting options are generated from the solver's own option table:
  a new `FireSolverOptionInfos()` exports their names, kinds, defaults and validators. Nothing about the options is
  restated.
- The fire solver must not be configured in an application process (§7). The schema therefore adds two hooks:
  - `completeParameters`: every option is made explicit, so a scene bake never picks up a legacy `RISE_FIRE_*`
    environment default;
  - `predictEffectiveParameters`: Configure's effective map, with the window it derives (`frame_origin_s`,
    `end_time_s`) taken from the cache's last bake when the scene does not state it.
- Text options are keyed by their exact text, as the environment variable was. `floor_emissivity 1` and
  `floor_emissivity 1.0` are different keys.

## 3. Scene inputs (`Simulation/SceneInputs.{h,cpp}`)

`SceneObjectInputProvider::Capture(names, upstreamKeys)` returns a detached `ISceneInputs`. Each named object, and
each of its ancestors, becomes a **pose holder**: `Object::ClonePoseHolder`, owned and never shared. The provider
also keeps a counted reference to the scene's animator. The named object and every ancestor must be exact built-in
`Object` instances. CSG composites and custom subclasses are refused before any pose holder is cloned: this
nonvirtual clone cannot preserve their overridden setters, finalization or geometry behavior. CSG operands
are also refused: their stored pose is composite-local, and a shared operand can have multiple world placements.
After `Capture` returns, nothing reads the live scene again.

**SampleTransform(name, t)** works on private clones:

1. Each holder in the chain is cloned again.
2. The clone is posed with the read-only `IAnimator::EvaluateElementAtTimeInto` (DL-465) when the element is
   animated.
3. The chain is composed parent first (`FinalizeTransformations(parentWorld)`).
4. The result is the rigid part of M(t): R from the polar decomposition of the linear part A = R·S, and the
   translation as is.

A stretch S (a scale) that differs from its value at t = 0 is refused. A scene input moves rigidly only.

**Sample(name, t)** returns `mesh.v1` in **world space**: the object's geometry transformed by M(t).

- The geometry is tessellated in object space at Capture: `IGeometry::TessellateToMesh` with detail 24 for
  parametric shapes; meshes are used as they are.
- For a `simulated_mesh_geometry`, the positions are the frame its binding shows at t. They are read from the
  producer's cache with the consumer's retiming. If that cache does not hold the producer's current key, Sample
  refuses.
- World space at t is what cloth's mesh collider wants: its rest positions are world space at t = 0, moved by
  T(t)·T(0)⁻¹ of the track.

**Identity(name)** = SHA-256 of canonical CBOR:

```
["rise.simulation.scene_input.object.v1", geometry, motion]
geometry = ["mesh", 24, SHA-256 of the object-space mesh.v1 payload]
         | ["simulation", producer's expected key, output, time_offset, time_scale, end_policy]   (bound by source)
         | ["cache", cache key, generation digest, output, time_offset, time_scale, end_policy]    (bound by a path)
motion   = [[t, M(t) as 16 float64] for t in T]
T        = {0} ∪ every keyframe time of the object and its ancestors (active animation)
               ∪ {a + f·(b − a) : consecutive keyframe times a < b, f ∈ {¼, ½, ¾}}
```

The quarter points make a changed interpolator change the identity. Material, shader, the object's name and
everything else are not part of it.

- Keyframes outside the simulated span still count. This over-invalidates, but never under-invalidates.
- `Animator::CollectElementKeyframeTimes` (new, non-virtual) supplies the per-element keyframe times.

**Hooks into cloth.**

- `collider_object X` becomes `{type: mesh, scene_input: X, motion: {type: scene_transform, input: X}}`.
- `pin … follow X` becomes `motion {type: scene_transform, input: X}`.

Cloth already samples both through `ISceneInputs`, and its motion condition becomes `scene-transform` (preview).

**Thread safety.** The solver samples from its own threads. Holders are private. The animator's timelines are only
read: `Timeline::EvaluateInto` is const and posing the shared scene does not write them. The in-process path never
writes a scene object. The captured `IAnimator`, the holders and their geometries are counted references.

## 4. Graph, keys and states (`Simulation/SceneSimulation.{h,cpp}`)

**Planning.** `SceneSimulationGraph::Plan(specs, provider)` works in four steps:

1. **Dependencies.** An input whose geometry is a `simulated_mesh_geometry` bound by `source` depends on that
   producer.
2. **Order.** It orders the simulations depth first, upstream first, and refuses a cycle, naming it.
3. **Capture.** It captures each simulation's inputs with its upstreams' expected keys.
4. **Configure.** It configures each simulation: `Configure` + `PrepareCacheDescription`, or the schema's prediction
   for fire.

**Key.**

```
MakeCacheKey(solver, version, effective parameters, [(input, Identity(input))...])
```

This is the contract's key, computed exactly as every host computes it. The upstream key enters through the
identity. Configure results are memoized by (solver, parameters, input identities, and the baked values a
prediction used), so re-planning an unchanged scene configures nothing (test E).

**States.** `InspectSceneSimulation` compares the expected key with the cache on disk:

| State | Rule |
|---|---|
| missing | no cache |
| baking | a writer holds the cache lock |
| stale | the cache's key differs from the expected key; it still loads and plays |
| partial | current key, not sealed |
| complete | current key, sealed |
| failed | configuration refused, cache corrupt, or the last bake failed and left a partial cache (worker status `failed`) |

A `simulation_cache` is never stale: its key is its cache's.

**Propagation.**

- A change upstream changes the downstream identity, so the downstream key no longer matches its cache: the
  downstream is stale.
- A downstream whose upstream is not complete with its current key cannot sample its inputs. It reports `missing`
  (no cache) or `stale` (a cache baked against other upstream data), with `upstreamOutOfDate` and the reason.
- A fire with no stated window and no bake reports `missing` with "key unknown until the first bake". The worker
  still bakes it.

**Queries.**

- `IJobPriv::DescribeSimulations` (appended at the true end of the IJobPriv tail).
- `Job::PlanSimulations`, `Job::SimulationSpecs`.
- `SceneEditController::GetSimulations` (non-blocking).
- C API `GetSimulationCount` / `GetSimulation` (§6).

**Load behaviour.**

| Cache state | Load |
|---|---|
| stale or partial | loads and plays; marked in the states |
| missing | the consumer is an **unbound** `SimulatedMeshGeometry` (no triangles, hidden; `CreateUnbound`) or an empty homogeneous medium (fire), with a warning naming the bake command |
| missing, consumer requests predictive | the load fails |

**Edits.**

- An edit through `ApplyCstParamEdit` and its siblings updates the state: the next query re-captures identities
  and finds the new key; the memo avoids configuring again when nothing changed.
- An edit never starts a bake (test H).
- An edited producer chunk falls back to a full re-derive. `DropChunkByCategory` and the preflight's `EntityExists`
  know `Simulation`, but the incremental capture and restore machinery does not yet hold a simulation record.

**Cache paths.** A relative `cache` resolves against the scene file's directory. `LoadAsciiSceneViaCst` sets that
directory before the derive (`SetSimulationSceneDirectory`), and the editor's staging Jobs inherit it. A derive
without a scene file (the golden test, string scenes) resolves against the working directory.

## 5. Baking (`Simulation/SceneSimulationWorker.{h,cpp}`)

```
rise-sim bake   <scene.RISEscene> (<simulation> ... | --all) [--force] [options]
rise-sim resume <scene.RISEscene> [<simulation> ...] [options]
rise-sim status <scene.RISEscene>
```

The worker loads the scene privately (no rendering) and plans it. It then bakes the requested simulations, plus
every upstream they need, in dependency order:

- complete and up-to-date simulations are skipped;
- a stale cache is rebaked as a new epoch;
- a partial cache of the current key is resumed;
- a `simulation_cache` is reported, not baked.

The scene is planned again before each bake, because a consumer's inputs read the upstream cache that the previous
bake just wrote. Bakes run one at a time, so there is at most one GPU solver; the backend is printed.

The worker no longer refuses configurations with scene inputs. The old forms are unchanged:

- `bake <solver id>` keeps the empty scene;
- `resume <cache dir>` of a configuration with inputs says to resume from its scene.

New worker entry points: `WorkerBakeConfiguration`, `WorkerResumeConfiguration`, `ReadWorkerJobRecord`,
`WorkerOptions` (public, plus `--all`). `rise_sim.cpp` now calls `RegisterBuiltinSolvers`, which also registers the
schemas.

**Capability.** `Capabilities::backend` is a new additive field. Its default `""` means "not stated". Cloth sets
`"cpu"` and fire `"metal"`. It is recorded in contract Appendix C.

## 6. Live from the scene

`SceneEditController::LiveSimulationAttachSimulation(simulation)` (C API: same name) runs in two parts.

**Under the scene mutex:**

- finds the producer and the consumer geometry bound to it by `source`;
- captures the scene inputs detached (through `Plan` when an input depends on another simulation);
- encodes the declared parameters;
- builds a fingerprint (the parameter digest plus the input identities).

**Off every scene lock:** `LiveSimulation::CreateForSimulation` configures, starts and publishes the first frame,
with the captured inputs. They outlive every solver configured with them: `Run::sceneInputs` and
`LiveSimulation::m_inputs`.

**No pre-bake.** An unbound consumer takes its topology from the first published frame:

- `SimulatedMeshGeometry::RebindSource` on a mesh that was never built runs `BuildTopology`;
- `StartRun` skips the topology check when the geometry has no source.

The live key equals the scene's expected key (test).

**Detach and teardown.**

- Detach rebinds the consumer's original source. For an unbound consumer that means `UnbindSource`: hidden again,
  topology kept, so a later attach of the same topology shows it.
- Teardown (`ShutdownSimulation_`) unbinds too.

**Edits.** `LiveSimulationResync()` (C API) restarts every live scene simulation whose fingerprint or consumer
geometry changed, against the current scene. That gives a new key.

- After a full re-derive the old geometry has left the scene; the new consumer is bound and the old run retired.
- `ApplyAgentParamEdit` calls it.
- A GUI calls it after its own edits; this is M5's wiring.

The existing API-only `LiveSimulationAttach(geometry, solverId, parameters)` is unchanged.

## 7. Fire: edits inside `Fire/`, line by line

| File | Change | Behaviour |
|---|---|---|
| `FireSolver.h` | + `struct FireSolverOptionInfo` and `FireSolverOptionInfos()` declaration (after `FireSolverEnvironmentOptions`) | additive |
| `FireSolver.cpp` | + `FireSolverOptionInfos()`: one loop over the existing `Options()` table, copying parameter, environment, kind, default, max level, validator | additive, reads only |
| `FireSolver.cpp` (Configure) | + `out.capabilities.backend="metal";` after `out.capabilities.modes=...` | capabilities are not keyed, so key, payloads and manifest are unchanged |
| `FireParameterSchema.{h,cpp}` | new files | — |

No fire test was run, and no fire solver was configured or stepped. Fire's Configure runs Metal kernels in its
capability check (`QueryFireProductionComputeCapability`) and writes the process environment, so the schema path
never configures it in process.

**Key check against the running M2 bake (read only).** A scene stating the same parameters as the bake's
`params.json` was checked with `rise-sim status`. Its window origin was taken from the bake's job record. The
predicted key, `d8385875bd28…`, equals the key in the bake's `rise-sim.job.cbor`. So fire's key for an existing
parameter set is unchanged by the scene path.

## 8. Tests

| Test | Checks | Time |
|---|---|---|
| `SimulationSceneTest` (new) | 466 | ~6 s |
| `SimulationSceneLiveTest` (new) | 20 | ~3 s |

**SimulationSceneTest**, CPU, cloth.

| Section | Checks |
|---|---|
| A: schema vs Configure | as in §2 |
| B: describe | missing, keyed, 6 frames, backend cpu, default cache beside the scene; the consumer is unbound and hidden; no timeline range |
| C: bake equivalence | `rise-sim bake <scene> flag` equals `rise-sim bake cloth.shell <json>` **byte for byte**: manifest, every frame, statics, the same key and epoch. A second scene bake skips; `status` |
| D: load behaviour | stale loads and binds; partial (bake stopped after 2 frames) loads; `resume <scene>` equals the uninterrupted bake byte for byte; a predictive consumer of a missing cache fails; usage errors |
| E: key sensitivity | the key changes with each of the 34 keyed parameters and with material, pins and colliders; `pace_ms` and `threads` leave it unchanged; the memo avoids a second Configure |
| F: scene inputs | below |
| G: chain | below |
| H: edits | `ApplyCstParamEdit("flag", substeps)` makes the simulation stale and writes no cache file; editing back makes it complete again |
| I: legality | unknown simulation, unknown output, `cache` + `source` together, a dotted name, a pin following an undeclared object, an unused `follow_object`; `source flag` + `output surface` binds |
| J: fire_simulation | below |
| K: simulation_cache | below |
| L: editor and agent | below |

**F: scene inputs.**

- The key follows the collider's radius, position, animation and keyframe values.
- It does not change with a camera move, a light, the collider's material, or an unrelated object.
- `SampleTransform` evaluates the keyframes (x = 0.3 at t = 0.5) without posing the live object: its final matrix is
  bit-equal before and after.
- `Sample` returns world-space mesh.v1.
- **A scene-input bake with an animated collider equals the same motion given parametrically.** The collider is the
  same world mesh with `motion translate 0.6 0 0` (linear keyframes at 0.6 m/s). In this test the results are
  **exactly equal**: every payload byte for byte, max position and velocity difference 0. In general equality holds
  to solver tolerance, because the scene track's velocities are central differences over ±1 µs.
- The comparison is not vacuous: the same bake with a static ball differs by 3.3 cm at the last frame.

**G: chain.**

- Unbaked: the downstream is missing, upstream out of date, with no key.
- `bake <scene> lower` bakes `upper` first.
- Both complete. Then the upstream's gravity is edited: both are stale, and the downstream is marked upstream out of
  date.
- `--all` rebakes both, and the downstream key changes.
- A dependency cycle is refused, naming it.

**J: fire_simulation**, with no fire solver created.

- The chunk maps exactly the authored fields, in the solver's types.
- Completion states all 27 parameters with the solver's canonical defaults.
- The expected key equals an independent `MakeCacheKey` over the completed map; 17 frames are predicted.
- An option given its default keys like the option left out. Six different edits each change the key.
- Without a window, the key is "unknown until the first bake".
- Parse-time validation through the solver's own validators and ranges: `advective_cfl 2`, a malformed
  `vreman_zone`, `soot_self_absorption 3`, a 0.4 m pool and an unknown mode are refused.
- A predictive fire with no cache fails.
- Mesh↔grid cross-binding is refused both ways, and so is `source` + `sequence_manifest`.

**K: simulation_cache.**

- A cache baked elsewhere binds and is complete.
- `rise-sim` does not bake it.
- `fire_medium` refuses its mesh output.

**L: editor and agent.**

- The three sample scenes round-trip: load, `SerializeCst`, load again; the same specs (parameters byte for byte,
  inputs, outputs) and the same keys.
- The three producers register under `ChunkCategory::Simulation`. `SchemaGenForChunk` and the agent's `simulation`
  category list them.
- `cloth_simulation`'s descriptor is generated from the schema (every name and default).
- `source` is a `Reference` to `{Simulation}`.

**SimulationSceneLiveTest**, over the real interactive rasterizer.

- A scene whose cloth (interactive, 21 × 21, with an animated `collider_object`) has no cache loads with an unbound
  consumer.
- `LiveSimulationAttachSimulation("flag")`: the consumer takes 441 vertices from the first frame; the live key is
  the scene's expected key.
- It plays while the test scrubs the scene. The solver samples the animated collider throughout without error, and
  at least 20 frames are published.
- `ApplyAgentParamEdit(flag, gravity)` triggers a full re-derive. The resync restarts the run with the edited key on
  the re-derived consumer, and the new run publishes.
- Detach unbinds; attach again; closing while playing works.

**Suites.**

| Suite | Result |
|---|---|
| `SourceHygieneTest` | 706/0. The five build lists were updated; the IJob vtable manifest gained six tail lines: `AddSimulation`, `RemoveSimulation`, `EnumerateSimulationNames`, `AddSimulatedMeshGeometryFromSimulation`, `SetSimulationSceneDirectory`, `AddFireMediumFromSimulation` |
| `SceneEditorSuggestionsTest` | keyword pin 181 → 184 |
| `ConnectionLegalityTest` | 445/0 |
| `RewireConnectionTest` | repeatable-Reference pin 2 → 4, 333/0 |
| `CstDeriveGoldenTest` | 3 new golden lines, nothing else changed |
| fast tier, no `Fire*` | §8.1 |

**`CstDeriveGoldenTest` and missing caches.** The golden derive (no scene file) resolves `caches/<name>` against the
working directory, the repo root, where no cache exists. A bake of a sample scene writes beside the scene
(`scenes/**/caches/`, now gitignored), which the golden never reads. So every machine derives the consumers
unbound, and `DumpJob` prints the same text.

**`DumpJob`.** It gains a `simulations:` section, printed only when a scene declares simulations, so every older
digest is unchanged. The section lists name, keyword, solver, the parameter digest, the cache's leaf, the inputs and
the outputs.

**Golden lines and git.** The golden lines were generated with a temporary git index listing the new scenes. The
real index was not touched. Until the three scenes are tracked, the golden test reports them `STALE`; once they are
tracked, it passes.

### 8.1 Fast tier

The non-fire fast tier was run with `GIT_INDEX_FILE` pointing at a temporary index that adds the three new scenes.
That simulates the committed state for the golden test.

- 420 built, 0 failed; the build is warning-free.
- 419 passed, 1 failed. The failure was `RewireConnectionTest`'s pin on repeatable `Reference` parameters (2 → 4:
  `cloth_simulation.follow_object` and `.collider_object`, on the non-addressable `Simulation` category). After
  re-deriving that pin, the test passes on its own (333/0).

## 9. Limits, and what was not done or verified

**Contract gap.** A one-way coupling fed from a cache through a port (`CachedPortFeed`) is not keyed: the key covers
scene inputs only. M4 binds upstream simulations through scene elements instead, whose identity embeds the upstream
key. Port-fed keys belong with M6 (contract Appendix A.3, item 7).

**Mesh colliders are sampled at t = 0.** Cloth samples a mesh collider once, at t = 0, then moves it rigidly. A
collider that shows a deforming simulation is therefore its frame at scene time 0; use `time_offset` to choose which
frame (the chain sample uses drape's last frame). A scale that changes over time is refused. Since cloth m3a.4 a
collider whose geometry deforms over the run is detected at Configure, stated in the notes and labelled `preview`
(SIMULATION_M3A_RESULTS.md §10.5).

**Cloth convergence (observed, not changed).** In bake mode at 25 × 25 with the default 8 substeps, a cloth sliding
over a laterally moving mesh ball failed Newton ("did not reach its tolerance", 5–12 m/s²). A static ball, or a
vertical motion with 16 substeps, bakes; the chain sample uses those. **Fixed in cloth m3a.4**: the cause was a jump
in the edge–edge barrier energy at near-parallel edges (the UV sphere's rings against the sheet); the lateral scene
now bakes at 8 substeps (SIMULATION_M3A_RESULTS.md §10.1–10.2).

**Fire keys are predicted.**

- A wrong prediction reads as `stale` right after a bake, never as current. One case: someone bakes the same scene
  with a fire solver whose legacy environment differs — a scene bake makes every option explicit, so only a direct
  `bake fire.methane` can differ.
- The interplay rules Configure enforces (`fast_fixed_vcycles` needs `fast`; the compact stencil's dependents) are
  checked at bake time only.
- The tier must be 6, 8 or 10. The schema admits 6–10 and Configure refuses 7 and 9.

**Not done.**

- No `end_policy hide` for meshes. There is no consumer retiming for `fire_medium`.
- The fidelity spellings `fidelity` (mesh) and `fidelity_mode` (fire) were not unified with aliases.
- No incremental re-derive of a producer chunk.
- `transform_source` is specified (§1), not built.

**An unbound consumer** has no triangles and is hidden. An emissive one would be area-sampled with zero area; this is
not handled.

**Not verified.**

- The VS2022, Xcode and Android build lists were edited by a script, not built.
- No ThreadSanitizer or AddressSanitizer run of the live-from-scene path.
- No fire bake from a scene, and no `fire_medium` bound by `source` to a real fire cache: no fire solver or Fire*
  test may run during the GPU bake.
- No GUI (M5).
- `LiveSimulationResync` after GUI property edits other than the agent path.
- The edit-staging derive logs a "no cache yet" warning when its staging Job resolves differently; staging Jobs now
  inherit the scene directory, so this should not recur, but it was not re-checked.
