# M7 prerequisites: results

2026-11-25. Design: [SIMULATION_M7_DESIGN.md](SIMULATION_M7_DESIGN.md) §6 (the method-independent prerequisites of
the liquid solver). Contract: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (Appendix C, four
2026-11-25 M7 rows). Delivered in the order points codec, changing topology, points geometry, streamed checkpoints,
owned implicit exchange; every stage builds warning-free and passes its tests.

## 1. `points.v1`

| | |
|---|---|
| Code | `src/Library/Simulation/SimulationPoints.{h,cpp}`: `PointsFrame`, `EncodePoints`, `DecodePoints`, `PeekPoints`, `PointsDomain` |
| Spec | [SIMULATION_POINTS_FORMAT.md](SIMULATION_POINTS_FORMAT.md) |
| Test | `tests/SimulationPointsTest.cpp` (< 1 s) |

- One particle set at one time: positions; optional velocities (with units), a uniform **or** per-point radius,
  u64 ids (strictly increasing), named per-point binary32 attributes (1–16 components, units; `position`,
  `velocity`, `radius`, `id` reserved). Empty sets are valid.
- Canonical (flags implied by the arrays), little endian, no trailing bytes; finite samples, radius sign bit clear.
  Decode bounds every count by the bytes remaining before allocating and charges the caller's budget for the
  retained input, the previous output and the candidate; failure leaves the output untouched. Bulk arrays are
  copied with `memcpy` on little-endian hosts (byte loop elsewhere).
- Refinements of the contract's sketch (A.2, never implemented): typed optional records instead of a channel list,
  the uniform radius, velocity units as in `mesh.v1`, u64 ids (a million particles a frame cannot exhaust them).
  A static payload for a fixed id set is not provided.
- `PointsDomain`: SHA-256 of the id words (else of the position words) — the `field.v1` domain of per-point data.
- `IsNativePayloadType("points.v1")`: the disk cache and the in-memory publisher accept and serve it.
- Test: two golden fixtures written with Python `struct`/`hashlib` (both directions, decode∘encode identity, domain
  digests), 20 encode refusals (ids, radii, counts, units, names, NFC, budget), every truncation, trailing bytes,
  version, unknown flag, both radius flags, hostile count, ids/radius/components refused on decode, output
  untouched, a 100k-point round trip and budget, cache and memory publisher round trips.

## 2. Changing-topology and empty meshes in `simulated_mesh_geometry`

