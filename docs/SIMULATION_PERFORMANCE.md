# Simulation performance: measurements and tuning opportunities (first half; second half: cloth §13, liquid §14)

2026-11-27. Branch `simulation-liquid` at `05eab7170` plus the working-tree changes listed in §8 (first half);
the cloth part of the second half (at `052fc4338`, thin LTO) is §13. Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (no contract change). Fire CPU timings of
earlier rounds: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) §11.6 and §12.3.

**Scope.** Measure every committed simulation scene class, attribute time to phases, rank the tuning opportunities
(§6), and implement the byte-preserving ones outside the liquid solver, fire and tests of both (§8). The liquid
section (§5) and the fire section (§7) are profile and write-up only: another agent is rewriting parts of the liquid
solver, and fire version 3 was just baselined.

**Outcome in one paragraph.** The simulation engine is not slow where one expects. Cloth, rigid, group exchange and
the cache are each under a few percent of a bake except in two places: the predictive cloth-over-moving-collider
step (1.4 s per frame for a 15 x 15 sheet, 98 % of the trampoline bake, one core of sixteen) and **plan-time
evidence**, which `Plan`/`Configure` recompute in every new process: 0.7 s for the cloth flag, 2.8 to 4.9 s for the
liquid scenes and 3.6 s for the trampoline, before a single step runs. That cost is paid by `rise-sim status`, by
every bake, and by every scene load in the renderer and the app. Beyond that: the fork-join pool shared by cloth and
liquid burned up to 37 s of system time in a 4 s run (spin with `sched_yield`; fixed here), the cache writer's cost
per frame grows with the number of frames already written (`IsUTF8NFC` on every manifest string; fixed here), and
SHA-256 ran at 430 MB/s in scalar code (now 2.9 GB/s on the ARM SHA-2 instructions; fixed here). All of those fixes
leave every output byte unchanged (frame sets compared, suite green apart from one liquid test that belongs to the
concurrent liquid work). Measured effect: trampoline bake 47.7 to 39.9 s and 23 % fewer instructions, trampoline plan
3.66 to 2.36 s, cache writer 4.4 times faster at 2000 frames, playback of a liquid frame 2.6 to 0.58 ms, system CPU of
the liquid bakes down 17 to 28 times. Fire on the CPU backend spends about half of its busy CPU in the FNV content
hashes even in fast mode (§7).

## 1. Machine, build, method

| | |
|---|---|
| Machine | Apple M4 Max, 16 cores (12 performance + 4 efficiency), 64 GiB, macOS 27.2.1 (Darwin 27.2.0), Apple clang 21.0.0 |
| Build used | make build (`build/make/rise`, Config.OSX): `-O3 -flto -ffast-math -fno-finite-math-only -funroll-loops -std=gnu++17`, default target `apple-m1` (NEON, SHA-2, no SVE); `-DRISE_ENABLE_MAILBOXING -DRISE_ENABLE_OIDN -DRISE_ENABLE_OPENPGL -DRISE_ENABLE_OPENVDB` |
| Binaries | `bin/tools/rise_sim` (the `rise-sim` bake worker) and the test programs; the "before" binary is a copy of the build at the start of the session (`rise_sim.base`) |
| Tools | `/usr/bin/time -l` (wall, user, sys, peak RSS, instructions retired), `sample` at 1 ms (no xctrace, no Instruments), hand-written drivers linked against `librise.a` for the cache writer and the playback source |
| Scratch | `/Volumes/SSD/claude-tmp/perf-scratch/` (bakes, samples, drivers); `RISE_SIMULATION_CACHE_DIR` under it, `RISE_SIMULATION_TRUSTED_ROOTS=/Volumes/SSD` |

**The machine was shared the whole time** (other agents building and running tests; load average between 3 and 30,
noted with each result). Wall times are therefore noisy and are given with that caveat; wherever a comparison mattered
it was repeated, and instructions retired (load independent) or output byte identity were used as well.
`sample` shares are shares of the sampled thread's samples (the main or step thread; pool workers are idle while it
waits for them, so a fork-join phase shows up as the caller's time). Inclusive shares of nested functions overlap.

**Byte identity.** A frame file is named by the SHA-256 of its contents, so two bakes wrote identical bytes exactly
when their `*.frame` file name sets are equal; that is what "frame sets identical" below means. The test suite pins
the golden hashes in addition.

## 2. Measurements per scene class (before any change)

`rise-sim bake <scene> --all --stop-after-frames N` on the committed scenes in `scenes/Tests/Simulation/`.
"Startup" is `rise-sim status` of the same scene (Plan: parse, Configure of every solver, evidence, group
coupling evidence), which every bake pays first. For coupled groups `N` counts macro steps (0.0083 s for the
liquid-cloth scenes), not output frames. "Busy cores" is (user + sys) / wall; the useful part is user / wall.

| Scene class (scene) | Window | Wall | User | Sys | Busy cores (user only) | Peak RSS | Cache written | Startup (status) | Approx. per frame after startup | Load avg |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| Cloth flag, 31 x 31, interactive (`cloth_flag_scene`) | 30 of 91 frames | 1.24 s | 1.15 s | 0.12 s | 1.0 (0.93) | 189 MB | 1.2 MiB (30 frames, about 40 KB each) | 0.69 to 0.74 s | about 17 ms (real time is 33 ms) | 3.8 |
| Rigid ball on cloth, coupled group, predictive (`rigid_cloth_trampoline`) | all 31 frames | 47.7 s | 47.6 s | 0.2 s | 1.0 (1.0) | 118 MB | 564 KiB (62 frame files) | 3.6 s | **1.4 s** | 4.0 |
| Liquid dam break, 48^3 (`liquid_dam_break`) | 8 of 46 frames | 3.94 s | 8.35 s | **28.8 s** | 9.4 (2.1) | 132 MB | 8.6 MiB (1.1 MiB per frame) | 2.8 s | 0.1 to 0.15 s | 2.8 |
| Liquid + rigid bodies, 64^3, group (`liquid_rigid_bodies`) | 10 of 61 frames | 12.2 s | 34.6 s | **80.2 s** | 9.4 (2.8) | 263 MB | 32 MiB (33 files) | 4.4 s | about 0.8 s | 2.6 |
| Liquid + cloth, catch, 64^3, group (`liquid_cloth_catch`) | 6 macro steps | 5.06 s | 10.8 s | 25.6 s | 7.2 (2.1) | 339 MB | 21 MiB | 3.3 s | about 0.3 s per macro step | 4.7 |
| Liquid + cloth, wave, 64^3, group (`liquid_cloth_wave`) | 6 macro steps | 4.33 s | 8.0 s | 22.8 s | 7.1 (1.8) | 183 MB | 11 MiB | 3.3 s | about 0.17 s per macro step | 4.4 |
| Honey and water, two liquids, 48^3 (`liquid_honey_water`) | 6 of 61 frames | 6.95 s | 12.4 s | 36.9 s | 7.1 (1.8) | 130 MB | 5.9 MiB | 4.9 s | about 0.34 s | 4.2 |
| Fire campfire, CPU backend (`backend cpu`), tier 6, fast mode | about 14 steps (stopped by SIGINT) | 69 s | 272 s | 7.7 s | 4.1 (3.9) | **2.97 GB** | none yet (first frame is at 1 s of fire time) | n/a | about 4.5 s per accepted step | 4 to 6 |

Notes. (1) The per-frame column subtracts the separately measured startup and is approximate (the two were measured
minutes apart on a loaded machine). (2) Solver sub-steps per frame are not reported by `rise-sim`; the figures are per
frame (or macro step). (3) Fire Metal was **not** run: a CPU step of the same case at tier 6 already shows the phase
structure, and the GPU must stay free for other work; the earlier Metal numbers are in the M8 documents. (4) The
liquid wall times of this machine state are inflated by the pool's system time (§4.1): sys is 2 to 4 times user. (5)
Frame size is the sum of the cache files over the frames written, including the static topology records.

### 2.1 Thread scaling of the liquid solver (dam break, 12 frames, `threads N` in a scratch copy of the scene)

| `threads` | 1 | 2 | 4 | 8 | 12 | 16 |
|---|---:|---:|---:|---:|---:|---:|
| Wall, old pool | 5.67 s | 4.56 s | 4.30 s | 4.24 s | 4.75 s | 4.86 s |
| User / sys CPU, old pool | 5.4 / 0.2 s | 7.0 / 0.6 s | 10.6 / 1.8 s | 16.4 / 5.5 s | 10.0 / 25.7 s | 9.9 / 27.7 s |

Eight to sixteen threads buy nothing over four on this scene, because about 2.8 s of the 4.2 s are the serial
plan-time evidence (§5.1); beyond eight the pool also burns system time (§4.1). The default is `min(16, hardware)`.

### 2.2 Cache write cost (driver `cache_bench`: `CacheWriter::Append` of n frames of b random bytes, preview cache)

Before this session's changes except the SHA-256 change (the driver was first linked after that one; the scalar SHA
adds roughly frame bytes divided by 430 MB/s to every row):

| Frames x bytes | Total Append | Average per frame | First frame | Last frame |
|---|---:|---:|---:|---:|
| 300 x 4 KiB | 295 ms | 0.98 ms | 0.56 ms | 1.44 ms |
| 2000 x 4 KiB | **7,719 ms** | 3.86 ms | 0.50 ms | **6.97 ms** |
| 100 x 1 MiB | 147 ms | 1.47 ms | 1.40 ms | 1.65 ms |
| 30 x 16 MiB | 384 ms | 12.8 ms | 13.1 ms | 10.9 ms |

The per-frame cost grows linearly with the number of frames already in the cache (about 3.2 microseconds per
existing entry): every `Append` re-encodes, re-hashes and rewrites the whole manifest (§4.4), so a long bake of cheap
frames is quadratic. A `sample` of the 2000-frame run put 72 % of `Append` in `RISECBOR64::IsUTF8NFC` (the
normalisation check of every key and digest string of the manifest) and most of the rest in copying the `Value`
tree and the four `fsync`s.

### 2.3 Playback: load and bind one cached frame (driver `playback_bench`, `MeshCacheSource`, sealed or partial disk caches)

| Cache | Vertices / triangles | Open and bind | First sample | Same frame, repeated | Sequential frames | Random scrub, average / worst |
|---|---|---:|---:|---:|---:|---|
| Liquid surface (dam break, partial: 30 of 46 frames) | 3,466 / 6,928 | 2.4 ms | 3.1 ms | 2.6 ms | 1.8 ms (includes 16 missing frames) | 1.6 / 3.0 ms (same caveat) |
| Cloth flag surface (complete, 91 frames) | 961 / 1,800 | 2.8 ms | 0.49 ms | 0.19 ms | 0.36 ms | 0.19 / 0.22 ms |

