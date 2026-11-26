# M5a results: the bake manager, its C API and the agent verbs

2026-11-23. The library half of M5 ([plan](SIMULATION_IN_RISE_PROPOSAL.md) §4.2, §4.5, §5 M5). Builds on
[M4](SIMULATION_M4_RESULTS.md) (scene simulations, `rise-sim bake <scene>`), [M3c](SIMULATION_M3C_RESULTS.md)
(playback clock, live API) and [M2](SIMULATION_M2_RESULTS.md) (worker sidecars, exit codes). The solver
contract is unchanged; the host-side additions are recorded in its Appendix C. The GUI panels (macOS and
Windows) are M5b and use the calls in §5.

## 1. What a bake is

A bake is a separate **`rise-sim` process** run on the scene **file**, one process per simulation:

```
rise-sim bake <scene.RISEscene> <simulation> --no-upstream [--force] [--gpu-lock PATH]
         [--checkpoint-seconds S] [--expect-key <simulation>=<key>]
rise-sim resume <scene.RISEscene> <simulation> [--gpu-lock PATH] [--expect-key ...]
```

- It survives the app quitting: it runs in its own session (`POSIX_SPAWN_SETSID`), with stdin from
  `/dev/null` and stdout/stderr appended to a job log.
- It never touches the loaded scene. The app sees its output through the cache, as any partial cache.
- The **bake manager** (`Simulation/SimulationBakeManager.{h,cpp}`) queues, starts, monitors, stops and
  adopts these processes.
- Platform pieces live in `Simulation/SimulationProcess.{h,cpp}`: spawn, reap, liveness with a start
  stamp, signals, an advisory file lock, executable path and per-user directories.

## 2. Design decisions

**Ownership: one process-wide manager.** `SimulationBakeManager::Instance()` is shared by every open
document and every agent session, with per-document use. This is the render coordinator's shape
([RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md) §2.1):

- The resource rules are process-wide and machine-wide (one GPU, a CPU cap, a memory budget). Managers
  owned per document would each have to coordinate with the others.
- Headless agent sessions have no controller, and they must bake too.
- A document talks to the manager through a `BakeSceneSnapshot`: its scene file, whether it has unsaved
  changes, and its simulations' `SceneSimulationStatus` in dependency order. Jobs are keyed by
  (canonical scene path, simulation).
- Tests construct private managers (two managers contending; destroy and recreate).

**Save rule: a bake needs a saved scene, and the file must plan the key the document shows.** The worker
bakes the file. Snapshotting the scene to a temporary file was rejected for three reasons:

- relative `cache` paths and relative scene assets resolve against the scene's directory;
- the cache's key would then belong to a scene nobody can reopen;
- a temporary copy of an unsaved document invites "which version did I bake?".

The rule is enforced in two places:

1. **At request time** (`RequestBake`, `Resume`):
   - A document with no file is refused: "save the scene first: …".
   - With unsaved changes, a simulation whose key cannot be computed yet is refused at once, with "save
     first". This covers a downstream whose upstream is not baked, and a fire with no stated window.
2. **In the worker.** Every simulation whose key the document knows is baked with `--expect-key
   name=key`. The worker plans the file and checks the key before it touches the cache. A different key
   fails with `scene mismatch: …; save the scene first`, and the job's error reads `save first: …`.
   - This check has no window between check and use: it runs on exactly what is about to be baked.
   - It also catches a file changed on disk after the document loaded.
   - An edit that does not change the key (a camera move, say) still bakes.

"Unsaved changes" means the document differs from the file:

- **GUI:** the controller's dirty trackers, confirmed by comparing `SerializeCst` with the file's bytes.
- **Headless:** the byte comparison alone.

**Lock protocol: one GPU solver at a time.**

