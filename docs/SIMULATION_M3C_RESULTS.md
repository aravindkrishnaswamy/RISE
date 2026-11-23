# M3c results: simulated deforming geometry in the interactive viewport

2026-11-23. Design: [SIMULATION_M3_DESIGN.md](SIMULATION_M3_DESIGN.md) §6 (step M3c). Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §2 decision 3, §4.2, §4.5. What it builds on:
[SIMULATION_M3B_RESULTS.md](SIMULATION_M3B_RESULTS.md).

**Status.** The library and controller side is done:
- A baked `simulated_mesh_geometry` plays and scrubs in the interactive viewport. Each displayed time installs its
  cache frame under the render park.
- The timeline appears for scenes whose only time-varying content is simulation assets.
- A library playback clock plays scene time at a frame rate.
- An in-process simulation (cloth.shell, interactive mode) can be attached to a simulated mesh. It plays live, with
  the clock never ahead of the latest published frame.

All of it is in the C API. No Swift or Qt code changed: the timeline appears through the existing `GetHasAnimation`
query. The UI is M5 (§6). The scene-file binding of a producer to a consumer (`cloth_simulation`,
`source name.output`) is M4. Live attachment here is API-only.

## 1. What was built

| Piece | Where | What |
|---|---|---|
| Live source | `Simulation/SimulatedMeshSource.{h,cpp}` | `MeshCacheSource::OpenLive(provider, options)` binds to any `IReadOnlyCacheSnapshot` through a provider (an `InProcessHost`'s `MemoryPublisher`). Disk and live share one code path: the bound snapshot became a `shared_ptr<const IReadOnlyCacheSnapshot>`. `CurrentSnapshot`, `CheapSnapshot` (the generation when getting it costs no I/O), `Live`, `Root` |
| Rebind and prepare memo | `Geometry/SimulatedMeshGeometry.{h,cpp}` | `RebindSource(source)` swaps the frame source (same topology only, never under the freeze). `PrepareAssetForRender` skips the payload read and decode when the last prepare had the same support and the same immutable generation |
| Viewport stills at the nominal time | `Job.cpp` (`FireSequencePreparationGroup::InstallAssetsAtNominal`) | The editor's viewport (`RasterizeExternalStillAtTime`) installs simulated meshes with support {t, t, t}. Fire media keep the full shutter support |
| Timeline ranges | `Simulation/SimulationTimeline.{h,cpp}`, `IJobPriv::DescribeTimeVaryingAssets` (appended at the end of the virtual tail), `Job::DescribeTimeVaryingAssets` | `TimelineRange` per asset: name, kind (mesh / fire), state (complete / partial / baking / live), fidelity, start, end, available end, interval, frames available / total. `MeshFrameSceneTime`, `LatestContiguousMeshFrame` |
| Playback clock, live sessions, install helper | `SceneEditor/LiveSimulation.{h,cpp}` (new) | `PlaybackClock`, `LiveSimulation`, `InstallSimulatedMeshesAtTime` |
| Controller | `SceneEditor/SceneEditController.{h,cpp}` | Install in the scrub park; clamp to live frames; completed-pass counter; `GetHasAnimation` / `GetAnimationOptions` see simulation assets; playback and live API; teardown in `Stop()` |
| Built-in solvers | `Simulation/BuiltinSolvers.{h,cpp}` (new) | `RegisterBuiltinSolvers()` registers cloth.shell and fire.methane. The registry (`SimulationSolver.cpp`) is unchanged and depends on no solver or backend |
| Parameters | `Simulation/SimulationWorker.{h,cpp}` | `ParseSolverParameters(bytes)`: the JSON / CBOR reading `rise-sim bake` already did, now public |
| C API | `RISE_API.{h,cpp}` | 16 new functions (§5). No existing signature changed |
| Tests | `tests/SimulationViewportTest.cpp` (new) | §4 |
| Build lists | make `Filelist`, Android `rise_sources.cmake`, VS2022 vcxproj and filters, Xcode | Three new `.cpp` files and three new `.h` files. The make build compiles them; the other three lists were edited by hand and not built |

**Fire.** Fire media take the same viewport path as before (`RasterizeExternalStillAtTime`, full shutter support).
Two things change, both by design:
- A fire-only scene now has a timeline.
- Without an `animation_options` chunk, the timeline's range comes from the fire manifest's time map instead of
  the default (0, 1, 30).

No fire test was run (the GPU bake owns the GPU), and no existing test drives a fire medium through the
controller. A fire scrub regression could not be reused (§7).

## 2. Scrub and playback

**The scrub path.** `OnTimeScrub(t)` is unchanged up to the mutation: cancel, wait for the render to park, apply
`SetSceneTime`. With the render still parked, it then calls `InstallSimulatedMeshesAtTime(job, t)`. That function:
- runs `PrepareAssetForRender({t, t, t})` on every simulated mesh some object renders, which does the lean install:
  positions, normals, areas and BVH refit, with no certificates and no logging;
- invalidates the top-level structure.

The kick follows.

The pass then goes through the M3b prepared path (`RasterizeScenePrepared`, `PrepareAndFreeze`):
- the TLAS is invalidated and rebuilt from the new frame;
- the light samplers are invalidated, so emissive meshes are rebuilt at `AttachScene`;
- the group prepares the mesh again at the same support. The memo finds that frame already in place, so the pass
  does no read and no decode. Without the memo, every progressive pass of a static scrub position re-read and
  re-decoded the frame (M3b's viewport behaviour).

So the geometry is written only on the thread that holds the park. The pass's own prepare remains as the fallback,
for example for the first pass after load.

**Motion blur in the viewport: off.** The interactive rasterizer (and the variant path-tracing pipeline in the
viewport) render stills through `RasterizeScene`, which gives no ray a sample time. With the camera's exposure as
support, shutter keys would only widen the BVH boxes and switch off the float filter, and every ray would see the
pose at the nominal time anyway. Assets therefore install at {t, t, t}. The viewport shows the nominal pose:
- the frame itself at a frame time;
- `P + V·Δs` between frames, as M3b's statics do.

Production renders keep deformation blur.

**Timeline.**
- `GetHasAnimation` is true when there are keyframes, time-varying media, or a bound simulated mesh.
- `GetAnimationOptions` keeps the scene's declared animation.
- When the scene declares none and has simulation assets, the range is the union of their frames:
  - start = the earliest first frame;
  - end = the latest last frame;
  - frames = span / smallest interval + 1.
- This reads time maps only: no I/O, cheap enough for the GUI's poll.

`GetTimeVaryingAssets` (C API: count and index) gives the per-asset ranges with availability. A partial disk cache
re-reads its manifest; `baking` means a writer holds the cache lock.

**Playback clock.** The controller has no playback mechanism of its own: the macOS shell loops `scrubTime` on a
~30 Hz Swift task. `PlaybackClock` is the library's.
- `PlaybackPlay(fps, loop)` uses the timeline range. `PlaybackPlayRange` takes an explicit one.
- It opens one scrub composite (one undo group) and starts a thread. The thread advances scene time in real time,
  quantized to the `fps` grid, and shows each new frame through `OnTimeScrub`.
- **Coalescing:** a new frame is applied only once the viewport has completed a pass since the last apply (an
  uncancelled pass, counted by the render loop), or after 1 s without one. Clock frames passed over are counted as
  dropped.
- **Stopping:** without loop, playback shows the last frame and stops. A manual `OnTimeScrubBegin` from another
  thread (the user grabbing the slider, or the shell's own play loop) pauses library playback first.
- `PlaybackSeek` moves the clock while playing, or does one scrub while paused.

## 3. Live simulation and the threading model

`LiveSimulationAttach(geometry, solverId, parameters)` does three steps.

On the calling thread, with no scene lock:
- creates the solver from the registry and parses the parameters (JSON or CBOR);
- `InProcessHost::Configure` (scene inputs are M4: `NoSceneInputs`). The solver must declare
  `capabilities.interactive`; fire.methane does not and is refused;
- `Start`, then `Step` once, which publishes frame 0;
- opens two live sources on the host's publication: one to bind, one for timing queries. The published topology must
  equal the bound mesh's. A different topology is refused: changing topology needs a reload (M7).

Under the park:
- checks that the geometry is still the scene's;
- `RebindSource` swaps the live source in, keeping the baked one for detach;
- if the scene time is past the latest published frame, moves it there with a `SetSceneTime`;
- installs the frame.

**Play** starts the clock and each attached host's worker (`InProcessHost::Play`). The solver then steps as fast as
it can, and frames accumulate in the `MemoryPublisher`. The clock follows real time but is clamped to the latest
frame published contiguously from the first, so it never runs ahead. `OnTimeScrub` clamps the same way, which
covers every scrub. `LastSceneTime` reports the clamped time.

**Scrubbing.**
- Backward reads published frames (they all stay in memory).
- Forward past the solver clamps to its latest frame. Clamping was chosen over waiting: a UI scrub never blocks on
  the solver.

**Pause** pauses the clock and the hosts. `InProcessHost::Pause` interrupts the current `Advance` at a safe point.

**Restart.**
- `LiveSimulationReset` restarts from the initial state. A new host is configured and started, and frame 0 is
  published off every lock. Under the park the geometry is rebound and scene time moves to frame 0. The old host is
  then retired.
- `LiveSimulationSetParameters` does the same with new parameters, which gives a new cache key. A topology change
  is refused and the current run is kept.
- `LiveSimulationRecord(dir)` restarts with `InProcessHost::Record`, so the run is also written to a new epoch of a
  disk cache, sealed when it finishes. It is implemented but not exercised by the test (§7).

**Detach** pauses and joins the solver, rebinds the baked cache under the park, and installs at the current time.
Playback goes on with the baked cache.

**Threads and what they touch.**

| Thread | Touches | Never touches |
|---|---|---|
| Solver (the host's worker) | Its own state; `MemoryPublisher::Publish`, which is mutex-guarded and makes a new immutable `MemoryCacheSnapshot` | Scene data |
| Playback clock | `OnTimeScrub` (the park protocol); `LiveSimulation::Latest` | Geometry directly |
| UI / API caller | Attach, detach and restart (geometry swaps under the park); status | |
| Render | The in-pass prepare (memoized) and rays; a live source's provider, which takes a shared pointer to the current immutable snapshot | |

**Installs** happen only while the render is parked: on the scrub's thread, or at the start of the render's own pass.
Both read immutable snapshots.

**Lock order:** `mLiveOpMutex` → `mLiveMutex` → `mRenderAdmissionMutex` → `mMutex`; then a session's mutex, the
feed's mutex and the publisher's mutex.
- `OnTimeScrub` holds `mLiveMutex` from the clamp through the install, so a restart cannot rebind between the two.
- The clock calls its live-limit and live-play hooks outside its own lock and outside any apply.

**Lifetime.**
- A source's provider reads its host through a `Feed`. The `Feed` is cleared under its mutex before the host is
  destroyed. A source that outlives its run then reads "publication gone" instead of a dead host.
- `Stop()` (and so the destructor and `PrepareForDestruction`) first shuts the clock down: the frame in flight
  finishes, the composite closes, and the thread is joined.
- It then detaches every live run: solver paused and joined, baked cache rebound under a forced park, host
  destroyed. Only then does the rest of teardown run.
- The live session holds a counted reference to the geometry.

## 4. Tests (`tests/SimulationViewportTest.cpp`, 104 checks, about 7 s)

Headless, over the real interactive rasterizer (`CreateInteractiveMaterialPreviewPipeline`), 640 × 360 film.

| Case | Checks |
|---|---|
| T | A scene whose only time-varying content is a 6-frame baked sheet: `GetHasAnimation` is true and the range is (0, 1.25, 6). The asset info reports complete, 6 of 6 frames, 0.25 s interval, preview |
| S | Scrub to t = 0.5, 1.0, 0.25, 1.25, 0.6 (between frames) and 0.0. Each time the mesh installed in the scene **equals exactly** the frame an independent `MeshCacheSource` gives for that time (frame index and every vertex). The install count does not change across the following passes (installed under the park; the pass's prepare is a no-op). No shutter keys although the camera has exposure 0.05 |
| P | Play 0–1.25 s at 8 fps without loop: stops on the last frame, shown + dropped = 11 clock frames (11 shown, 0 dropped here), mesh at frame 5. Looping playback; a manual `OnTimeScrubBegin` pauses it and its frame installs |
| L | cloth.shell 51 × 51, pins u0v0 and u1v0, sphere collider, interactive, 91 frames at 30 Hz, bound to a 2-frame bake of the same configuration (the scene needs a cache to load; the M4 binding removes this) |

What L checks:
- second attach refused; attach to a non-mesh refused; asset info reports state live with 91 frames;
- three 1 s play phases sampled every 10 ms: displayed time and published frames never decrease, and the displayed
  time is never past the latest published frame;
- after each pause, the solver stops, and the installed mesh equals the publication's frame for the displayed time
  (exact);
- at least 30 frames in 3 s;
- scrub back to 0.5 s shows frame 15; scrub to 100 s clamps to the latest frame;
- new parameters with a 41 × 41 topology are refused and the run is unchanged; new parameters with the same
  topology (gravity) give a new key, starting at frame 0;
- reset while playing: the installed mesh is the new run's frame for the displayed time;
- a paced run (`pace_ms` 60, slower than real time): the clock waits for it (about 90 waits in 1.5 s) and is never
  ahead;
- detach while playing: the baked cache comes back and playback continues;
- scene close (controller `Stop` and delete, then the job) while playing joins everything, in about 10 ms.

**ThreadSanitizer.** There is no TSan recipe in the tree. A TSan build of the whole library plus this test was made
in the session scratch, not in the tree:
- the 422 library translation units at `-O1 -g -fsanitize=thread`;
- external libraries (OIDN, OpenPGL, OpenVDB, TBB) uninstrumented.

It ran the full test with `RISE_M3C_SLOW=12`, which stretches the play windows because TSan slows the solver about
20×. Result: **no ThreadSanitizer report**. That covers play, pause, seek, reset while playing, paced-solver waits,
detach while playing, and scene close while playing. The one failure under TSan is the rate check: 22 frames in
3 × 12 s instead of 30. AddressSanitizer was not run.

**Other suites.**
- `SourceHygieneTest` 706/0: the five build lists and the vtable manifest; `IJob` is unchanged.
- `SimulatedMeshGeometryTest` 113/0, with the other agent's mesh.v2 edits in the tree.
- The non-fire fast tier (`./run_all_tests.sh --tier fast --timeout 300 --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`):
  417 built, **417 passed**, with the other agent's in-progress solver and codec edits in the tree (load average
  10–27 during the run). The build is warning-free.

## 5. Measurements (not a gate)

Setup: `SimulationViewportTest --bench`, an M1 Max, 640 × 360 film, live cloth.shell interactive, 301-frame runs
at 30 Hz. Each row: solver alone on the host thread for 3 s, then 4 s of playback at 30 fps.

Machine load: the GPU fire bake was running (`rise_sim_m2`, about 65% CPU), plus other sessions. The load average
was 10–16 for the two quoted runs. Earlier runs at load 9–15 gave much lower numbers in some repeats, while another
agent's tests were also running: 51 × 51 solver 17–56 frames/s, 101 × 101 4–16 frames/s. Read these as a
quiet-ish best case and a loaded worst case, not as exact rates.

| Grid (vertices) | Solver alone | Solver while playing | Displayed | Dropped | Viewport installs | Completed passes |
|---|---|---|---|---|---|---|
| 51 × 51 (2,601) | 66.9–67.0 frames/s (134 substeps/s) | 66.7–66.9 frames/s | **30.0–30.1 fps** (clock-limited) | 0 | 29.8–29.9/s | 30.1–30.3/s |
| 101 × 101 (10,201) | 19.3–19.9 frames/s | 18.9–19.4 frames/s | 18.9–19.4 fps (solver-limited; 176–182 clock waits) | 0 | 18.7–19.2/s | 19.2–19.7/s |

- **Acceptance.** The M3 target is ≥ 15 displayed fps at 51 × 51. It is met: 30 fps, the clock rate, with no
  drops.
- **101 × 101** plays at the solver's rate, just under 20 fps.
- **Resolution during playback.** The viewport renders at the scrub preview scale, 1/4 of the film: 160 × 90. This
  is the existing time-scrub behaviour (`OnTimeScrubBegin` sets the motion-start scale). Once playback pauses, the
  viewport refines back to full resolution. One run caught a 320 × 180 or 640 × 360 frame while the solver was
  stalled. Full-resolution playback would be an M5 choice: pin the scale during playback, at a lower frame rate.
- **Install cost** (M3b, §3): 0.7 ms at 2.6k vertices and 2.6 ms at 10k, now paid once per displayed frame instead
  of once per pass.

## 6. What M5 must do (GUI)

**Timeline visibility.** No change needed. Both shells already show the scrubber when
`RISE_API_SceneEditController_GetHasAnimation` is true, and size it from `GetAnimationOptions`. That query is now
true for simulation-only scenes, with the cache range.

**Play / pause.** Replace the shell-side play loops with the library clock:
- macOS: `RenderViewModel.startPreviewPlay` / `stopPreviewPlay`, the Swift `Task` stepping `sceneTime`.
- Windows: `ViewportTimeline`'s timer.

Calls:
- `PlaybackPlay(p, fps, loop)` (fps 0 uses the timeline's frame spacing), `PlaybackPause(p)`, and
  `PlaybackSeek(p, t)` for transport buttons.
- Poll `GetPlaybackStatus` (about 10–30 Hz) to move the playhead: `outTime` is the last shown time; `outPlaying`
  goes false at the end.
- A manual slider drag still calls `OnTimeScrubBegin` / `OnTimeScrub` / `OnTimeScrubEnd`. Begin pauses library
  playback, so the shell must re-read status.
- After any scrub, read `LastSceneTime`: with a live simulation the time may be clamped.

**Baked ranges.** `GetTimeVaryingAssetCount` and `GetTimeVaryingAsset(index, …)` give, per asset: name, kind,
state, predictive flag, start, end, available end, interval, frames available / total. Draw [start, availableEnd]
as baked and [availableEnd, end] as pending. State `baking` or `live` means the range grows, so poll at about 1–2 Hz.

**Live play.**
- Call `RISE_API_RegisterBuiltinSolvers` once at startup.
- `LiveSimulationAttach(p, geometry, "cloth.shell", json, len, err, errLen)` blocks for configure plus one step:
  about 0.5 s on first use for the cloth evidence, then ~10 ms. Call it off the main thread, or show progress.
- `LiveSimulationReset`, `LiveSimulationSetParameters`, `LiveSimulationRecord(dir)` and `LiveSimulationDetach`.
- Status: `GetLiveSimulationStatus(geometry, …)` (non-blocking; false while contended, so keep the last value) and
  `LiveSimulationCount`.
- Until M4 there is no scene-file way to say which geometry a solver drives: M5's UI picks a
  `simulated_mesh_geometry` by name.

**Signatures (all in `namespace RISE`, `RISE_API.h`).**

```cpp
bool RISE_API_RegisterBuiltinSolvers( char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_GetTimeVaryingAssetCount( SceneEditController* p, unsigned int* outCount );
bool RISE_API_SceneEditController_GetTimeVaryingAsset( SceneEditController* p, unsigned int index,
	char* nameBuf, unsigned int nameBufLen, int* outKind, int* outState, bool* outPredictive,
	double* outStart, double* outEnd, double* outAvailableEnd, double* outInterval,
	unsigned long long* outFramesAvailable, unsigned long long* outFramesTotal );
bool RISE_API_SceneEditController_PlaybackPlay( SceneEditController* p, double fps, bool loop );
bool RISE_API_SceneEditController_PlaybackPlayRange( SceneEditController* p, double timeStart, double timeEnd,
	double fps, bool loop );
bool RISE_API_SceneEditController_PlaybackPause( SceneEditController* p );
bool RISE_API_SceneEditController_PlaybackSeek( SceneEditController* p, double t );
bool RISE_API_SceneEditController_GetPlaybackStatus( SceneEditController* p, bool* outPlaying, bool* outLive,
	bool* outWaitingForSolver, double* outTime, double* outTimeStart, double* outTimeEnd, double* outFps,
	unsigned long long* outFramesShown, unsigned long long* outFramesDropped, unsigned long long* outSolverWaits,
	unsigned long long* outPassesCompleted, double* outWallSeconds );
bool RISE_API_SceneEditController_LiveSimulationAttach( SceneEditController* p, const char* geometryName,
	const char* solverId, const unsigned char* parameters, unsigned int parametersLen,
	char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_LiveSimulationDetach( SceneEditController* p, const char* geometryName,
	char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_LiveSimulationReset( SceneEditController* p, const char* geometryName,
	char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_LiveSimulationSetParameters( SceneEditController* p, const char* geometryName,
	const unsigned char* parameters, unsigned int parametersLen, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_LiveSimulationRecord( SceneEditController* p, const char* geometryName,
	const char* cacheDirectory, char* errorBuf, unsigned int errorBufLen );
bool RISE_API_SceneEditController_GetLiveSimulationStatus( SceneEditController* p, const char* geometryName,
	bool* outPlaying, bool* outFinished, bool* outFailed, long long* outLatestFrame, double* outLatestSceneTime,
	double* outFirstSceneTime, double* outLastSceneTime, unsigned long long* outFramesPublished,
	unsigned long long* outFramesTotal, double* outSimulatedTime );
unsigned int RISE_API_SceneEditController_LiveSimulationCount( SceneEditController* p );
```

`outKind`: 0 mesh, 1 fire. `outState`: 0 complete, 1 partial, 2 baking, 3 live. Every out pointer may be null.
Exceptions never cross the ABI: they come back as false, with the message when the call has an error buffer.

## 7. Limits and what was not verified

- **Topology is fixed.** A live solver must publish the bound mesh's topology. The scene still needs a baked cache
  of that topology to load; M4's producer binding removes this.
- **Texture coordinates come from the scene's cache.** A mesh.v2 UV set is installed once, at bind, from the baked
  cache. A live run's own UVs (cloth.shell publishes mesh.v1, without UVs) are not installed.
- **No scene inputs.** Live solvers get `NoSceneInputs` (M4). Colliders come from parameters only, not from scene
  objects.
- **Memory is unbounded per run.** The publication keeps every frame: about 150 KB per frame at 51 × 51, 0.6 MB at
  101 × 101. A run's length is the solver's `frames`. There is no ring buffer.
- **The clamp is global.** With several live simulations the clock follows the slowest one.
- **Agent re-derive.** An agent re-derive of the scene (D2) while attached is not detected automatically. The
  session keeps driving the old, orphaned geometry until it is detached. Attach and restart do check that the
  geometry is still the scene's.
- **Undo history.** Playback is one scrub composite. A restart or attach that moves scene time adds one undoable
  time edit.
- **Preview scale.** Playback renders at the scrub preview scale, 1/4 (§5).
- **Not exercised.**
  - `LiveSimulationRecord` is implemented over `InProcessHost::Record` but not exercised by the test.
  - The fire paths: the viewport path for fire is untouched by construction. Its timeline range from the fire
    manifest (`Job::DescribeTimeVaryingAssets`) is not tested, because no Fire* test could run (GPU bake) and no
    existing test drives fire through the controller.
  - VS2022, Xcode and Android were edited by hand, not built.
  - AddressSanitizer was not run.
  - `TimelineRange::Baking` is not tested (it needs a concurrent writer).