After the SHA-256 change (§8.2), same caches, same driver:

| Cache | Open and bind | First sample | Same frame, repeated | Random scrub, average / worst |
|---|---:|---:|---:|---|
| Liquid surface (dam break) | 1.5 ms | 1.1 ms | **0.58 ms** (was 2.6) | 0.32 / 0.62 ms |
| Cloth flag surface | 1.2 ms | 0.37 ms | **0.07 ms** (was 0.19) | 0.07 / 0.08 ms |

Every `Sample` reads the frame file, verifies its SHA-256 against the manifest, decodes the payload and converts it
to doubles; nothing is kept between samples, so a scrub costs the same as a sequential step. At 2.6 ms for a
1 MiB liquid frame the digest was most of it (scalar SHA at 430 MB/s is 2.4 ms per MiB). Not measured: grid, field,
points and transform frames (their codecs were read, not timed), and the render-side binding in the app (§11).

## 3. Cloth, rigid, group exchange, cache and bake worker: phase breakdowns

### 3.1 Cloth under a moving collider (the trampoline group, predictive; `sample` of the main thread, 7,246 samples, 10 s)

98 % of the bake is `ClothSystem::StepRecursive`; the sheet is 15 x 15 (675 degrees of freedom) with barrier
contact against the ball's mesh, which follows a scene-sampled `Track` motion. The pool (4 threads) is idle: the
cloth step is one dependent chain of small solves. The shares overlap:

| Phase | Function | Share of the main thread |
|---|---|---:|
| Kinematic continuation (collider and pinned motion, per candidate pair) | `BarrierContact::KinematicStep` | 36.6 % |
| ... its path bounds | `Motion::PathBound`, 16 `TrackMap` evaluations per point, each two `SampledTrack::At` (sine, cosine, Rodrigues) | 30.5 % |
| Newton iterations | `ClothSystem::Newton` | 43.0 % |
| ... linear solve, direct (banded Cholesky, bandwidth about 50) | `BandedCholesky::Factor` 25.7 %, `Solve` 1.7 % | 27.4 % |
| ... energy, gradient, Hessian | `ClothSystem::Evaluate` (`ElementEnergies` 3.3 %, `BarrierContact::Evaluate` 3.0 %) | 6.2 % |
| ... largest feasible step | `BarrierContact::MaxStep` | 3.3 % |
| Candidate pair search per iteration | `BarrierContact::Candidates` | 15.3 % |
| Collider placement (hash grid rebuilt from `unordered_map` nodes) | `Place` 3.9 %, `PlaceStage` 3.8 %, `PlaceGrid` 7.4 % (`Insert` 6.7 %) | about 8 % (overlapping) |
| Group exchange (ports, adapters, ledger) | `SimulationGroup::Serve`, `Exchange::Accept`, `RigidSurfaceAdapter::Convert` | under 0.3 % |
| Checkpoint restore (`ClothSolver::Apply`, fresh system and pool) | | 0.3 % |
| Cache encode, hash, write (`CacheWriter::Append`, `EncodeField`) | | 0.2 % |
| Rigid solver | `Rigid::ShapeMass` only | under 0.1 % |

So for this class: cloth contact is everything; rigid contact, the exchange, hashing and I/O do not register. With
the cloth pool idle, the single dependent chain is the limit; the useful levers are fewer or cheaper kinematic
bounds (A, §6) and a cheaper factorization (B).

### 3.2 Cloth flag (interactive mode, free cloth with a pinned edge, 91 frames, 3.5 s of main thread)

Plan and evidence (`ClothSolver::Configure`, `FreeVibration` and the other canonical cases) are 39 % of the whole
run; stepping is the rest: the Newton solves with the multigrid-preconditioned CG (`GridMultigrid::Cycle` and
`LineSweep` about 14 % of the whole run, `BandedCholesky::Factor` of the coarse level 12 %), element energies 8 %,
and the cache 66 ms of 91 frames (0.7 ms per frame; 4 % of the 17 ms frame). The flag's pool is used (`For` 247
samples) but its phases are short.

### 3.3 Plan-time evidence (every process, every scene load)

| Item | Cost | Where |
|---|---:|---|
| Cloth canonical evidence (nine cases, each a small simulation), cloth flag | about 0.3 s | `ClothSolver.cpp` `Memoised` / `RunEvidence` |
| Cloth canonical plus contact evidence (six more cases), trampoline sheet | about 2.8 s of the 3.6 s | same |
| Cloth-rigid coupling evidence (a 16-frame zero-gravity group run), trampoline | about 1.9 s | `Coupling/ClothRigidCoupling.cpp` `ComputeEvidence` via `CouplingEvidence` |
| Liquid evidence, liquid scenes | 1.3 to 1.7 s of 2.8 to 4.9 s | `Liquid` `ComputeEvidence` (§5.1) |

The cloth memo is per process (`static std::map`); every `rise-sim` invocation, every scene load in the renderer
and every GUI scene change repeats it. The cases are independent and deterministic.

### 3.4 Cache writer and codecs

See §2.2 for the numbers. Structure of one `CacheWriter::Append`: SHA-256 of the frame; `fsync` of the staged file,
`linkat`, `unlinkat`, `fsync` of the directory; then `Publish`: copy of the snapshot, a `Value` tree of every
entry, a canonical CBOR encode (every string checked for UTF-8 NFC), a SHA-256 of the manifest, a staged write with
`fsync`, `renameat`, `fsync` of the directory. Each `Sample` on the read side re-hashes the frame.

### 3.5 Bake worker

`rise-sim` writes a status sidecar durably after every frame (`WriteStatus`, `ReplaceFileDurably`: 14 of 735
samples on the flag, 2 %), runs the solver on its own thread and polls it with `sleep_for` (the main thread's
samples in `nanosleep`; not a cost). No other overhead appeared above 1 %.

## 4. Mechanisms behind the measurements

### 4.1 The fork-join pool (`Cloth::ParallelPool`, used by cloth and liquid)

The old pool had idle workers spin for 200 microseconds calling `std::this_thread::yield()` every 256 iterations,
woke every sleeper with `notify_all` for any job however few chunks it had, and had the caller yield-spin on the
completion counter. With 16 threads and jobs of a few chunks each (a liquid step issues thousands of them) that is
a system call storm: `swtch_pri` was 14,469 of 51,004 samples in the dam break, and the process showed 28 to 80 s
of system time in runs of 4 to 12 s. It also slows everything else on the machine, which matters when a bake runs
next to a render or the app. See §8.1 for the change and §2.1 for what thread count does to the liquid wall time.

### 4.2 Why cloth and liquid walls do not scale with threads

Cloth: a dependent chain of small solves (§3.1). Liquid: after about 2.8 s of serial plan-time evidence the stepping
itself is 20 to 150 ms per frame at 48^3 to 64^3, so the serial startup dominates a short bake, and the
multigrid half-sweeps dispatch many short jobs (§5).

### 4.3 Hashing

SHA-256 is used for every frame, every manifest, scene identities and cache keys (`RISECBOR64::SHA256Hex`); the
implementation was scalar (430 MB/s). Fire's FNV content hashes are separate (§7).

### 4.4 The manifest

The manifest is rewritten whole at every `Publish` (one per frame, one per static payload, one at seal), which keeps
every committed generation self-contained and the crash story simple; the cost is quadratic in frames (§2.2).

## 5. Liquid solver (`src/Library/Simulation/Liquid/`): profile and proposals for the next round

**Second half: §14** (re-profiled at m7m.1; L1 done with persistence, the restriction part of L2 and the sheets' barrier
build done, all byte-identical; B4 measured and not adopted). The rest of this section is the first-half write-up.
Not edited here. All samples are of the base binary; the stepping phases are taken from the sampled step thread
with the plan-time evidence subtree removed. The windows are short (the bakes finish in 1 to 3 s of stepping at
these sizes), so shares are good to a few percent.

### 5.1 The largest single item: plan-time evidence (`anon::ComputeEvidence`, `ComputeBodyEvidence`)

`LiquidSolver::Configure` runs the solver's own validation problems at plan time: `RunTaylorGreen`,
`RunColumnCollapse`, `StandingWave`, and for body scenes `RunFloatingStatics` / `HeldForce`, `RunAddedMassImpulse`,
each a `LiquidModel::Run` of tens of `Step`s. 1.29 s of 2.41 s of the main thread in `liquid_rigid_bodies`
(`ComputeBodyEvidence` 1.12 s), 1.69 s of 2.65 s in `liquid_cloth_catch` (`StandingWave` 0.37 s), 1.35 s of 2.0 s in
the dam break. `rise-sim status` of the liquid scenes takes 2.8 to 4.9 s and so does the renderer's scene load.
Proposals, in order of value:

1. **Run the independent cases concurrently** (the same pattern as the cloth change in §8.4; each case builds its own
   model from the options, so the results are unchanged): expected 2 to 3 times faster; risk low; bytes unchanged;
   the pool inside each case should be created with fewer threads (4) so that the cases do not oversubscribe.
2. **Memoise within the process on a key of the options** (as the cloth evidence does) if `Configure` is called more
   than once per process for the same options (the group `Configure` calls it again). Bytes unchanged.
3. **Persist the evidence across processes**, keyed by the content hash of the options and of the solver version
   string. Saves the whole 1.3 to 1.7 s per process; bytes unchanged. The store now exists (§13.4,
   `SimulationSolver.h` `LoadPersistedEvidence` / `PersistEvidence`, bound to the build so staleness cannot occur);
   adopting it is a few lines in `LiquidSolver.cpp` `ComputeEvidence` / `ComputeBodyEvidence`: build a key `Value`
   from the solver id, `SolverVersion` and every option the cases read, `LoadPersistedEvidence("liquid.mac", key, e)`
   first, and `PersistEvidence` after computing.

### 5.2 Per-step phases (share of `LiquidModel::Step` on the step thread, evidence removed)