- **The worker** classifies `Capabilities::backend`: stated and not `cpu` means the GPU (`IsGpuBackend`).
  Such a worker takes an exclusive advisory lock (`flock`; `LockFileEx` on Windows) on the GPU lock file
  before `Start`/`Restore`, and holds it until the process exits.
  - It takes the lock after `Configure`, because the backend is only known then. Fire's `Configure` runs
    a brief Metal capability check before the lock.
  - It takes the lock after the cache writer lock, so the cache shows as `baking` while it waits.
  - While another process holds the lock, the worker:
    - writes status `waiting-gpu`, with detail `waiting for the GPU (backend X; lock PATH, held by pid N)`;
    - polls every 200 ms;
    - honours SIGINT and the stop file. It ends `stopped`, exit 3; there is nothing to checkpoint.
  - The holder writes `<pid> <cache>` into the lock file for that message.
- **The lock path** is `--gpu-lock PATH`, else `$RISE_SIM_GPU_LOCK`, else the per-user default:
  - macOS: `~/Library/Caches/RISE/rise-sim.gpu.lock`
  - Linux: `$XDG_CACHE_HOME` (or `~/.cache`) `/rise/rise-sim.gpu.lock`
  - Windows: `%LOCALAPPDATA%\RISE\rise-sim.gpu.lock`

  Missing directories are created 0700 and the file 0600.
- **Who cooperates:** every app instance, agent session and command-line bake of this user, as long as it
  runs a `rise-sim` that has this change. The manager passes `--gpu-lock` only when it is configured
  (tests); otherwise each worker uses its own default.
- **The manager** also runs at most one GPU-class job at a time from its own queue. GPU-class means a GPU
  backend, or an unstated one: the manager treats unknown as exclusive. Fire's predicted plan states no
  backend, so the manager serializes it, and its worker still takes the lock. The second such job stays
  `queued` with `waitingFor` "the GPU slot …".

**CPU cap and memory budget.**

- At most `maxCpuBakes` bakes (default 2) run on a CPU backend.
- The declared `Capabilities::memoryEstimate` of running bakes must fit the budget (default: half the
  physical memory). A job whose own estimate is over the budget fails, saying so. One that does not fit
  next to the running bakes waits ("memory …").
- An estimate of 0 (unknown, which is cloth's and fire's today) is not counted.

**Monitoring.**

- One monitor thread per manager wakes every `pollMilliseconds` (250 by default), or at once on a request.
- It reaps children (`waitpid WNOHANG`) and reads each live job's `rise-sim.status.cbor`, a small record.
  A record is accepted only when its `pid` is the job's; that excludes the previous bake's record before
  the new worker writes its first.
- It reports:
  - the state (§3);
  - simulated time and end time;
  - frames written and total;
  - wall time;
  - the ETA: remaining simulated time × observed wall time per simulated second since the first
    observation;
  - the last error: the status record's, else the last `rise-sim:` line of the job log;
  - the pid.
- There is no busy loop: the monitor waits on a condition variable between passes.

**Adoption.** `Attach(snapshot)` runs at scene load, on the first job-list call, on every
`GetSimulationCount`, `sim_list` and request. For each cache with no active job it reads the status record.

- **A live worker is adopted** only when all three hold:
  - the record's `pid` is alive with the same `pid_start` stamp (macOS `proc_pidinfo` start time; Linux
    `/proc/<pid>/stat` field 22), so a recycled pid has another stamp;
  - `host` matches (or is empty);
  - the cache's writer lock is held.
  It becomes a `running` or `waiting-gpu` job with `adopted: true`, followed to the end and stoppable.
- **A record that says running when nothing runs** becomes a `stopped` job with `interrupted: true`, and
  `resumable` when the cache is partial with the current key.
- **A cleanly stopped record** (`stopped`) on a partial cache becomes a resumable `stopped` row.
- **An older `rise-sim` that records no pid but holds the lock** (the M2 binary) is shown as `running`
  with `stoppable: false`. The manager can follow it, but it has no verified process to signal.
- Adopted children of this process (a destroyed and recreated manager) are reaped through `waitpid`, so
  they never linger as zombies.

**Stop, resume, cancel.**

- `Stop(job)` on a queued job cancels it; dependents are cancelled on the next pass with "upstream X did
  not finish".
- On a running job it writes `<cache>/rise-sim.stop`, plus SIGINT on POSIX. The worker checkpoints at its
  next safe point and exits 3; the job ends `stopped` with `resumable: true`.
- While a job is `stopping`, the manager re-asserts the stop file every pass. A worker that is still
  starting removes a stale stop file before it begins.