| | |
|---|---|
| Source | `SimulatedMeshSource.{h,cpp}`: no refusal of a different digest or of an empty bind frame; a frame whose digest is not the bind frame's carries its connectivity and UV set (`MeshFrameSample::topology/ownTopology/indices/uvs`); keys without velocities only from neighbours of the same digest; predictive admission no longer requires one topology. `MapCacheFrameTime`, `SameCacheBinding` factored out (shared with points) |
| Geometry | `SimulatedMeshGeometry.{h,cpp}`: per install, the frame's digest against the installed one: equal → the M3b lean refit (unchanged); different → `ReplaceTopologyLean` (arrays reused in place, pointer triangles rebuilt, BVH rebuilt in full on the thread pool, areas/CDF recomputed, uncertified). Rollback restores the previous topology. `RebindSource` accepts another topology (the next prepare rebuilds). Empty frames: no hits, area 0, finite bounds, safe `UniformRandomPoint` |
| Indexed mesh | `TriangleMeshGeometryIndexed::ReplaceTopologyLean`; recycled mailbox ids (`MailboxTableEntries/Bytes` diagnostics) |
| BVH | `BVH.h`: `BuildOptions { parallel, quiet }`, `Rebuild(prims, options)` (reuses the tree's storage), opt-in parallel build |
| Tests | `tests/SimulatedMeshTopologyTest.cpp` (T, K, R, P, J, V, M; ~3 s; `--bench [--extended]`), two rows in `SimulatedMeshBlurTest` (t, x), `SimulatedMeshGeometryTest` F updated |

**Parallel BVH build.** The builder was serial and recursive. A node's descendants occupy one contiguous run whose
layout depends only on that subtree, so the top levels are split serially (the same `SplitNode` the serial build
uses), the subtrees (≥ 16k primitives, up to 4 × workers of them) are built as local trees on `GlobalThreadPool`
in one `ParallelFor`, and spliced in the serial order with offset child indices. Per-primitive boxes are computed
in parallel chunks (min/max reductions are exact in any order). **The tree is bit-identical to the serial one**
(test P: the serialized mesh, BVH nodes and primitive order included, at 1k and 300k triangles; an in-place
rebuild equals a fresh build; hits equal the classic `DoneIndexedTriangles` build). Static meshes keep the serial
build (opt-in only). Refactoring the serial path into `SplitNode` + recursion made it about 10 % faster (bins are
freed before recursing): 39.2 → 35.4 ms on a 100k-triangle mesh.

**Per-frame rebuild cost** (`SimulatedMeshTopologyTest --bench --extended`, triangle soup half grid, 15 pool
workers, min of 3, machine load ≈ 3.4):

| Triangles | full rebuild, serial | full rebuild, parallel | lean refit (fixed topology) | RSS |
|---|---|---|---|---|
| 100k | 38 ms | 17 ms (×2.2) | 3 ms | 54 MB |
| 1M | 445 ms | 149 ms (×3.0) | 55 ms | 485 MB |
| 5M | 2,555 ms | 947 ms (×2.7) | 337 ms | 2.4 GB |

The parallel rebuild still runs the pointer-triangle pass, the float filter, the BVH4 collapse and the area CDF
serially; those are the remaining third.

**Mailbox table.** Each thread kept one entry (and a stamp per triangle) for every geometry id ever created; ids were
monotonic and never freed. Ids are now recycled (smallest free id; a min-heap under a mutex at construction and
destruction only), and a never-reused 64-bit epoch per geometry resets a recycled id's stale entry at the first ray
of that geometry on a thread (`BeginMailboxRay`, out of line on the reset). The per-element lookup is unchanged.

| 1,000 frames | before | after |
|---|---|---|
| 1,000 meshes of 20,000 triangles created, intersected, destroyed (scratch bench) | RSS 8.4 → 109.5 MB (+101 MB) | 22.1 → 22.3 MB |
| changing-topology playback in place, 1,000 installs (1,000 rebuilds) (test M) | — (refused before) | RSS 496.8 → 496.8 MB; table 1,202,008 → 1,202,008 bytes |
| 1,000 meshes of 5,000 triangles (test M gate) | — | table entries 3 → 3 |

**Static meshes unchanged.** 100,352-triangle static mesh, 400k rays, one thread, median of 7, binaries built
before and after (the six files reverted to HEAD in a scratch build), alternated: closest hit 117.3–118.8 ns/ray
before vs 116.2–119.9 after (equal); any hit 77.6–80.8 before vs 78.7–81.5 after (+1.5 % on average). The any-hit
difference stays when the only new instruction on that path (the epoch compare) is removed, so it is code placement,
within the ±15 % run-to-run noise M3b reported; checksums and hit counts identical.

**Tests.**
- T: seven frames (a sphere, refined; split into two; empty; two spheres; the same topology moved; one sphere) each
  installed at its time hit **bit-equal** to a static indexed mesh of that frame (hits, any-hit, distances, normals,
  UVs); the area equals the full build's to the last place (1 ulp: fast-math sum); refit exactly where the digest is
  unchanged (5 rebuilds, 1 refit); an empty bind frame binds and grows.
- K: keys from a changed-topology frame's own velocities: at the shutter ends bit-equal distances to static meshes
  at the keys; without velocities no keys across a topology change, keys from a same-topology neighbour.
- R: an undo across a topology change restores the previous topology exactly; rebind to another topology.
- J: a prepared animation renders every frame of an emissive changing-topology mesh, the empty one included.
- V: the interactive viewport scrubs every frame (`InstallSimulatedMeshesAtTime`), installs its topology and renders.
- `SimulatedMeshBlurTest` row **t** (deformation blur of a changing-topology frame vs the time-average of 8×3
  statics, PT): 0.874451 ± 0.000592 vs 0.875285 ± 0.000063, t = −1.40 (crit 4.89); row **x** (emissive, the bind
  frame a 5×5 sheet of a quarter of the area, the installed frame 9×9): 0.185329 ± 0.000171 vs an indexed mesh
  0.185121 ± 0.000138, t = +0.95 (crit 3.92). Rows d, r, e give the M3b numbers unchanged (critical values move with
  the Bonferroni count, 4 → 6 comparisons).
- `SimulatedMeshGeometryTest` F used to assert that a different topology fails the frame; it now asserts the rebuild
  and the rebuild back (deliberate behaviour change).

**Emissive changing-topology meshes.** Supported without blur: the production path forces a light-sampler rebuild
before the prepare and rebuilds at `AttachScene`, the area CDF is recomputed on every rebuild (row x); the viewport
invalidates the light samplers each pass. Under deformation blur emission is sampled at the nominal pose (the M3b
limit, unchanged). An empty frame has no area and contributes no light.

## 3. `simulated_points_geometry`

| | |
|---|---|
| Source | `Simulation/SimulatedPointsSource.{h,cpp}`: `PointsCacheSource` (disk caches): time map (`MapCacheFrameTime`), end policies, follow mode for partial caches, predictive admission, radii (frame per-point, frame uniform, `radius` default; × `radius_scale`), the colour attribute, keys from velocities or from neighbours matched by id |
| Geometry | `Geometry/SimulatedPointsGeometry.{h,cpp}`: `BVH<uint32_t>` over the points, rebuilt in full (parallel) at every install; per-point test = `SphereGeometry`'s in the sphere's frame; blur keys and the shared sample-time mechanism; freeze, rollback (swap, no copy), missing-frame hold/hide, unbound |
| Chunk | `simulated_points_geometry` (`cache` or `source <simulation>.<output>`, output, time_offset, time_scale, end_policy, missing_frame, fidelity, motion_blur, radius, radius_scale, color_attribute); `IJob::AddSimulatedPointsGeometry` (tail append) |
| Seams | `Job::ActiveTimeVaryingAssets`, `DescribeTimeVaryingAssets` (`TimelineRange::SimulatedPoints = 2`), `SceneBindsSimulatedMesh` (now any simulated asset), `InstallSimulatedMeshesAtTime` (viewport scrub/playback) |
| Scene | `scenes/Tests/Simulation/simulated_points_spray.RISEscene` (`simulation_cache spray`, caches/spray beside it; `SimulatedPointsGeometryTest --write-sample-cache <dir>` writes a 2,000-particle fountain) |
| Test | `tests/SimulatedPointsGeometryTest.cpp` (A–F, V; ~1 s; `--bench [--extended]`) |

- **Static equivalence.** A non-blurred frame against explicit `SphereGeometry` spheres (rays moved into each
  sphere's frame): ranges and points bit-equal, normals/UVs/derivatives to 1e-13 (fast-math contraction differs
  between translation units), any-hit identical, at two frames. A scene with the chunk renders **pixel-identical**
  to the same spheres as 40 `sphere_geometry` objects (0 of 1,024 pixels differ).
- **Blur.** Keys from velocities: at the shutter ends hit records bit-equal to spheres at the keys, mid-shutter
  likewise; from ids (no velocities, churned sets): within 1e-7; without ids none. Blur row F (PT, pairs of points
  meeting at the frame time): blurred 0.924775 ± 0.001217 vs time-average 0.925039 ± 0.000306, t = −0.21; every
  blurred frame took the time-indexed path. Red-proof: the shutter fraction frozen at 0.5 fails it (t = −6.0).
- **Policies.** Hold/hide of a partial cache (with undo), predictive refusals and admission, end policy error,
  an empty frame (no hits, no area, finite bounds) with an undo back, freeze.
- **Attributes to shading.** One attribute (1 or 3 components) as the hit's vertex colour (`color_attribute`,
  read by `vertex_color_painter`): cheap (3 floats per point). Other attributes are **not** exposed: the hit record
  has no named per-hit channel, and adding one is renderer work beyond this pass.