| Phase (function) | Dam break (629 samples) | Catch (863) | Wave (792) |
|---|---:|---:|---:|
| Pressure: `Project` (incl. the calls from `ProjectCoupled`) | 39.1 % | 23.6 % | 43.9 % |
| ... of which `Multigrid::Precondition` (share of `Project`) | 52.9 % | 47.5 % | 67.2 % |
| ... of which pool dispatch and wait in `Project` (`ParallelPool::For`, share of `Project`) | 23.9 % | 11.3 % | 12.1 % |
| `ProjectCoupled` (sheets as moving boundaries; calls `Project`) | n/a | 20.4 % | 39.6 % |
| Velocity advection (`LimitedAdvectionStage`) | 23.7 % | 15.6 % | 12.8 % |
| Velocity extrapolation (`ExtrapolateVelocity`) | 14.6 % | 21.8 % | 11.9 % |
| Level set: `Reinitialize` / `AdvectLevelSet` | 7.8 % / 7.0 % | 5.2 % / 5.6 % | 3.9 % / 4.4 % |
| Sheets: `SweepSheetCells` / `SheetDynamics::Prepare` / `CloseSheetFaces` / `BuildBarrier` | n/a | 11.8 / 5.6 / - / 1.3 % | 6.2 / 4.0 / 6.2 / - % |
| Marker particles (`Advect`, `Correct`, `AdjustRadii`) | 2.4 + 1.6 + 0.8 % | 1.6 % (advect) | 1.3 + 1.1 + 0.9 % |

Earlier, shorter samples of the whole process (evidence included, so unsuitable for phase shares) gave the same
ranking for the rigid-body scene: `ProjectBodies` 26 % of `Step` (it is a pressure solve with body coupling) next to
`Project` 26 %, and for honey `ViscousStep` 12.8 % of `Step` beside `Project` 33 %. The honey and rigid-body scenes
finished their bakes inside the evidence window of the sample and were not resampled; **their per-phase tables are
not measured** (§11).

### 5.3 Proposed changes (function names, measured share, proposed change)

| # | Change | Where | Measured share | Expected gain | Bytes |
|---|---|---|---:|---|---|
| L1 | Plan-time evidence: concurrent cases, process memo, optional persistence (§5.1) | `LiquidSolver.cpp` `ComputeEvidence`, `ComputeBodyEvidence` | 40 to 70 % of a short bake or a scene load | 2 to 3 times on the 1.3 to 1.7 s; 0 s with persistence | unchanged |
| L2 | `Multigrid::Precondition` / `Cycle` / `HalfSweep`: fuse the red and black half sweeps and the restrict/prolong passes into fewer `ForRows` dispatches, bigger row blocks on the coarse levels (a coarse level of a few thousand cells should run on the caller, not on 16 threads) | `LiquidGrid.h` `ForRows`, `Multigrid` | 53 to 67 % of `Project`, plus 12 to 24 % pool dispatch | 1.3 to 1.5 times on `Project` (about 15 % of a step) | unchanged if each cell's arithmetic and the chunking stay as they are |
| L3 | `ForRows` chunk policy: below a work threshold run serially (the pool change in §8.1 makes dispatch cheaper but not free) | `LiquidGrid.h` | the 12 to 24 % above | 5 to 10 % of `Project` | unchanged |
| L4 | `LimitedAdvectionStage`, `UpwindAdvectionRhs`, `OneSidedDerivatives`: one pass over the faces rather than per-component passes, cache-friendly (x fastest) traversal | `LiquidFlow.cpp`, `LiquidLevelSet.cpp` | 13 to 24 % | 1.2 to 1.5 times on the phase | unchanged if the per-face arithmetic is the same |
| L5 | `ExtrapolateVelocity` (three passes of `ForRows` with a mask; 12 to 22 %) run on the active band only | `LiquidFlow.cpp` | 12 to 22 % | up to 2 times on the phase | unchanged if the band covers every cell the result reads |
| L6 | Sheets: `SweepSheetCells`, `SheetDynamics::Prepare`, `CloseSheetFaces` (12 to 17 % together) once per macro step instead of per sub-step where the geometry did not move | `LiquidSheets.cpp`, `LiquidModel.cpp` | 12 to 17 % | 1.1 to 1.2 times overall | unchanged if the geometry is read-only between the calls |
| L7 | `SampleVelocity` / `SampleCells` in the marker particle passes: hoist the cell lookup, process particles in cell order | `LiquidFlow.cpp`, `LiquidBodies.cpp` | 3 to 5 % (particles are cheap at these sizes) | small | unchanged only if the particle order and per-particle arithmetic stay; sorting particles changes the order of the particle output, so this one is **B** unless the sort is undone before output |

B-class liquid changes (list only, not to be done without a decision): looser `pressure_tolerance` / fewer
iterations, a different preconditioner (e.g. a two-level or a smoother with fewer sweeps), a different reduction
order in the dot products, single-precision coarse levels. Each moves every frame's bytes.

## 6. Tuning opportunities, ranked

**A. Byte-preserving (output bytes cannot change).** "Done" items are in §8.