- `Stop(job, kill=true)` sends SIGKILL (`TerminateProcess` on Windows). It is used only on an explicit
  request: there is no checkpoint at the stop, and a resume continues from the last periodic checkpoint.
- `Resume` runs `rise-sim resume <scene> <simulation>` for a partial or failed cache of the current key.
  `RequestBake` on such a cache resumes it too: the worker's scene bake continues a partial cache of its key.
- `CancelQueued(scene)` cancels every queued job of a scene.

**Change notification.**

- Every change bumps `ChangeCounter()` (the GUI's ~2 Hz poll) and calls listeners with
  `BakeEvent{JobChanged}`. When frames land in a scene's cache, `FramesLanded` follows and
  `FramesGeneration(scene)` is bumped.
- `SceneEditController::RefreshSimulationFrames()` turns that into the viewport refresh. When the
  generation moved since its last call, it does the following under the render park, the time-scrub
  protocol:
  - rebinds every `simulated_mesh_geometry` bound by `source` whose consumer was unbound (no cache at
    load), or whose cache started a new epoch. That is a rebake: the old frames are gone, so the consumer
    is unbound and hidden until the new epoch has a frame;
  - installs the current time's frames (`InstallSimulatedMeshesAtTime`), the install that scrubbing and
    playback do;
  - kicks a pass.
- A partial cache of the same epoch needs no rebind: each `Sample` re-reads its manifest. The baked-range
  query `GetTimeVaryingAssets` already re-reads it too.
- Geometries with a live simulation are left alone.
- A changed topology cannot be rebound. It is reported ("reload the scene").

## 3. Job states

| State | `outState` | Meaning |
|---|---|---|
| queued | 0 | Not started. `waitingFor` says why: "upstream simulation X", "a CPU bake slot (n of m running)", "the GPU slot (…)", "memory (…)" |
| waiting-gpu | 1 | The worker runs and waits for the machine-wide GPU lock |
| running | 2 | Baking |
| stopping | 3 | Stop requested; it checkpoints at its next safe point |
| stopped | 4 | At a checkpoint (`resumable`), or `interrupted` (killed, crashed, machine restarted) |
| finished | 5 | Sealed (or already up to date: `detail` "up to date", no process) |
| failed | 6 | `lastError` says why (worker error, "save first: …", over the memory budget, worker not found) |
| cancelled | 7 | Removed from the queue, or its upstream did not finish |

A request for a simulation that has an active job returns that job: there are no duplicate bakes. The
manager keeps the last 256 finished jobs.

## 4. File and process layout

| What | Where |
|---|---|
| Cache, worker sidecars | the simulation's `cache` directory (M1/M2): `manifest.cbor`, `*.frame`, `writer.lock`, `rise-sim.job.cbor`, `rise-sim.checkpoint`, `rise-sim.status.cbor` (+ optional `pid`, `pid_start`, `host`; state `waiting-gpu`) |
| Stop request | `<cache>/rise-sim.stop`, honoured on every platform. The worker polls it 4×/s, and also while waiting for the GPU |
| GPU lock | per user (§2) or `--gpu-lock` |
| Job logs | `<state dir>/logs/<time>-<app pid>-<job>-<simulation>.log`; state dir default `~/Library/Caches/RISE/bakes` (macOS), `~/.cache/rise/bakes` (Linux), `%LOCALAPPDATA%\RISE\bakes` (Windows). The first line is the command run |
| Worker executable | search order: an override (`RISE_API_SimulationBake_SetWorkerPath`), then `$RISE_SIM_WORKER`, then beside the running executable (`rise-sim`, `rise_sim`, `tools/rise_sim`, `../tools/rise_sim`, `../Helpers/rise-sim`, `../Resources/rise-sim`; `.exe` on Windows), then `$PATH`. An explicit override that is not executable is an error, not a fall back |

The make tree already satisfies the search: `bin/rise` finds `bin/tools/rise_sim`, and tests in `bin/tests/`
find `../tools/rise_sim`. `make rise` builds `rise_sim` (order-only prerequisite).

## 5. API for the GUI layers

All calls are in `RISE_API.h` (namespace `RISE`). Every out pointer may be null, text is truncated to its
buffer, and exceptions never cross the ABI.

```cpp
// Process-wide
bool RISE_API_SimulationBake_SetWorkerPath( const char* path );                 // null/"" = search
bool RISE_API_SimulationBake_GetWorkerPath( char* pathBuf, unsigned int pathBufLen,
	char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SimulationBake_SetLimits( unsigned int maxConcurrentCpuBakes,       // 0 keeps the value
	unsigned long long memoryBudgetBytes );                                       // 0 = half of RAM
unsigned long long RISE_API_SimulationBake_ChangeCounter();
typedef void ( *RISE_SimulationBakeChangeCallback )( void* userData, unsigned long long changeCounter );
bool RISE_API_SimulationBake_SetChangeCallback( RISE_SimulationBakeChangeCallback callback, void* userData );

// Per document (controller)
bool RISE_API_SceneEditController_SimulationBake( SceneEditController* p, const char* simulationName,
	bool force, unsigned long long* outJobId, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_SimulationBakeAll( SceneEditController* p, bool staleOnly, bool force,
	unsigned int* outJobCount, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_SimulationBakeResume( SceneEditController* p, const char* simulationName,
	unsigned long long* outJobId, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_SimulationBakeStop( SceneEditController* p, const char* simulationName,
	bool kill, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_SimulationBakeCancelQueued( SceneEditController* p, unsigned int* outCancelled );
bool RISE_API_SceneEditController_GetSimulationBakeJobCount( SceneEditController* p, unsigned int* outCount );
bool RISE_API_SceneEditController_GetSimulationBakeJob( SceneEditController* p, unsigned int index,
	unsigned long long* outJobId, char* simulationBuf, unsigned int simulationBufLen,
	int* outState, bool* outResumable, bool* outAdopted, bool* outInterrupted,
	double* outSimulatedTime, double* outEndTime,
	unsigned long long* outFramesWritten, unsigned long long* outFramesTotal,
	double* outWallSeconds, double* outEtaSeconds, unsigned long long* outPid,
	char* backendBuf, unsigned int backendBufLen, char* waitingForBuf, unsigned int waitingForBufLen,
	char* detailBuf, unsigned int detailBufLen, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_RefreshSimulationFrames( SceneEditController* p, bool* outRefreshed,
	char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_LiveSimulationDetachSimulation( SceneEditController* p,
	const char* simulationName, char* errorBuf, unsigned int errorBufLen );
```

**A Simulations panel, call by call.**

1. **At startup:** `RISE_API_RegisterBuiltinSolvers` (M3c). A bundle that ships the worker elsewhere calls
   `SimulationBake_SetWorkerPath` once; show `GetWorkerPath`'s error when it fails ("rise-sim was not
   found …").
2. **Rows: the simulations** (M4).
   - `GetSimulationCount` / `GetSimulation(index, …)`: name, keyword, state (0 missing … 5 failed),
     upstream out of date, interactive, frames, cache, error.
   - Since M5, `GetSimulationCount` also adopts bakes still running from an earlier session. Poll at
     ~2 Hz, or when the change counter moves.
3. **Per-row bake state.**
   - `GetSimulationBakeJobCount` / `GetSimulationBakeJob(i, …)`. A row's job is the newest job with that
     simulation name.
   - Progress is `framesWritten / framesTotal` (or `simulatedTime / endTime`), with `etaSeconds` (-1 =
     unknown).
   - The state badge comes from `outState` (§3). Show `waitingFor` for queued jobs, `detail` for
     waiting-gpu and running ones, the error for failed ones.
   - The first call for a scene adopts its running bakes.
4. **Buttons.**
   - Bake: `SimulationBake(name, force=false)`. Rebake is `force=true`.
   - Bake all stale: `SimulationBakeAll(staleOnly=true, force=false)`.
   - Stop: `SimulationBakeStop(name, kill=false)`. Offer "force stop" (`kill=true`) only after a stop has
     not ended the job.
   - Resume: shown when `outResumable`; `SimulationBakeResume(name)`.
   - Cancel queue: `SimulationBakeCancelQueued`.
   - A false return carries the message in `errorBuf`: "save the scene first: …", "nothing to resume …",
     "`x` is already running".
5. **Queue view.** The job list is in queue order (job id). Queued rows show `waitingFor`.
6. **Refresh.** Poll `SimulationBake_ChangeCounter()` at ~2 Hz, or use `SetChangeCallback`, which runs on the
   manager's thread: hop to the UI thread. When the counter moves:
   - re-read the rows;
   - call `RefreshSimulationFrames(p, &refreshed, …)`. The viewport then shows newly landed frames, and a
     first bake makes an unbound mesh appear;
   - re-read `GetTimeVaryingAssetCount/Asset` for the timeline's baked ranges (M3c).
7. **Save rule in the UI.** Disable Bake for a never-saved document, and say why. A failed job whose
   error starts with `save first:` wants a Save, then Bake again.
8. **Live play** (M3c/M4): `LiveSimulationAttachSimulation(name)`, `PlaybackPlay`, and
   `LiveSimulationDetachSimulation(name)` (new) to stop.

`outState` values and the job fields are stable: the macOS GUI work reads this section.

**C++ (for the bridges or tests).**

- `SimulationBakeManager::Instance()`: `RequestBake`, `Resume`, `Stop`, `StopSimulation`, `CancelQueued`,
  `Jobs(scene)`, `Job`, `LatestJob`, `Wait`, `ChangeCounter`, `FramesGeneration`, `AddListener`,
  `RemoveListener`, `Attach`, `Settings`/`SetSettings`, `SetWorkerOverride`, `ResolveWorker`,
  `LocateWorker`.
- `SceneEditController::GetSimulationBakeSnapshot`, `RefreshSimulationFrames`, `SimulationBakeSceneKey`,
  `LiveSimulationDetachSimulation`.

## 6. Agent verbs

They are exposed through `AgentRpc` and the MCP adapter (`tools/list`: 51 tools). The full shapes are in
[MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md) §4.4a, and the code is `Agent/AgentSimulation.{h,cpp}`.

- **Read-safe:** `sim_list`, `sim_status` and `sim_wait` are on `IsReadSafeVerb`.
- **Commit autonomy and the owner's session:** `sim_bake`, `sim_cancel`, `sim_live_start`,
  `sim_live_stop` and `sim_seek`. They start or stop processes or drive the viewport; an External session
  is refused.

```jsonc
→ {"method":"sim_list","params":{}}
← {"ok":true,"scene":"/…/flag.RISEscene","unsavedChanges":false,"liveSupported":false,
   "simulations":[{"name":"flag","state":"missing","keyStatus":"missing","solver":"cloth.shell",
     "backend":"cpu","framesTotal":6,"cacheDir":"/…/caches/flag",…}]}
→ {"method":"sim_bake","params":{"simulation":"flag"}}
← {"ok":true,"jobId":13,"jobIds":[13],"jobs":[{"jobId":13,"state":"queued",…}]}
→ {"method":"sim_wait","params":{"jobId":13,"timeoutMs":10000}}
← {"completed":true,"found":true,"job":{"state":"finished","framesWritten":6,"progress":1,…}}
→ {"method":"sim_cancel","params":{"jobId":14}}          // mid-bake
← {"cancelled":true,"found":true,"job":{"state":"stopping",…}}
→ {"method":"sim_bake","params":{"simulation":"slow"}}   // continues the partial cache
→ {"method":"sim_bake","params":{"all":true,"staleOnly":true}}
→ {"method":"sim_live_start","params":{"simulation":"flag"}}   // headless
← {"ok":false,"supported":false,"message":"sim_live_start needs a live in-app viewport; …"}
```

**Headless** (`rise --agent-stdio` / `--mcp`):

- Baking works. The worker is found as `bin/tools/rise_sim` beside `bin/rise`.
- The default read posture allows only the read verbs: run with `--agent-autonomy=commit` to bake.
- The live verbs report `supported:false`.

The bake verbs were deliberately not added to the in-app chat catalog (`AgentChatCodecs.cpp`): that tool
set is curated for scene building and pinned by fingerprint tests.

## 7. Tests (`tests/SimulationBakeManagerTest.cpp`)

The test is CPU only and uses cloth.shell and the reference solvers. Every bake is a real separate worker
process, started through the manager's own spawn path:

- The test program re-runs itself as `rise-sim` (`--rise-sim-worker`): the library `WorkerMain` with the
  built-in and reference solvers registered, the same entry point as `tools/rise_sim.cpp`.
- That registration makes the fake-GPU solver available. It also means the test does not depend on
  `bin/tools/rise_sim` being rebuilt.
- Every pid the test sees is SIGKILLed and reaped at the end, and from an `atexit` handler.

| Section | Checks |
|---|---|
| L | an override that does not exist is an error, not a fall back; the search reaches `bin/tools/rise_sim` from `bin/tests`; a configured command wins |
| Q | a bake of `lower` queues `[upper, lower]`, and `lower` depends on `upper`; while `upper` bakes, `lower` is queued with "upstream simulation upper"; the listener sees `lower running` only after `upper finished`; both caches are complete; an up-to-date request starts no process |
| G | three worker processes of `test.fake_gpu` (the advection-diffusion reference solver declaring backend `test-gpu`) share a private lock: worker two reports `waiting-gpu` naming worker one's pid; a second manager adopts all three (one running, two waiting); a stop while waiting ends `stopped`; one and two are never running at once and both caches seal. Then two cloth sims declared GPU in the snapshot: this manager keeps the second queued for "the GPU slot" |
| S | a 12-frame paced cloth: running with pid, ETA and end time; listener, frames generation and change counter fire while frames land; stop leads to stopped (exit 3), resumable, partial; resume leads to finished; the sealed cache equals an uninterrupted bake of the same scene **byte for byte** (manifest, frames, statics) |
| A | the manager object is destroyed while its worker runs; the worker lives on; a new manager adopts it (same pid), a second attach adds no row, it is followed to the end, sealed |
| P | a stopped partial cache with a forged `running` record: (1) a live pid with the wrong start stamp, (2) a pid that has exited, (3) a live pid with the right stamp that does not hold the cache lock. None is adopted; each is `stopped`, `interrupted`, `resumable`, pid 0, and cannot be stopped. A resume finishes it |
| F | no file is refused at once; an unsaved edit of `substeps` (the key changes) is queued, and the worker refuses it with `save first: scene mismatch …`, no cache created; the clean document bakes; unsaved changes plus a downstream whose key is not known yet (upstream unbaked) are refused at once, nothing started |
| V | over `AgentRpcDispatcher` (commit), headless: `sim_list` (missing), `sim_bake`, `sim_wait`, `sim_status`, then `sim_list` (complete, key current, job finished); a paced bake: `sim_wait` with `timeoutMs` -5 clamps to a poll, `sim_cancel` mid-bake gives stopped and resumable, `sim_list` partial, `sim_bake` resumes it to finished and complete; param errors; unknown simulation gives ok:false; `sim_live_start` / `sim_seek` give supported:false; `sim_status` lists the jobs; under read autonomy `sim_list` works and `sim_bake` is refused (-32011) |
| R | a live `SceneEditController` over the interactive pipeline, through the C API: an unbound consumer; refresh with nothing new does nothing; `SimulationBake`; the job row shows frames landing; the change counter moves; `RefreshSimulationFrames` binds and shows the consumer, then does nothing until more frames land; finished (state 5, 10 frames); a forced rebake is rebound to the new epoch; stop and resume through the C API; `GetWorkerPath` |

**Result:** 118 checks pass in about 20–25 s. Three repeated runs were stable, and no worker processes
were left behind.

**Updated pins:**

- `AgentMcpAdapterTest`: tools/list 43 → 51, the new names, a `sim_wait` / `sim_bake` schema spot-check.
- `AgentAutonomyPolicyTest`: 43 → 51 under each posture; read-annotated 23 → 28 (the five control verbs).
- `AgentRpc.h`: the read-safe restatements (checked by `SourceHygieneTest`).

**Updated pins (cont.):** `AgentMcpStdioSmokeTest`: tools/list 43 → 51.

**Fast tier (non-fire)** (`--tier fast --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 421 built with no
warnings; 420 passed. The one failure was `AgentMcpStdioSmokeTest`'s tool-count pin. After updating it,
that test and the four agent and manager tests pass on a rerun.

**Manual check with the real `bin/tools/rise_sim`.** The scene was a cloth scene in a trusted private
directory:

- `rise --agent-stdio --agent-autonomy=commit` ran `sim_bake`, then `sim_wait`, then `sim_list`;
- the bake finished with 8 frames, backend `cpu`, key `current`, state `complete`;
- no worker was left running.

This check found that the headless CLI never registered the solvers, so its plan read the simulation as
`failed` with no key. `AgentSimulationSnapshot` now registers the built-in solvers once.

## 8. Packaging (what is left)

- **macOS app bundle.** The worker must ship in the bundle. Recommended: `Contents/MacOS/rise-sim`, which
  is found first. `Contents/Helpers/rise-sim` and `Contents/Resources/rise-sim` are also searched.
  - It must be signed with the app's identity and hardened runtime.
  - It needs the same OpenVDB/Metal dylibs the app links (fire).
  - A sandboxed app would need the worker as an embedded helper with an inherited sandbox; the current
    app is not sandboxed.
  - The Xcode project lists the new library sources. It does **not** yet build a `rise-sim` target or copy
    it into the bundle: that is M5b's packaging step. Until then, set the path with
    `RISE_API_SimulationBake_SetWorkerPath` (or `RISE_SIM_WORKER`) to a make-built `bin/tools/rise_sim`.
- **Windows.** Ship `rise-sim.exe` beside `RISE-GUI.exe`. There is no VS2022 project for the `rise_sim`
  tool yet.
- **Packaging scripts** were not changed.

## 9. Windows status

Written, **not compiled or tested** here. All of it is behind `#ifdef _WIN32` in `SimulationProcess.cpp`.

| Piece | Implementation |
|---|---|
| Spawn | `CreateProcessW` (detached, new process group, log handles inherited) |
| Liveness and stamp | `GetProcessTimes` creation time |
| Exit | `GetExitCodeProcess` |
| Kill | `TerminateProcess` |
| Locks | `LockFileEx` |
| Stop | the stop file (there is no SIGINT): the worker's watcher polls it on every platform |

## 10. Limits, and what was not done or verified

- **No fire bake through the manager.** No fire or GPU solver was run (the long GPU bake owns the GPU). The
  GPU path was exercised with a fake-GPU reference solver and a private lock file. The real per-user lock
  was never taken by a test.
- **The running M2 fire bake.** `rise_sim_m2` is an old binary: it takes no GPU lock and records no pid. A
  manager that loads its scene shows it as running and not stoppable. It does count against that manager's
  own GPU slot, so the manager starts no GPU-class bake beside it. A bake started from another process does
  not see it: new bakes take the lock, so they cooperate only with each other.
- **Lock scope is per user.** It is one lock per user, not across users. A process that does not take the
  lock (an older binary, another tool) is not excluded.
- **Fire consumers.** `RefreshSimulationFrames` rebinds simulated meshes only. A `fire_medium` bound to a
  cache that did not exist at load stays an empty medium until the scene is reloaded. A same-epoch partial
  fire cache already plays as it grows.
- **Topology.** A rebake that changes a mesh's topology needs a reload; the refresh reports it.
- **Memory estimates.** Cloth and fire declare none (0), so the memory budget has nothing to count yet.
- **Status read under the manager lock.** The monitor reads the small status records while holding the
  manager's lock. That is fine at the default cadence; a slow network cache would stall `Jobs()` callers
  for that read.
- **Not done.**
  - A process-wide queue view across documents (`Jobs("")` exists in C++; there is no C call).
  - A log viewer call (the log path is in the C++ job, and in the agent's `job.log`).
  - Clearing old finished rows.
- **Not verified.**
  - The Windows code path.
  - The Xcode and VS2022 project edits are not built. The new library files were added to all five build
    lists; the Xcode project already listed two of them (added by the concurrent GUI work).
  - The Android CMake list is not built.
  - No ThreadSanitizer run.
