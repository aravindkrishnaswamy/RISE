# Simulation performance: measurements and tuning opportunities (first half of the tuning pass)

2026-11-27. Branch `simulation-liquid` at `05eab7170` plus the working-tree changes listed in §8. Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md). Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (no contract change). Fire CPU timings of
earlier rounds: [SIMULATION_M8_RESULTS.md](SIMULATION_M8_RESULTS.md) §11.6 and §12.3.

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
   string. Saves the whole 1.3 to 1.7 s per process; risk: staleness when the solver changes without a version bump
   (it is the same hazard the cache key already has); bytes unchanged.

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
| A1 | Plan-time evidence concurrency and memoisation (cloth, coupling, liquid) | `ClothSolver.cpp`, `ClothRigidCoupling.cpp`, Liquid | cloth contact evidence: trampoline status 3.66 to 2.36 s; liquid 1.3 to 1.7 s; coupling 1.9 s | low; cases are independent and deterministic | cloth done; coupling and liquid proposed |
| A2 | Persist plan-time evidence across processes (hash of the key and the solver version) | the three evidence sites | removes up to 2.3 to 4.7 s per process | medium: stale entries after a solver change without a version bump | proposed |
| A3 | Hardware SHA-256 | `RISECBOR64.cpp` | 429 to 2,869 MB/s (6.7 times); frame verify 2.4 to 0.4 ms per MiB; every Append and Sample | low; checked against `shasum` on 20 sizes and incremental updates | done |
| A4 | ASCII fast path of `IsUTF8NFC` (every manifest key and digest) | `RISECBOR64.cpp` | cache writer: 2000 x 4 KiB frames 7.7 to 2.1 s; last-frame Append 7.0 to 1.4 ms | low; 2.3 million strings agree with the full path | done |
| A5 | Pool without syscalls (spin hint, bounded spin, wake only as many helpers as chunks) | `ClothParallel.cpp` | system time 37.9 to 2.2 s on a 16-thread liquid bake; wall neutral on a quiet machine and better on a loaded one | low; chunking and reduction order untouched | done |
| A6 | Moves instead of copies of `Value` trees in the manifest encode and the CBOR decoder | `RISECBOR64`, `EncodeManifest` | part of A4's remaining cost (about 40 %) | none | done |
| A7 | Per-stage path bounds computed once per vertex (`KinematicStep`): each vertex is in many candidate pairs | `ClothContact.cpp` | up to 30 % of the trampoline step (PathBound), expected 15 to 25 % wall | low: the same function of the same inputs | done, see §8.5 |
| A8 | Incremental or batched manifest publication: encode the new entry only and keep a running manifest, or publish every k frames and at seal | `SimulationCache.cpp` `Publish` | removes the O(n) per frame: last-frame Append 1.4 ms to about 0.7 ms at 2000 frames | medium: weakens "every committed frame is durable and in the manifest" unless the frame files are still individually durable and the resume path re-publishes; final manifest bytes unchanged | proposed |
| A9 | Cache hash grid with open addressing and a reused flat array instead of `unordered_map<uint64, vector>` rebuilt at every `Place` | `ClothContact.h/.cpp` | `Place*` and `Insert` are about 8 % of the trampoline | low; **the first attempt (a sort-based flat table) was slower** (the `std::sort` of tens of thousands of pairs and a binary search per cell cost more than the hash map; reverted); a counting-sort build or an open-addressed table is the way | tried and reverted |
| A10 | `BandedCholesky::Factor` loop structure (25.7 % of the trampoline): block the update loops, keep each row's inner product in the same order | `ClothLinearAlgebra.cpp` | up to 1.5 to 2 times on 26 % | **high for byte identity**: the build uses `-ffast-math`, so the compiler may already reassociate the reduction, and a restructured loop can vectorise differently; verify by bake comparison only | proposed |
| A11 | `SampledTrack::At` memo | `ClothModel.cpp` | 13.7 % fewer instructions but 1 % wall: the TLS lookup costs about what the sine and cosine do | n/a | tried and reverted |
| A12 | Skip the redundant digest of a frame that was just verified (render path reads a frame once per sample) | `SimulatedMeshSource`, `SimulationCache` | with A3 already down to 0.4 ms per MiB | low | not needed after A3 |
| A13 | Overlap hashing with the write of a frame file (hash on the solver thread, write and fsync on another) | `SimulationCache.cpp` | 30 x 16 MiB: 13 ms per frame of which about 6 is the hash | medium (ordering with the manifest) | proposed, small |
| A14 | Fire (not touched): see §7 | | | | |

**B. Would change results (not implemented).**

| Rank | Change | Why it would help | Why it changes bytes |
|---:|---|---|---|
| B1 | Fill-reducing ordering (reverse Cuthill-McKee or nested dissection) for the cloth `BandedCholesky` instead of the natural vertex order | Factor is 26 % of the trampoline | different elimination order: every solve differs in the last bits |
| B2 | Cloth: reuse a factorization across Newton iterations (modified Newton) or a looser `cg_tolerance` / `tolerance` | fewer factorizations | different iterates |
| B3 | Cloth: a coarser `PathBound` (fewer than 16 samples) or a looser safety factor in `KinematicStep` | the largest kinematic cost | different stage lengths, different contact sequence |
| B4 | Liquid: looser pressure tolerance, fewer V-cycle sweeps, a two-level preconditioner, single-precision coarse levels | `Precondition` is 50 to 70 % of `Project` | different pressures |
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
| make (`Config.OSX`) | `-O3 -flto` (full LTO), `-ffast-math -fno-finite-math-only`, `-funroll-loops`, default `-mcpu=apple-m1` | Already aggressive. No profile-guided optimisation; no `-mcpu=apple-m4` (the M4 has SME but the code is NEON; a newer `-mcpu` gains little for this code and ties the binary to M4). `-ffast-math` is on for the whole library except the fire production sources, which compile with `-fno-fast-math -ffp-contract=off` |
| Xcode, Deployment configuration, targets `rise` and `rise-sim` | `GCC_OPTIMIZATION_LEVEL = 3`, `LLVM_LTO = YES_THIN` (thin, not full), `-fno-finite-math-only` but **no `-ffast-math`**, no `-funroll-loops` | The Deployment build is configured differently from the make build the goldens and the test hashes were recorded with: no fast-math means different floating-point contraction and reassociation in the cloth and liquid solvers, so **a Deployment `rise-sim` may write different bytes than the make `rise_sim` for the same scene** (the `RISE_FP_CONTRACTION_MODE = compiler_default` setting of the project is the only related knob). The `Opto` configuration does have `-ffast-math` and full LTO. This should be decided and tested (bake a scene with each and compare frame sets) before the app's baked caches are trusted against the test-suite goldens. Thin LTO also leaves inlining across modules on the table; the make build's full LTO is what produced the numbers here |
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