| Rank | Opportunity | Where | Measured / expected gain | Risk | Status |
|---:|---|---|---|---|---|
| A1 | Plan-time evidence concurrency and memoisation (cloth, coupling, liquid) | `ClothSolver.cpp`, `ClothRigidCoupling.cpp`, Liquid | cloth contact evidence: trampoline status 3.66 to 2.36 s; liquid 1.3 to 1.7 s; coupling 1.9 s | low; cases are independent and deterministic | cloth done; coupling done (§13.3: computed beside the members' configuration); liquid done (§14.2: concurrent cases, status 2.5 to 4.6 s to 1.5 to 2.3 s) |
| A2 | Persist plan-time evidence across processes (hash of the key, the solver version and the build) | the three evidence sites | removes up to 2.3 to 4.7 s per process | low: the entry is bound to the linked image's identity, so a code change can never reuse one | cloth and coupling done (§13.4: trampoline status 2.1 s to 0.04 s); liquid done (§14.2: status 0.05 s warm) |
| A3 | Hardware SHA-256 | `RISECBOR64.cpp` | 429 to 2,869 MB/s (6.7 times); frame verify 2.4 to 0.4 ms per MiB; every Append and Sample | low; checked against `shasum` on 20 sizes and incremental updates | done |
| A4 | ASCII fast path of `IsUTF8NFC` (every manifest key and digest) | `RISECBOR64.cpp` | cache writer: 2000 x 4 KiB frames 7.7 to 2.1 s; last-frame Append 7.0 to 1.4 ms | low; 2.3 million strings agree with the full path | done |
| A5 | Pool without syscalls (spin hint, bounded spin, wake only as many helpers as chunks) | `ClothParallel.cpp` | system time 37.9 to 2.2 s on a 16-thread liquid bake; wall neutral on a quiet machine and better on a loaded one | low; chunking and reduction order untouched | done |
| A6 | Moves instead of copies of `Value` trees in the manifest encode and the CBOR decoder | `RISECBOR64`, `EncodeManifest` | part of A4's remaining cost (about 40 %) | none | done |
| A7 | Per-stage path bounds computed once per vertex (`KinematicStep`): each vertex is in many candidate pairs | `ClothContact.cpp` | up to 30 % of the trampoline step (PathBound), expected 15 to 25 % wall | low: the same function of the same inputs | done, see §8.5 |
| A8 | Incremental or batched manifest publication: encode the new entry only and keep a running manifest, or publish every k frames and at seal | `SimulationCache.cpp` `Publish` | removes the O(n) per frame: last-frame Append 1.4 ms to about 0.7 ms at 2000 frames | medium: weakens "every committed frame is durable and in the manifest" unless the frame files are still individually durable and the resume path re-publishes; final manifest bytes unchanged | proposed |
| A9 | Cache hash grid with open addressing and a reused flat array instead of `unordered_map<uint64, vector>` rebuilt at every `Place` | `ClothContact.h/.cpp` | `Place*` and `Insert` are about 8 % of the trampoline | low; the first attempt (a sort-based flat table) was slower; the second (open-addressed table built by a counting sort, §13.2) is faster | done: trampoline 6.9 % fewer instructions, frames identical |
| A10 | `BandedCholesky::Factor` loop structure (45 % of the trampoline step after A7): block the update loops, keep each row's inner product in the same order | `ClothLinearAlgebra.cpp` | up to 1.5 to 2 times on 45 % | **high for byte identity**: under `-ffast-math` the compiler splits each inner product over vector lanes in an order the source does not state; a blocked kernel vectorises differently | not done (§13.5) |
| A11 | `SampledTrack::At` memo | `ClothModel.cpp` | 13.7 % fewer instructions but 1 % wall: the TLS lookup costs about what the sine and cosine do | n/a | tried and reverted |
| A12 | Skip the redundant digest of a frame that was just verified (render path reads a frame once per sample) | `SimulatedMeshSource`, `SimulationCache` | with A3 already down to 0.4 ms per MiB | low | not needed after A3 |
| A13 | Overlap hashing with the write of a frame file (hash on the solver thread, write and fsync on another) | `SimulationCache.cpp` | 30 x 16 MiB: 13 ms per frame of which about 6 is the hash | medium (ordering with the manifest) | proposed, small |
| A14 | Fire (not touched): see §7 | | | | |

**B. Would change results (not implemented).**

| Rank | Change | Why it would help | Why it changes bytes |
|---:|---|---|---|
| B1 | Fill-reducing ordering (reverse Cuthill-McKee or nested dissection) for the cloth `BandedCholesky` instead of the natural vertex order | Factor is 26 % (45 % after A7) of the trampoline | different elimination order: every solve differs in the last bits. **Done (§13.1, m3a.8)**: trampoline 26 % fewer instructions, 1.3 times faster as B1 alone; landed after the self-collision incline evidence was made well posed (§13.7) |
| B2 | Cloth: reuse a factorization across Newton iterations (modified Newton) or a looser `cg_tolerance` / `tolerance` | fewer factorizations | different iterates |
| B3 | Cloth: a coarser `PathBound` (fewer than 16 samples) or a looser safety factor in `KinematicStep` | the largest kinematic cost | different stage lengths, different contact sequence |
| B4 | Liquid: looser pressure tolerance, fewer V-cycle sweeps, a two-level preconditioner, single-precision coarse levels | `Precondition` is 50 to 70 % of `Project` | different pressures. **Fewer sweeps and two or three levels measured (§14.4): more iterations, no net gain (best 2 % of a step); not adopted** |
| B5 | Liquid and cloth: reductions in a different order (tree sums, chunk size), threads in the order of completion | removes serial reductions | floating-point sums reorder |
| B6 | Cache: a compressed or quantised frame codec (zstd, half floats) | less I/O, smaller caches | the frame bytes change by definition |
| B7 | Fire: fewer or cheaper identities, no per-stage hashes in fast mode | about half of the CPU busy time (§7) | protocol change of the certified path; in fast mode the digests are not part of the output, but the pinned tests cover them |

## 7. Fire (profile and write-up only; version 3 was just baselined, bytes must not move)

Tier 6 (the minimum the schema allows), fast mode, CPU backend (`backend cpu`), the committed campfire scene; about
14 accepted steps in 69 s of wall, 272 s of user CPU (3.9 cores of 16 busy), peak RSS 2.97 GB. A `sample` (25 s, all
threads) puts about two thirds of the non-idle samples in worker pool tasks:

- `FireProductionFNV1aWords` slices (the sliced content hash, §11.6 of the M8 results) 26,054 self samples plus 3,446
  in its callers `OwnerHashFloats`, `HashEOSFloatArray`, `OwnerResultIdentity`, `OwnerHashCoupledStage`,
  `FireProductionProjectedHeunTransportPublicationIdentity`, `HeunStageInputIdentity`, `OwnerHashFluxStage`,
  `OwnerHashFCT`, `HeunFCTRequestIdentity`: roughly half of the busy CPU **even in fast mode**, which the notes say has
  no per-step seals or digests; the identities are still computed and used inside the owner. The remaining busy time:
  `EvaluateFireProductionScalarFCTStagesCPU` 6,909 + 4,838 self, `ApplyOperator` (projection smoother) 2,824,
  `_platform_memcmp` 2,098 (the exact-content memo of the stage identity), `BuildFireProductionScalarPhysicalFluxPrerequisiteCPU`
  1,998, `pow` 1,625, `EvaluateCellMolecularTransport` and `MixturePropertiesBySpeciesOrder` (3,714 / 2,939 inclusive),
  `FireSimulationMethaneRecord::FindSpecies` 2,299 inclusive (a string search in a hot path).
- The 3.9 cores of 16 reflect the workers waiting for the main thread between phases.

Proposals for the next round (fire files untouched here): (a) skip identity hashing that nothing reads in fast mode,
or hash a cheaper summary there (B7; protocol); (b) replace `FindSpecies(const char*)` in the per-cell transport
loop with an index resolved once per step (A, bytes unchanged); (c) keep more of the hashing off the critical path
by hashing a stage's output on the worker that produced it (A); (d) the memcmp of the exact-content memo reads every
byte once more than the hash does; compare 64-bit words or hash and compare lengths first (A). The Metal path was not
profiled.

## 8. What was implemented (all A, no byte changes)

Before and after use the same machine state where stated; all after-bakes were compared with the before-bakes by the
set of frame file names (SHA-256 of the content).

### 8.1 Fork-join pool without a system call storm (`Cloth/ClothParallel.cpp`)

Workers spin with the CPU's yield hint (no system call) for 100 microseconds and then sleep; the caller does the same
while it waits for stragglers and gives its core up with `yield()` only after 50 microseconds; `For` wakes
`min(sleepers, chunks - 1)` sleepers with `notify_one` instead of all of them. Chunk assignment, chunking and every
reduction are untouched, so results are identical for any thread count.

| | Before | After |
|---|---:|---:|
| Dam break, 12 frames, 16 threads: wall (user / sys) | 4.93 s (11.1 / 37.9 s), 4.91 s repeat | 4.26 s (19.7 / 2.2 s), 4.35 s repeat |
| Dam break, 8 threads | 4.11 s (15.8 / 5.6 s), 4.12 s repeat | 4.68 s (14.1 / 0.8 s), 4.15 s repeat |

| Dam break, all 46 frames, default 16 threads, load 11 to 12 | 12.28 s (27.2 user / 100.7 sys) | 10.33 s (51.1 / 3.8) |
| Honey and water, 30 frames, load 11 to 15, two runs each | 20.17 and 20.06 s (29.9 / 66.5 and 29.4 / 79.8) | 18.74 and 18.17 s (50.0 / 3.7 and 49.5 / 3.6) |

Load average 4 to 8 during the first two rows, 11 to 15 for the last two. Wall is neutral to 10 to 20 % better (the
serial evidence dominates short bakes); total CPU drops from 96 to 110 s to about 53 s on the honey run and the system
time by 17 to 28 times. Frame sets identical at 16 threads (dam break) and for the dam break, honey and rigid-body
scenes against the before bakes. (The pool in isolation was also compared with the old pool on the current tree for
`liquid_cloth_catch`: identical frames. That scene's liquid output differs from the before-bake because of the other
agent's uncommitted edits to `LiquidModel.cpp` and `LiquidLevelSet.cpp` in the shared working tree, not because of
anything in this document; its cloth and group frames are identical.) A single-core dependence of the spin in a
heavily oversubscribed machine remains: with the load average above the core count the busy-wait costs user time
instead of system time.

### 8.2 SHA-256 on the ARM SHA-2 instructions (`Utilities/RISECBOR64.cpp`)

`TransformBlocksHardware` (`vsha256hq_u32`, `vsha256h2q_u32`, `vsha256su0q_u32`, `vsha256su1q_u32`) behind
`__aarch64__ && __APPLE__`; the portable rounds stay for every other platform. `Update` also hands whole 64-byte
blocks straight from the input to the transform instead of copying each through the buffer. 256 MiB of constant
bytes: 429 MB/s before, 2,869 MB/s after. Checked against `shasum -a 256` for 20 sizes (0 to 1 MiB + 13, including
every padding boundary) and for an incremental update in odd chunk sizes: all equal.

### 8.3 ASCII fast path in `IsUTF8NFC` and move construction of `Value` trees (`RISECBOR64.cpp/.h`, `SimulationCache.cpp`)

Pure ASCII is valid UTF-8 and is NFC; the function now returns `true` after a scan of the bytes instead of building
two code point vectors and running the decomposition and composition passes. All 2,313,669 strings of a check (every
1-, 2- and 3-character ASCII string, 200,000 random ASCII strings and five non-ASCII ones) give the same answers with
the old and the new function (the signature of the answers is equal). `Value::ArrayValue(Values&&)` and
`MapValue(Members&&)` are new, used by `EncodeManifest` and by the CBOR decoder (which also moves items and members
instead of copying them).

| `CacheWriter::Append` (driver `cache_bench`) | Before | After the NFC and move changes |
|---|---:|---:|
| 300 x 4 KiB | 295 ms (0.98 ms per frame) | 167 ms (0.56 ms) |
| 2000 x 4 KiB | 7,719 ms (last frame 6.97 ms) | **1,751 ms (last frame 1.21 ms)**: 4.4 times |
| 100 x 1 MiB | 147 ms | 139 ms (hash and disk bound) |
| 30 x 16 MiB | 384 ms | 400 ms (no change; the `before` already had the hardware SHA) |

### 8.4 Parallel cloth evidence (`Cloth/ClothSolver.cpp`, `RunEvidence`)

The nine canonical cases and the six contact cases (four more with self-collision) run with `std::async`, each on a
copy of the settings, and are collected and added in the same order as before. They are single-threaded,
deterministic and share no state. `rise-sim status` of `rigid_cloth_trampoline`: 3.66 s before, 2.36 s after (load
average 7 to 12; the remaining 1.9 s is the coupling evidence). Status text identical for three scenes (the cache
keys, which include the evidence digests, are equal).

### 8.5 Per-vertex path bounds in `KinematicStep` (`Cloth/ClothContact.cpp`)

The bound `PathBound(x, t0, t1)` of a point depends only on its position and its motion, fixed for the whole call,
and each vertex belongs to many candidate pairs: it is now computed once per vertex per call (a vector of
sentinels). Trampoline group, same machine state (load average 10 to 12 for both runs):

| | Before | After |
|---|---:|---:|
| 10 frames, wall | 12.24 s | 8.35 s |
| 10 frames, instructions retired | 244.0 G | 165.8 G (-32 %) |
| 31 frames (all), wall | 47.66 s | 39.90 s |
| 31 frames, instructions retired | 991.5 G | 762.5 G (-23 %) |
| Stepping time per frame (wall minus the separately measured startup of 3.66 s before and 2.36 s after, §8.4) | about 0.86 s (10 frames), 1.42 s (31 frames) | about 0.60 s, 1.21 s |

Frame sets identical for the 10-frame and the 31-frame bakes (62 files). The wall gain on the stepping part is
15 to 30 %, smaller than the instruction gain because the remaining time is latency bound (the Cholesky and the hash
grid walk memory in dependent chains).

### 8.6 Tried and reverted

A flat sort-based hash grid (slower, A9) and a per-thread memo of `SampledTrack::At` (13.7 % fewer instructions,
1 % wall, A11). Both reverted; the working tree does not contain them.

## 9. Test and warning status

- Build: `$S/mk.sh all tests build-tool/rise_sim build-tool/simulation_cache_import`, 452 test programs, **no
  warnings or errors** (only the Homebrew "built for newer macOS" linker notices, filtered).
- `./run_all_tests.sh --tier fast --timeout 300 --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'` on the tree with all the
  changes of §8: **451 of 452 pass**. The one failure is `LiquidClothCouplingTest` ("the piston's acceleration is
  p0 / (sigma + rho d)" and "liquid, face liquid and plate gain the load's impulse"): it is the liquid agent's
  in-progress work (the test file and `LiquidModel.cpp` / `LiquidLevelSet.cpp` are modified in the working tree by
  them); its section G, which compares threads and checkpoints, passes. The determinism and cache tests
  (`SimulationCacheTest`, `SimulationGroupTest`, `SimulationWorkerTest`, `SimulationBakeManagerTest`, the cloth tests,
  `SimulationSceneTest`) pass.
- After the suite, `ClothParallel.cpp` was swapped for the committed version for one comparison build and restored
  (byte for byte), and everything was rebuilt once more, warning-free; the suite was not run again on that rebuild.

## 10. Build flags: what is left on the table (reported, not changed)

| Build | Flags | Finding |
|---|---|---|
| make (`Config.OSX`) | `-O3 -flto` (full LTO when measured; thin `-flto=thin` is the default since fire solver version 4), `-ffast-math -fno-finite-math-only`, `-funroll-loops`, default `-mcpu=apple-m1` | Already aggressive. No profile-guided optimisation; no `-mcpu=apple-m4` (the M4 has SME but the code is NEON; a newer `-mcpu` gains little for this code and ties the binary to M4). `-ffast-math` is on for the whole library except the fire production sources, which compile with `-fno-fast-math -ffp-contract=off` |
| Xcode, Deployment configuration, targets `rise` and `rise-sim` | `GCC_OPTIMIZATION_LEVEL = 3`, `LLVM_LTO = YES_THIN`, `-ffast-math -fno-finite-math-only` (updated in `24423067f`; see [BUILD_FLAGS.md](BUILD_FLAGS.md)) | **Correction:** the guess here that a Deployment `rise-sim` may write different bytes than the make `rise_sim` was measured and is wrong: the frame sets are identical ([BUILD_FLAGS.md](BUILD_FLAGS.md)). Xcode `Deployment` now carries `-ffast-math -fno-finite-math-only` matching `make` (with the strict-FP simulation/fire/OSL/radiometric sources pinned to `-fno-fast-math -ffp-contract=off`). make's LTO default went full, then thin, then back to full (`24423067f`: thin LTO moved two pinned fire digests), and is **thin again since fire solver version 4** (`f7d0b2f4b`: the fire-adjacent sources compile strict, so fire bytes no longer depend on the LTO mode; `make LTO=full` restores full LTO; [BUILD_FLAGS.md](BUILD_FLAGS.md) §8). The §13 measurements below are thin-LTO builds compared with thin-LTO builds. |
| Library compile options for the simulation | none specific | The simulation hot loops (cloth `BandedCholesky`, liquid multigrid) are plain scalar loops that clang vectorises only as far as `-ffast-math` allows; an explicit NEON kernel would be a result-changing rewrite (B) unless the lane order is the same |

## 11. Not verified, not measured

- Metal fire (not run; §2 note 3). Fire CPU at tiers above 6 and in certified mode.
- Per-phase tables of the rigid-body and honey liquid scenes (their bakes finished inside the evidence window of the
  `sample`); liquid thread utilisation on a quiet machine (the machine was loaded throughout).
- Playback of grid, field, points and transform frames, and the render-side binding (`SimulatedMeshGeometry` and
  the app's viewport): only the cache source's `Sample` was timed, not the installation of the mesh into the BVH or
  the GUI.
- Solver sub-step counts (not exposed by `rise-sim`).
- The GUI app was not run; the Xcode build was not built (flags read from `project.pbxproj`).
- Quiet-machine wall times: every wall time here carries the load average noted with it.

## 12. Files changed

`src/Library/Simulation/Cloth/ClothParallel.cpp`, `src/Library/Simulation/Cloth/ClothSolver.cpp`,
`src/Library/Simulation/Cloth/ClothContact.cpp`, `src/Library/Simulation/SimulationCache.cpp`,
`src/Library/Utilities/RISECBOR64.cpp`, `src/Library/Utilities/RISECBOR64.h`, this document. No new source files.
Drivers and raw samples are in `/Volumes/SSD/claude-tmp/perf-scratch/` (not in git).

## 13. Second half, cloth part (2026-11-27, at `052fc4338`, thin LTO)

Same machine, using thin LTO (`make LTO=thin`; the default was full at the time, `24423067f`, and is thin again since fire solver version 4, [BUILD_FLAGS.md](BUILD_FLAGS.md) §8): every before/after pair below is
thin against thin (`base_thin` is `052fc4338` built in that mode). The machine was heavily shared (load average 10 to
130, noted per table); instructions retired (`/usr/bin/time -l`) are the load-independent figure. Bakes:
`cloth_flag_scene` (all 91 frames), `rigid_cloth_trampoline` (all 31 frames), `liquid_cloth_catch` reduced to 12
macro steps. Byte identity is the frame-name comparison of §1. Scratch, drivers and bakes:
`/Volumes/SSD/claude-tmp/perf2-scratch/`.

### 13.1 B1, fill-reducing ordering for the cloth factorisation: done (cloth.shell m3a.8)

**Structure.** With the default `bending lines` the pattern couples each vertex to its eight neighbours and to the
vertices two along its row and two along its column (rods), so the narrower grid order (u- or v-fastest) has a band of
two grid lines: 6 nu + 2 scalars. Cholesky fills a band completely, and in grid order the envelope (each row from its
first nonzero) is the band, so a smarter storage alone gains nothing; a different ordering is needed.

**Choice.** Orderings compared with a symbolic driver (`order_probe.cpp`) on the four committed meshes: grid order,
reverse Cuthill-McKee (RCM) from a pseudo-peripheral vertex, exact minimum degree, and a geometric nested dissection
with two-line separators. On these sizes (169 to 961 vertices) minimum degree and nested dissection give **no less
fill and no fewer operations** than RCM with envelope storage (trampoline: nnz(L) 40.8 K minimum degree, 46.6 K nested
dissection, 42.6 K RCM envelope; operations within 3 % of RCM for minimum degree, 27 % more for nested dissection), and
a general sparse factorisation would replace the contiguous inner products of the band by indexed ones. Asymptotically
nested dissection wins (O(n^1.5) against O(n^2)), but the crossover is well above these meshes. So: RCM + envelope
(variable-band) Cholesky, `EnvelopeCholesky` in `ClothLinearAlgebra.{h,cpp}`, with the grid order as a candidate (the
cheaper of the two is used), pinned vertices decoupled (their off-diagonal blocks are zero after `MaskFixed`; a
nonzero there would trigger a re-analysis with the full pattern), computed once per pattern and pin set, ties by
vertex index, everything on one thread in a fixed order. `BandedCholesky` itself was left unchanged (liquid.mac uses
it, see below). The direct/iterative choice (`directCost_`) keeps the band's cost, so no scene changes solver. The
multigrid's coarsest level used the same class.

**Per mesh** (driver `env_probe.cpp`: the real pattern, a random SPD matrix masked like `MaskFixed`, 200 repeats):

| Mesh (pins) | dofs | Band: half-width / stored / multiply-adds / Factor / Solve | Envelope RCM: half-width / stored / multiply-adds / Factor / Solve | Factor speed-up |
|---|---:|---|---|---:|
| trampoline 15 x 15 (corners) | 675 | 92 / 62.1 K / 2.62 M / 900 us / 62 us | 89 / 41.7 K / 1.47 M / 518 us / 35 us | 1.74 |
| catch 17 x 17 (boundary pinned) | 867 | 104 / 90.2 K / 4.35 M / 1,592 us / 90 us | 92 / 42.7 K / 1.52 M / 535 us / 36 us | 2.98 |
| wave 13 x 19 (edge v0) | 741 | 80 / 59.3 K / 2.23 M / 746 us / 66 us | 83 / 43.3 K / 1.49 M / 519 us / 26 us | 1.44 |
| flag 31 x 31 (edge v0; not direct in the scene, multigrid) | 2,883 | 188 / 542 K / 49 M / 20.3 ms / 0.57 ms | 185 / 350 K / 24.8 M / 9.8 ms / 0.20 ms | 2.07 |

Relative difference of the solutions from the band solver's: 3e-17 to 2e-15; residuals 2e-15 either way.

**Bakes, B1 alone** (load average 10 to 36; before = `base_thin`):

| Scene | Before: wall / instructions | B1: wall / instructions | Max \|x_B1 - x_before\| over all frames (at the last frame) |
|---|---|---|---|
| `cloth_flag_scene`, 91 frames | 1.24 s / 44.1 G | 1.13 s / 41.0 G (-7 %, the multigrid's coarse level) | 3.7e-9 m (0 at binary32) |
| `rigid_cloth_trampoline`, 31 frames | 41.4 s / 763 G | **31.8 s / 566 G (-26 %)** | 5.7e-14 m (0); the ball's frames identical |
| `liquid_cloth_catch`, 12 macro steps | 6.77 s / 544.5 G | 6.58 s / 535.7 G (-1.6 %; the liquid dominates) | cloth frames identical; 1 of 4 liquid frames differs in bytes, its positions identical at binary32 |

(`sample` of the trampoline before: `BandedCholesky::Factor` 3,349 of 7,473 main-thread samples, 45 % of the step
after A7.) The gain passes the 10 % bar easily.

**Final state** (B1 + A9 + A1 + A2, evidence store cold; the two binaries run back to back at load average 4 to 6):

| Scene | `base_thin` (052fc4338): wall / instructions | Final: wall / instructions | Max \|x - x_before\| (at the last frame) |
|---|---|---|---|
| `cloth_flag_scene`, 91 frames | 0.83 s / 44.0 G | 1.19 s / 40.6 G (-7.7 %) | 3.7e-9 m (0) |
| `rigid_cloth_trampoline`, 31 frames | 40.6 s / 763.8 G | **27.3 s / 511.7 G (-33 %; 1.49 times faster)** | 5.7e-14 m (0); the ball's 31 frames identical |
| `liquid_cloth_catch`, 12 macro steps | 6.68 s / 540.1 G | 6.47 s / 518.2 G (-4 %) | cloth frames identical; liquid positions identical at binary32 (1 of 4 frames differs in bytes) |

(The flag's walls are under a second and noisy; its instruction count is the figure to read. The catch's `base_thin`
binary was built at 14:37 and the final one at 15:17: the liquid agent's edits in between may be part of the catch
difference.) With a warm store each of these bakes also skips the plan-time evidence (§13.4).

**Validation.** `ClothValidationTest` with B1 holds every gate with the same margins (membrane frequencies, plates,
energy drift, catenary and cantilevers identical to four digits; contact and self-collision cases within their
tolerances) once the self-collision incline cases are well posed (§13.7). Before that fix, the polyester self-stick
case flipped from 1.235e-2 to 1.784e-2 against its 1.6e-2 gate, and so did the old solver with its grid order
reversed: the case was measuring round-off, not the solver.

**Liquid side.** liquid.mac does call `Cloth::BandedCholesky` (`LiquidSheets.cpp` `SheetDynamics::Prepare` factors
its copy of the sheet's response under its own breadth-first `BandOrder`; `Solve` and `Predict` back-substitute inside
the pressure system). B1 leaves that class and that call path byte for byte as they were, so no liquid code changes;
the liquid's bytes move only because the cloth's values do. **Next round:** the liquid's sheet solve could adopt
`EnvelopeCholesky` (with its pins decoupled; the catch sheet's factor would be about 3 times cheaper, see the table
above). The share it would affect is `SheetDynamics::Prepare` (factor) plus the back-substitutions inside
`ProjectCoupled`: `Prepare` was 5.6 % of `LiquidModel::Step` in the catch and 4.0 % in the wave (§5.2), so the gain is
a few percent of a liquid step; it changes the liquid's bytes (a liquid version bump).

### 13.2 A9, contact hash grid: done, bytes identical

`HashGrid` (`ClothContact.h`) is now an open-addressed table (linear probing, splitmix64 of the cell key) of the
occupied cells over one flat id array, built in two passes by a counting sort (`Insert` records the primitive's cell
range, `Finish` counts, takes prefix sums and fills); the vectors are reused from one `Place` to the next. Each cell
holds its ids in insertion order, as before, and every caller either sorts and deduplicates its candidates or takes a
minimum, so the result cannot depend on the table. Trampoline, 31 frames: 763.4 G to 710.9 G instructions (-6.9 %;
the persistence of §13.4 was cold, so this is the grid), wall 41.4 to 37.7 s (load 15 to 30). Flag 44.1 to 43.7 G.
Frame sets identical to `base_thin` for all three scenes (flag 91, trampoline 62 files, catch cloth and liquid),
and for the trampoline with the cloth's `threads 1` (a scratch copy of the scene) against the default threads.

### 13.3 A1, coupling evidence beside the members' configuration: done, bytes identical

The coupling evidence is one dependent run (a 16-frame zero-gravity group), already memoised per process; what can
overlap is its run with the configuration of the group's members (their own evidence). `SceneSimulationGraph::Plan`
now derives each group's likely coupling kind from its `connect` lines (the sorted adapters) and starts
`PrefetchCouplingEvidence(kind)` (`SimulationGroup.h`) on another thread before configuring the members;
`CouplingEvidence` waits for it (same mutex, same memo; a wrong guess only costs CPU), and the futures are joined
before `Plan` returns. `rise-sim status rigid_cloth_trampoline`, store off, three runs each at load average 100 to
130: 2.57 / 2.65 / 2.73 s before, 2.05 / 2.15 / 2.17 s after; status text identical.

### 13.4 A2, plan-time evidence persisted across processes: done for cloth and coupling

`LoadPersistedEvidence` / `PersistEvidence` (`SimulationSolver.h/.cpp`): an entry is
`<simulation cache base>/evidence/<SHA-256 of (format, space, key, build)>.cbor` holding the key, the build identity,
the evidence (doubles as bit patterns, so values round-trip exactly) and a SHA-256 of it. The **build identity** is
the Mach-O `LC_UUID` of the image the library is linked into (the linker derives it from the image's contents), so
any rebuild with a code change gets other entries; where it is unknown (other platforms for now) the store is off.
Writes go to a temporary file renamed into place (safe under concurrent processes); a missing, short, corrupt or
foreign entry is ignored and recomputed (a driver flipped each of the 2,784 bits of an entry and truncated it: no
corrupt version loaded); the directory is pruned to the 192 most recently used entries when it passes 256 entries or
16 MiB; `RISE_SIMULATION_EVIDENCE_STORE=off` disables it. Users: the cloth memo (`ClothSolver.cpp` `Memoised`, key =
the evidence configuration with solver id and version) and the cloth-rigid coupling (`ClothRigidCoupling.cpp`, key =
the scenario's member parameters, the policy and both solvers' versions). Liquid can adopt it next round (§5.1).

| `rise-sim status` (load 100 to 130) | Before | Store off (A1 only) | Store cold (first process) | Store warm |
|---|---:|---:|---:|---:|
| `rigid_cloth_trampoline` | 2.57 to 2.73 s | 2.05 to 2.17 s | 1.97 to 2.06 s | **0.04 s** |
| `cloth_flag_scene` | 0.28 to 0.29 s | 0.30 to 0.31 s | 0.29 to 0.30 s | **0.04 s** |
| `liquid_cloth_catch` (liquid evidence not persisted) | 5.96 s | | 5.60 s | 4.99 s |

Bakes with a warm store: frame sets identical (all three scenes); trampoline 651 G instructions against 711 G cold;
the flag bake 32.3 G against 43.7 G. Status text (which carries the cache keys and so the evidence digests) is
identical computed, loaded, after corruption and under four concurrent cold processes. Scene load in the `rise` CLI
(`load` of the trampoline with a baked cache) takes 0.02 s with or without the store: it does not plan the
simulations; the app's scene load was not measured.

### 13.5 A10 (blocking the `Factor` update loops): not done

Under `-ffast-math` the compiler vectorises each inner product of `BandedCholesky::Factor` with its own split over
lanes and accumulators; a blocked or register-tiled kernel is vectorised differently, so each entry's sum would be
accumulated in another order and the bytes would move. Reproducing the compiler's lane order by hand is possible
but fragile (it changes with the compiler and flags). B1 is the better lever for the same 45 %.

### 13.6 Tests and warnings (this part)

Build `mk.sh all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free (only the Homebrew
"built for newer macOS" linker notices). `./run_all_tests.sh --tier fast --timeout 300 --filter
'[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'` with A1, A2 and A9 in the tree: **453 of 453 pass**. With B1 and §13.7 as
well (final state, cloth.shell m3a.8): **453 of 453 pass**, including the liquid-owned `LiquidClothCouplingTest`,
`LiquidSheetTest` and `LiquidRigidTest` (no cloth-dependent pin there moved). The full make of `all tests` is
warning-free; its only error is the link of `bin/tests/FireProductionCalibrationOracle` (undefined fire Metal
symbols, from the concurrent build-flag work; no fire file was touched here).

Files changed: `src/Library/Simulation/Cloth/ClothLinearAlgebra.{h,cpp}` and `ClothModel.{h,cpp}` (B1),
`src/Library/Simulation/Cloth/ClothValidation.cpp` and `tests/ClothValidationTest.cpp` (§13.7),
`tests/ClothSolverTest.cpp` (version pin m3a.8), `src/Library/Simulation/Cloth/ClothContact.{h,cpp}` (A9), `src/Library/Simulation/SimulationSolver.{h,cpp}`
(the store), `src/Library/Simulation/Cloth/ClothSolver.{h,cpp}` (m3a.8, self-slide gate, cloth persistence, `ClothSolverVersion()`),
`src/Library/Simulation/Coupling/ClothRigidCoupling.{h,cpp}` (coupling persistence), `src/Library/Simulation/SimulationGroup.{h,cpp}`
and `src/Library/Simulation/SceneSimulation.cpp` (prefetch), this document. No new source files.

### 13.7 The self-collision incline evidence made well posed (self-stick, self-slide)

**Cause.** `RunSelfIncline` first settles the upper layer on the pinned lower one by a frictionless static solve
(gravity normal to the layers, four frictionless walls 12.5 um beyond the layer's edges). In the plane of the layers
that solve has a neutral rigid mode: nothing resists an in-plane translation until a wall's barrier engages. The
settle's Newton iterations therefore left the layer wherever round-off pushed it: centred, or in one of the four
corners at (+-1.25e-5, +-1.25e-5) m (measured, driver `selfcase.cpp`, a replica of the case). The tilted run then
measured a creep speed that depends on the corner (2.92e-4, 2.977e-4 or 3.03e-4 m/s for polyester), i.e. on round-off.

**Perturbation set** (all on the same build): old `BandedCholesky` in grid order, the same with the grid order
reversed, the old solver with a 1-ulp increase of one initial coordinate (four choices of vertex and axis), the old
solver at 1 and 8 cloth threads; the new `EnvelopeCholesky` (RCM), it forced to the grid order, and it with three
1-ulp nudges. Spread = max - min of the gate quantity over the set, both integrators:

| Case | Material | Committed case: old solver (8 perturbations) | Committed: new solver (5) | Redesigned: old (8) | Redesigned: new (5) |
|---|---|---|---|---|---|
| self-stick | cotton-plain-weave | 7.77e-3 .. 1.686e-2 | 7.77e-3 .. 1.686e-2 | 1.2333e-2 (spread 0) | 1.2333e-2 (0) |
| self-stick | denim-twill | 2.383e-3 (0) | 2.383e-3 (0) | 1.2392e-2 (0) | 1.2392e-2 (0) |
| self-stick | silk-lightweight | 1.227e-2 .. 1.437e-2 | 1.227e-2 .. 1.437e-2 | 1.2272e-2 (2e-7) | 1.2272e-2 (2e-7) |
| self-stick | polyester-knit | 6.81e-3 .. 1.784e-2 | 6.81e-3 .. 1.784e-2 | 1.2346e-2 .. 1.2348e-2 (1.9e-6) | 1.2346e-2 .. 1.2347e-2 (1.0e-6) |
| self-slide | cotton-plain-weave | 2.983e-3 .. 3.019e-3 | 2.983e-3 (0) | 2.9468e-3 (0) | 2.9468e-3 (0) |
| self-slide | denim-twill | 3.841e-3 (0) | 3.841e-3 (0) | 3.6753e-3 (0) | 3.6753e-3 (0) |
| self-slide | silk-lightweight | 2.295e-3 .. 2.308e-3 | 2.295e-3 (0) | 2.2870e-3 (0) | 2.2870e-3 (0) |
| self-slide | polyester-knit | 3.498e-3 .. 3.555e-3 | 3.498e-3 .. 3.555e-3 | 3.4946e-3 .. 3.4982e-3 (3.6e-6) | the same (3.6e-6) |

(Backward Euler shown; BDF2 behaves the same, its values 1e-5 lower for stick and 15 % lower for slide.) The committed
case's intrinsic noise was up to 1.1e-2 on a gate of 1.6e-2: cotton already failed the gate under one 1-ulp nudge of
the old solver.

**Redesign.** The settle still supplies the normal (contact) equilibrium; afterwards the upper layer's in-plane
coordinates are reset to its plan placement (aligned with the lower layer, which is what the case states), so the
tilted run starts from one defined state. What the case tests is unchanged: the smoothed-friction creep speed of a
sticking layer (stick) and the sliding acceleration (slide) against their closed forms. Spreads fall to at most 2e-6
(stick) and 4e-6 (slide), relative 2e-4 and 1e-3, and the two solvers agree within them; the full test under four
solver variants (old grid, old reversed, new RCM, new grid) gives the same self-collision table to four digits.

**Gates**, by the rule the other gates use (1.25 times the largest value over the four built-in records and both bake
integrators, rounded up to two digits): self-stick 1.25 x 1.239e-2 = 1.549e-2, so **1.6e-2 (unchanged)**; self-slide
1.25 x 3.675e-3 = 4.594e-3, so **4.6e-3 (was 4.8e-3)**. Both in `ClothSolver.cpp` `SelfSpecs` and
`tests/ClothValidationTest.cpp`. The evidence values change, so this is part of the m3a.8 bump.

**The other contact and self-collision cases** under the solver perturbations (old grid, old reversed, new RCM, new
grid; four records, two integrators): rest (identical to all digits), cylinder (identical), single-layer slide (1e-15
to 1e-12 on a 1e-6 gate: round-off), mesh and bounce (0), self-stack (9e-14 to 2.7e-12 on 1e-8), self-fold (0
violations). The single-layer **stick** case is bistable too (BDF2: 3e-13 or 1.18e-6 depending on the variant, backward
Euler 1.37e-7 throughout) but both branches are under its 1e-5 solver-tolerance gate by 8.5 times or more, so it was
not changed. The 1-ulp-nudge and thread perturbations were applied only to the incline replica, not to these.

## 14. Second half, liquid part (2026-11-27, at `d0256a0a4`, liquid.mac m7m.1, thin LTO)

Everything in `src/Library/Simulation/Liquid/`. All A-class: **no liquid byte moved, liquid.mac stays m7m.1**; the
B4 preconditioner was measured and not adopted (§14.4). Builds: a private copy of the tree (cloth sources frozen at
the state of 15:30, cloth.shell m3a.8) built with `make LTO=thin`; `base` is `d0256a0a4`'s liquid in that copy, `a1`
adds §14.2 (evidence), `a3` all of §14.2 and §14.3. The final check in the real checkout (then-default full LTO) is §14.6.
Machine shared throughout: load average 5 to 30, noted per table. Scratch, drivers, samples and bakes:
`/Volumes/SSD/claude-tmp/perf3-scratch/` (not in git).

**Byte identity, every item.** Reduced bakes of all eight committed liquid scenes (dam break 10 frames, honey and
water 6, rigid bodies 6, cloth flap staggered and one-way (the flap with `pool.sheet_load -> flap.surface_load`
removed) 24 macro steps, wave 24, catch 24, honey cloth 24), each at the default threads (16) and at `threads 3`
(scratch copies of the scenes): 16 bakes per binary, frame-name sets **and `manifest.cbor`** (which carries the
evidence) compared per member with the `base` bakes. `a1`, `a2` and `a3` (each step of this section) are identical in
all 16, with the evidence store off. With the store on, cold and warm, the `rise-sim status` text of seven scenes
(which carries the cache keys and so the evidence digests) is identical to `base`'s, and dam-break bakes with a warm
store have the same manifest as computed ones. Longer bakes (dam 46 frames, honey 12, rigid 12, wave, catch and honey
cloth 36 macro steps) of `a1` against `a3` with a warm store: frame sets identical (§14.3 table).

### 14.1 Profile (re-measured; `sample` of whole bakes, the step thread, plan-time evidence removed)

Shares of `LiquidModel::Step` on the step thread; inclusive shares overlap. Before = `base`, after = `a3` (`a2` for
the rows that §14.3's last change does not touch). Pool waits (the caller spinning or yielding for a straggler, load
dependent) are counted inside the phase that dispatched.

| Phase (function) | Dam break 48^3 (load 5 / 8) before / after | Rigid bodies 64^3 (load 24 / 6) | Catch 64^3 (load 14 / 24) | Wave 64^3 (load 10 / 7) | Honey cloth 40^3 (load 12 / 7) |
|---|---|---|---|---|---|
| Pressure `Project` (inside `ProjectBodies` / `ProjectCoupled`) | 33.3 / 34.6 % | 59.2 / 54.0 % | 18.7 / 21.7 % | 33.0 / 34.8 % | 10.3 / 11.0 % |
| ... `Multigrid::Precondition` | 22.8 / 24.0 % | 45.1 / 39.8 % | 9.1 / 10.3 % | 23.7 / 24.1 % | 4.6 / 5.1 % |
| ... ... `Restrict` | 4.4 / 4.8 % | **16.1 / 4.6 %** | 2.4 / 1.2 % | 5.0 / 2.1 % | 0.4 / 0.2 % |
| Velocity advection `LimitedAdvectionStage` | 19.1 / 20.4 % | 8.2 / 9.8 % | 13.1 / 19.1 % | 10.5 / 11.4 % | 7.9 / 5.1 % |
| Velocity extrapolation `ExtrapolateVelocity` | 20.4 / 16.3 % | 10.2 / 10.7 % | 16.4 / 18.6 % | 11.7 / 10.8 % | 7.7 / 5.3 % |
| `std::sort` of cell indices (the extrapolation's next front; in sheet scenes also `SweepSheetCells`) | 5.7 / 0 % | 2.4 / 0 % | 7.4 / 0.2 % | 5.0 / 0 % | 2.3 / 0.1 % |
| Level set `Reinitialize` (two calls) + `AdvectLevelSet` | 13.6 + 6.1 / 14.2 + 6.5 % | 4.0 + 2.8 / 5.1 + 3.2 % | 8.9 + 4.3 / 13.8 + 6.8 % | 7.9 + 3.4 / 8.4 + 4.3 % | 7.4 + 3.5 / 4.2 + 2.1 % |
| Marker particles `Advect` | 6.8 / 6.8 % | 2.5 / 2.7 % | 1.3 / 3.2 % | 2.0 / 2.0 % | 0.7 / 0.2 % |
| Bodies `BodyGeometry::Update` | | 8.5 / 9.6 % | | | |
| Sheets `CloseSheetFaces` (barrier and two-valued level set build) | | | **19.6 / 5.4 %** | 15.5 / 14.0 % (`a2`) | 4.9 / 3.9 % (`a2`) |
| Sheets `SweepSheetCells` | | | **8.8 / 1.1 %** | 6.4 / 0.9 % | 1.6 / 0.3 % |
| Sheet solve: `SheetDynamics::Prepare` (`BandedCholesky::Factor`) + `BandedCholesky::Solve` | | | 4.7 + 1.9 / 5.5 + 2.2 % | 5.1 + 0.6 / 5.1 + 1.3 % | 2.4 + 4.1 / 2.7 + 5.6 % |
| Viscosity `ViscousStep` | | | | | 54.1 / 64.4 % |

What changed since §5.2: the two-valued level set at sheets (m7l) made `CloseSheetFaces` a fifth of a coupled step,
almost all of it allocation (`std::vector` of trilinear weights per band cell, a `std::map` per recipe) and binary
searches of the band's slot list; with sheets and viscosity (m7m) the honey cloth's step is two thirds viscous solve.
The rigid-body scene's pressure solve was dominated by a serial restriction (below). The pool's own cost (`notify_one`,
`steady_clock::now`, the job mutex: 6 % of `Precondition` in the rigid scene, about 15 % of `ViscousStep` in the honey
cloth, contended `mutex::lock` 4 %) is in `Cloth/ClothParallel.cpp`, which this part does not own (§14.5).

### 14.2 2a, plan-time evidence: concurrent cases and the persisted store (`LiquidSolver.cpp`)

`ComputeEvidence`, `ComputeBodyEvidence` and `ComputePhysicsEvidence` now hand their cases (13, 8 and up to 8 runs)
to `RunEvidenceJobs`: up to `threads` of them at once, each on its own single-threaded pool, longest first; each run
writes only its own result variables and the error map, the case list and the digests are built afterwards exactly as
before. Then `MemoisedEvidence`: the process memo, then `LoadPersistedEvidence("liquid.mac", key, e)`, else compute and
`PersistEvidence`. The key (`EvidenceStoreKey`) holds the solver id, `SolverVersion`, which part (base, bodies,
physics with its viscosity and capillary flags) and **every** `SolverOptions` field the runs can read
(`EvidenceOptionsValue`: the numerics, the particle options, every `PressureOptions` scalar, the viscous and capillary
numerics), a superset of the `NumericsValue` the digests bind; the store adds the build identity. The runs do not read
the thread count (results are bitwise thread-independent, LiquidGrid.h), and the status text at 16 threads (cases on
pools of 16) equals the new one (cases on pools of 1). No `SimulationSolver.{h,cpp}` change was needed.

`rise-sim status`, wall seconds (load 5 to 12, two runs each; within 0.05 s of each other except honey 6.7 s once):

| Scene | `base` (store off) | `a3`, store off | `a3`, store cold | `a3`, store warm |
|---|---:|---:|---:|---:|
| `liquid_dam_break` | 2.54 to 2.58 | 1.45 | 1.45 | **0.05** |
| `liquid_honey_water` | 4.63 | 2.11 | 2.14 | **0.05** |
| `liquid_rigid_bodies` | 4.00 to 4.04 | 2.08 | 2.08 | **0.05** |
| `liquid_cloth_flap` | 2.66 | 1.57 | 1.58 | **0.05** |
| `liquid_cloth_wave` | 2.66 | 1.56 | 1.57 | **0.05** |
| `liquid_cloth_catch` | 2.68 to 2.79 | 1.58 to 1.60 | 1.62 | **0.06** |
| `liquid_honey_cloth` | 3.86 to 3.90 | 2.26 | 2.26 | **0.05** |

(The cold column includes the cloth evidence where there is a cloth member; with a warm store neither is computed.)
The concurrent cases are bounded by the longest single-threaded runs (the 64-cell added-mass cylinder, the solid
collapse); every bake, status and scene load of a liquid scene saves 1.1 to 2.5 s with the store off and all of it
(2.5 to 4.6 s) with it warm.

### 14.3 2b and 2c, the step (`LiquidFlow.cpp`, `LiquidModel.cpp`, `LiquidLevelSet.{h,cpp}`)

- **`Multigrid::Restrict`** ran on the pool only when the *coarse* level had 32768 cells or more, in chunks of
  16384 *coarse* cells: the restriction from the finest level of a 64-cell scene (coarse level 15,360 cells in the
  rigid scene) ran on one thread and was 16 % of the rigid scene's step, a third of `Precondition`. It now follows the
  fine level's size (pool from 32768 fine cells, chunks of about 16384 fine cells), and the per-axis prolongation
  lists it rebuilt (`std::vector` of vectors) on every call are built once per level in `Coarsen`. Each coarse cell is
  still written by one chunk with the same arithmetic. The other multigrid loops already ran serially below 32768
  cells (`PoolFor`) with row chunks of 16384 cells, so no other dispatch policy was changed (§14.5).
- **`ExtrapolateVelocity`**: the next front (the unknown neighbours of the faces just filled, sorted and unique) was
  a `std::sort` of six times the front; the front is sorted, so the neighbours along each of the six directions are
  too, and a six-way merge gives the same sorted set in one pass (2 to 6 % of a step).
- **`CloseSheetFaces` / `BuildBarrier` / `ExtensionRecipe`**: the trilinear weights in fixed arrays instead of three
  vectors per band cell, the recipe's merge by cell in a 24-entry array (each cell's sums in the same order from zero
  as the `std::map` did) instead of a map, and a dense cell-to-slot index (`LevelSetBarrier::Second::IndexSlots`,
  `slotOf`) behind `Second::Slot`, which every recipe, the barrier's stencil build and the sweep call per cell instead
  of a binary search. Catch: `CloseSheetFaces` 19.6 % of the step to 5.4 % (602 to 202 samples in back-to-back
  profiles of the same window).
- **`SweepSheetCells`**: the cells whose marker particles go were a sorted list with duplicates (every sheet face,
  two sides, 25 offsets) looked up by binary search per particle; now flags over the grid. Catch 8.8 % to 1.1 %.

Bakes with a warm evidence store (`a1` = evidence change only, against `a3`; so only the step differs), two runs each,
interleaved; wall / user CPU / instructions retired:

| Scene (window) | `a1` | `a3` | Wall | Frames |
|---|---|---|---:|---|
| `liquid_dam_break` (46 frames, load 10) | 8.84 to 8.86 s / 40.0 s / 766 G | 8.38 to 8.41 s / 39.7 s / 763 G | -5 % | identical |
| `liquid_honey_water` (12 frames, load 8) | 5.57 to 5.61 s / 12.0 s / 232 G | 5.49 s / 11.9 s / 231 G | -2 % | identical |
| `liquid_rigid_bodies` (12 frames, load 10 to 14) | 13.81 to 14.15 s / 76.3 s / 1,379 G | 12.81 to 12.93 s / 82.1 s / 1,475 G | **-8 %** | identical |
| `liquid_cloth_wave` (36 macro steps, load 11 to 12) | 5.75 to 5.84 s / 26.1 s / 471 G | 4.64 to 4.65 s / 25.2 s / 460 G | **-20 %** | identical |
| `liquid_cloth_catch` (36 macro steps, load 11 to 22) | 13.21 s / 63.6 s / 1,075 G (a 27.4 s outlier at load 18) | 12.38 to 13.49 s / 56.7 s / 936 G | -6 to -13 % (instructions) | identical |
| `liquid_honey_cloth` (36 macro steps, load 18 to 21) | 6.82 to 6.89 s / 41.6 s / 738 G | 5.96 to 6.24 s / 41.0 s / 732 G | **-11 %** | identical |

The rigid scene's user CPU and instructions rise by 7 % while its wall falls: the restriction now wakes the pool's
helpers, which spin between jobs. The whole-bake effect of §14.2 and §14.3 together (store off for `a3`, so the
evidence is computed concurrently; the default-thread verification bakes, load 10 to 17): dam break 10 frames 5.60 to 3.43 s,
honey and water 10.4 to 4.2 s, rigid bodies 8.1 to 5.3 s, flap 5.0 to 3.5 s, wave 6.2 to 4.2 s, catch 11.0 to 6.9 s,
honey cloth 7.4 to 5.3 s; instructions 21 to 35 % lower (most of it the evidence: its pools no longer spin).

### 14.4 B4, a cheaper pressure preconditioner: measured, not adopted (no byte change, no version bump)

The approved family ("two-level, fewer sweeps", the tolerance unchanged) was measured with an instrumented build of
`a2` (environment overrides of the level-0 and coarser smoothing counts, the coarsest level's sweeps, the number of
levels and the coarsest size; a counter of PCG iterations and solve seconds over every `Project` of the bake; warm
evidence store, so only the bake's solves are counted). Current: red-black Gauss-Seidel, 2 sweeps before and after each
coarse correction on every level, the coarsest level (at most 512 cells) 30 symmetric sweep pairs, levels down to 512
cells. Iterations / solves / solve seconds (the best of two runs) / share of the bake's wall:

| Variant | Dam break 46 frames (load 9 to 15) | Rigid bodies 12 frames (load 10 to 13) | Catch 36 macro steps (load 11 to 16) | Wave 36 (load 12 to 13; later rows at 18 to 22) | Honey cloth 36 (load 23 to 31) |
|---|---|---|---|---|---|
| current (2 / 2 / 30) | 9,160 / 874 (10.5 per solve) / 2.18 s / 29 % | 4,807 / 402 (12.0) / 4.76 s / 45 % | 4,105 / 291 (14.1) / 1.81 s / 17 % | 2,832 / 242 (11.7) / 1.30 s / 27 % | 2,508 / 215 (11.7) / 0.55 s / 6 % |
| 1 sweep on every level | 13,889 (16.2) / 2.71 s | 7,537 (18.7) / 5.78 s | 4,934 (17.0) / 1.73 s | 4,078 (16.9) / 1.43 s | 2,842 (13.2) / 0.48 s |
| 1 sweep on level 0, 2 below | 13,324 (15.5) / 2.66 s | 7,012 (17.4) / 5.89 s | 5,147 (17.7) / 1.85 s | 3,782 (15.6) / 1.40 s | 2,883 (13.4) / 0.69 s |
| coarsest 10 sweep pairs | 9,010 (10.5) / 2.03 s | 4,763 (11.8) / 4.71 s | 4,105 (14.1) / 1.71 s | 2,830 (11.7) / (1.99 s, load 18) | 2,508 (11.7) / 0.64 s |
| 1 sweep, coarsest 10 | 13,592 (16.1) / 2.46 s | 7,406 (18.4) / 5.65 s | 4,931 (16.9) / 1.73 s | 4,059 (16.8) / (1.71 s) | 2,842 / 0.65 s |
| 3 sweeps on every level | 7,519 (8.8) / 2.11 s | 4,001 (10.0) / 5.03 s | 4,020 (13.8) / 2.02 s | 2,410 (10.0) / (1.74 s) | 2,443 / 0.80 s |
| two levels (fine + one coarse, 30 pairs there) | 8,258 (9.6) / 2.65 s | 5,001 (12.4) / **11.92 s** | | | |
| three levels | 9,160 / 2.20 s | 4,579 / 4.87 s | | | |
| levels down to 4096 cells | 8,258 / 2.65 s | 4,579 / 4.86 s | | | |

Fewer sweeps raise the iteration count by 44 to 57 % (catch and honey cloth by 13 to 25 %) and cost more than they
save on the dam break, the rigid bodies and the wave; truncating the hierarchy (two or three levels) leaves a large
coarse problem that 30 sweep pairs do not solve, so the iterations do not fall and each one costs more (the rigid
scene's solve 2.5 times slower). The best variant (the coarsest level with 10 instead of 30 sweep pairs) saves 1 to
7 % of the solve time, 2 % of a step at most: far below the 15 % bar. **Not adopted**; the instrumented build was never
in the working tree. Because nothing changed, the convergence, symmetry and validation questions of the brief do not
arise: liquid.mac stays m7m.1, no pin changed, and the max-difference comparison was not needed. A real gain from the
preconditioner would need a different smoother or coarse operator (e.g. an operator-dependent restriction for cut cells
and bodies, which would also cut the 10 to 14 iterations per solve), outside the approved family.

### 14.5 What remains (candidates, by measured share)

| Candidate | Where | Measured share | Note |
|---|---|---|---|
| Sheet solve with `Cloth::EnvelopeCholesky` (task 4) | `LiquidSheets.cpp` `SheetDynamics::Prepare` (factor; its own `BandOrder`) and `Solve` / `Predict` (inside `ProjectCoupled` and the viscous solve) | catch 5.5 % factor + 2.2 % solve = 7.7 % of the step; wave 5.1 + 1.3 = 6.4 %; honey cloth 2.7 + 5.6 = 8.3 % | with the envelope factor 2.98 / 1.44 times cheaper (catch / wave mesh, §13.1) and the solve 2.5 times: about 5 % of a catch step, 2.4 % of a wave step; result-changing (other elimination order): a liquid version bump |
| Viscous solve | `LiquidViscosity.cpp` `ViscousStep` (`System::Gather`, `Strains`, `Build`, `Dot`) | 64 % of the honey cloth step | matrix-free CG over many short `ForRows` passes on a 40-cell grid; the pool's own cost is about 15 % of it (below); fewer, fused passes per iteration would keep the bytes if each face's sums keep their order |
| Pool overhead (not liquid's file) | `Cloth/ClothParallel.cpp` `For`: `steady_clock::now()` per job, `notify_one` per helper, the job mutex that waking workers contend | 6 % of `Precondition` (rigid), about 15 % of `ViscousStep` (honey cloth) | a per-job deadline only when the wait is long, and a lock-free job hand-off; bytes unchanged; for the cloth owner |
| Advection | `LimitedAdvectionStage`, `UpwindAdvectionRhs`, `OneSidedDerivatives` | 10 to 20 % | §5.3 L4 still open |
| Level set | `Reinitialize`, `AdvectLevelSet` | 7 to 20 % | §5.3 L4 (shares `OneSidedDerivatives`) |
| Extrapolation, the first front and the layer values | `ExtrapolateVelocity` | 10 to 19 % after the merge | §5.3 L5 |
| `ForRows` thresholds | `LiquidGrid.h` | pool waits 4 to 12 % of a step at load 6 to 24 | not changed: the effect could not be told from the load on a shared machine; any change keeps bytes (non-reduction loops only, reductions keep their chunking) |

### 14.6 Tests, warnings, final check (this part)

Real checkout, then-default full LTO: `mk.sh all tests build-tool/rise_sim build-tool/simulation_cache_import`,
**warning-free** (not even the Homebrew linker notices this time). `./run_all_tests.sh --tier fast --timeout 300
--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`: **453 of 453 pass** (the liquid suites included, so every liquid validation
gate and pin holds unchanged; no pin needed updating: the liquid tests pin no cloth version, and cloth m3a.8 was
already in the tree). Definitive byte check: the full-LTO `rise_sim` of the real checkout against the thin-LTO `base`
bakes of the private copy, all 16 reduced bakes (eight scenes, two thread counts, store off): frame sets and manifests
identical; with a warm store (rigid, catch, honey cloth, honey and water) as well. The cloth sources of the private
copy are byte-identical to the committed cloth.shell m3a.8 (`cc071c604`), so the coupled scenes compare like with like.

Files changed: `src/Library/Simulation/Liquid/LiquidSolver.cpp` (§14.2), `LiquidFlow.cpp` (`Restrict`, the front
merge), `LiquidModel.cpp` (`TrilinearWeights`, `ExtensionRecipe`, `BuildBarrier`, `SweepSheetCells`),
`LiquidLevelSet.{h,cpp}` (`Second::slotOf`, `IndexSlots`), this document. No new source files, no test changes, no
version change.