- **Not supported.** Live (in-process) points bindings and rebinding after a bake (reload the scene); points as area
  lights (`CanBeAreaLight` false); a `source` producer with no cache gives empty points until the scene is reloaded.

**Performance** (`--bench --extended`, 15 workers): install = read + SHA-256 + decode + radii + full BVH build.

| Points | bind | per-frame install | with shutter keys |
|---|---|---|---|
| 100k | 31 ms | 22 ms | 22 ms |
| 1M | 241 ms | 157 ms | 162 ms |
| 4M | 982 ms | 643 ms | 664 ms (peak RSS 1.5 GB) |

Intersection, 200k primary rays, closest hit, against the same spheres as separate `sphere_geometry` objects under
the scene's TLAS: 1,000 spheres 188 vs 279 ns/ray; 10,000 spheres 307 vs 591 ns/ray (one geometry's BVH beats one
TLAS leaf per sphere).

## 4. Streamed checkpoints

| | |
|---|---|
| Contract | `ICheckpointSink` / `ICheckpointSource`, `MemoryCheckpointSink` / `MemoryCheckpointSource`, `ISimulationSolver::CheckpointTo` / `RestoreFrom` (defaults: the in-memory methods), `ISimulationGroupHost::CheckpointTo` / `RestoreFrom` |
| Worker | `rise-sim.checkpoint` version 2 (`WriteStreamedCheckpoint` / `ReadStreamedCheckpoint`): header, stream, trailer (length, SHA-256, end magic); `DurableSidecarWriter` (stage, fsync, rename, fsync directory) and `SidecarReader`; resume verifies the digest in a first pass, then re-hashes what the solver reads; version 1 still resumes |
| In process | `InProcessHost::SaveCheckpointTo` / `RestoreCheckpointFrom` (any sink; `MemoryCheckpointSink` in memory) |
| Group | `SimulationGroup::CheckpointTo`: u64 index length, index (format, identity, time, steps, member names and stream lengths, offers, statics), member streams in step order; lengths from a counting pass (each member serialises twice, nothing held). The scene group worker writes it as `rise-sim.group.checkpoint` version 2 and still resumes the in-memory format |
| Solvers | Reference solvers (`test.advection_diffusion`, `test.membrane`) stream their own layout (`RISEREFS`, header, binary64 fields in chunks). Cloth and rigid use the default (their states are small); fire stays on the in-memory path (no fire file edited) |
| Tests | `SimulationWorkerTest` (`StreamedCheckpoints`: version-2 format, corrupted and truncated files refused, version-1 resume, both reference solvers' in-memory and streamed resumes identical to the uninterrupted run, a truncated stream refused; the existing stop/resume runs now go through the stream); `SimulationGroupTest` (the group stream resumes byte-identically; truncation refused); `--checkpoint-memory N` (manual, the 1 GB measurement) |

**Memory, 1 GB state** (`SimulationWorkerTest --checkpoint-memory 1024`, peak RSS of the worker process):

| | no checkpoint | writing a checkpoint | resuming from it |
|---|---|---|---|
| solver on the default (in-memory `Checkpoint`) | 1,026.6 MB | 2,050.6 MB | 2,051.7 MB |
| solver streaming (`CheckpointTo` / `RestoreFrom` in 8 MiB chunks) | 1,026.7 MB | **1,026.6 MB** | **1,027.7 MB** |

The worker adds nothing: the stream goes straight to the staged file, hashed as it passes; the restore reads
1 MiB chunks to verify, then feeds the solver.

## 5. Rigid mass properties over a port; owned implicit exchange

| | |
|---|---|
| Rigid | `rigid.bodies` offers `body_mass_properties` (`field.v1` over the body set: `center_of_mass` [m] ×3, zero — the body frame's origin is the centre of mass; `inertia` [kg m²] ×9, body (principal) frame, diagonal; `mass` [kg] ×1), offered once at the start time and held by every reader; `body_load` unchanged |
| Contract | `Capabilities::ownedImplicitPorts`; `GroupPolicy::ownedExchanges { owner, partners }` |
| Group | owned links (every connection between the owner and a partner) are exchanged once per macro step, Gauss-Seidel with the owner first, never sub-iterated; refused under Jacobi, with the owner after a partner, with unknown partners, and beside other sub-iterating members. Label: implicit when every owned port is declared and the owner (in the closure) is predictive, whose evidence then stands in for the coupling's (condition `coupling: owned-implicit`); undeclared → preview with the reason. The policy member enters the group identity only when used |
| Test solver | `tests/SimulationAddedMassSolver.h` (`test.added_mass`): a 1-D liquid adding inertia and buoyancy to a `rigid.bodies` sphere; `staggered` (lagged acceleration) or `owner` (reads the mass over the port, solves the joint system by Crank–Nicolson per macro step, offers the step-average hydrodynamic force — which, held by the rigid solver, reproduces the joint step exactly) |
| Test | `tests/SimulationOwnedCouplingTest.cpp` (< 1 s) |

Results (exact solution of (m + mₐ) y″ = −k y − m g, m = 1 kg, k = 10 N/m, 2 s):

| Scheme | mₐ / m | H | max displacement error |
|---|---|---|---|
| staggered | 0.5 | 0.02 / 0.01 | 3.24e-2 / 1.60e-2 (order 1.02) |
| staggered | 2 | 0.02 | 2.1e26 (divergent) |
| staggered | 2 | 0.005 | ∞ (nonfinite; refining does not help) |
| owned implicit | 2 | 0.04 / 0.02 / 0.01 | 8.74e-4 / 2.19e-4 / 5.46e-5 (order 2.00, 2.00) |
| owned implicit | 10 | 0.04 / 0.02 / 0.01 | 2.36e-4 / 5.89e-5 / 1.47e-5 (order 2.00, 2.00) |

Labels: the owned group is predictive for both members; an owner that does not declare its ports, and the staggered
scheme, are preview with the reason.

## 6. Pins and checklist items

- `IJobVtableManifest.txt`: one tail line (`AddSimulatedPointsGeometry`), regenerated with
  `RISE_REGEN_IJOB_VTABLE_MANIFEST=1` (only that line added).
- `SceneEditorSuggestionsTest`: keyword count 189 → 190 (189 unique descriptors).
- `CstDeriveGoldenTest`: one hand-added line for `simulated_points_spray.RISEscene` (from the verify run's derive,
  1406 bytes, 32 lines; the generator was not run, so master's two UNCOVERED scenes stay out). Until the scene is
  committed (`git ls-files`), the test reports that line as **STALE**; it matches (`MATCH`) once tracked. The derive
  does not depend on a cache: without the scene directory, `caches/spray` resolves against the repository root.
- Build lists: `SimulationPoints`, `SimulatedPointsSource`, `SimulatedPointsGeometry` (.cpp/.h) in make, Android
  cmake, VS2022 vcxproj and filters, and the three Xcode Sources phases; `SourceHygieneTest` passes.
- The `simulated_mesh_geometry` chunk description now says the topology may change.

**Fast tier** (`./run_all_tests.sh --tier fast --timeout 300 --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, 443 tests):
423 pass. The 20 failures are the ones the merged master already had (19 BDPT/emitter tests and
`CstDeriveGoldenTest`'s two UNCOVERED master scenes); `CstDeriveGoldenTest` also reports the expected STALE line
above. Every new or touched simulation, mesh, BVH, hygiene and suggestion test passes. The new fast-tier tests
take about 6 s together. Fire tests were not run.

## 7. Limits

- Changing topology: blur keys only from the current frame's velocities or same-topology neighbours (no vertex
  correspondence across a change); emission under blur sampled at the nominal pose; the mesh is uncertified (as in
  M3b); the per-frame rebuild's serial passes (filter, BVH4, areas) bound the speed-up near ×3.
- A live in-process changing-topology run: `LiveSimulation` (the cloth live session) still refuses a restart whose
  bind topology differs (its existing behaviour, kept); the geometry itself accepts it.
- Streamed checkpoints: the group's counting pass serialises each member twice; the worker applies no budget to a
  version-2 stream (it is its own, digest-verified file); Windows uses a buffered stdio writer without fsync.
- Owned exchange: one owner per partner set, Gauss-Seidel only, no sub-iteration of other members beside it; the
  rigid body still applies the load held over the step (the owner must offer the step-average force for the joint
  scheme to be reproduced exactly; `field.v1` carries it in binary32).
- Not verified: the VS2022, Android and Xcode builds (edited by script; the make build compiles everything);
  Windows paths of the streamed sidecar writer.

## 8. What the liquid solver can now rely on

- [x] Publish spray and marker particles as `points.v1` (`PointsFrame`, ids for correspondence, per-point radius,
      attributes with units); caches and live publishers accept it; `simulated_points_geometry` renders it with blur.
- [x] Publish its surface as `mesh.v2` with a different connectivity (inline indices) every frame, including empty
      frames, with per-vertex velocities of the current frame and UVs; `simulated_mesh_geometry` rebuilds per frame
      (≈150 ms at 1M triangles), blurs, refreshes emissive light tables, scrubs in the viewport, and does not leak.
- [x] Checkpoint a multi-gigabyte state by overriding `CheckpointTo` / `RestoreFrom` (the worker and the group
      worker hold it once, verify a SHA-256, and stay durable); the in-memory path remains for small states.
- [x] Read each rigid body's mass, body-frame inertia and centre of mass from `rigid.bodies.body_mass_properties`,
      its state from `body_state`, and return the hydrodynamic force and torque on `body_load`.
- [x] Declare `ownedImplicitPorts` and be listed in `GroupPolicy::ownedExchanges`: the group then exchanges once per
      macro step and grants the implicit-coupling label when the liquid's own evidence (which must validate the
      joint solve) is predictive.
- [ ] Not provided: a scene chunk for the liquid, live points bindings, named per-point attributes in shading beyond
      one colour, sub-iterating other members beside an owned exchange.
