# M5b results: simulations in the macOS app

2026-11-23. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §4.5, §5 (M5). Builds on
[M3c](SIMULATION_M3C_RESULTS.md) (playback clock, live C API, "What M5 must do") and
[M4](SIMULATION_M4_RESULTS.md) (scene simulations). The bake manager it drives is the other M5 track,
`Simulation/SimulationBakeManager.*` (its results note is `SIMULATION_M5A_RESULTS.md`).
Architecture rule followed: [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md) — the shell
presents library state and forwards actions; it computes no keys, inspects no files and runs no simulation logic.

**Status.**

- The full SwiftUI app builds here (Deployment) and runs headless probes.
- Playback goes through the library clock.
- The timeline draws baked ranges.
- A Simulations panel lists the scene's simulations, with Bake / Stop / Resume through the real bake-manager C API
  and Live / Reset / Record for interactive solvers.
- Refused cache locations prompt for trust; trusted roots are persisted and re-applied at launch.
- Every headless probe run passes; the clean two-run sequence of §5 has 36 and 46 checks.

Not done: simulation parameters in the **properties panel** (§4). The Windows app is unchanged (§7).

## 1. Building and running the app here

Host: macOS 27.2.1, Xcode 27.2 (27B52), M1 Max. As found by the M1 follow-up, Swift must compile against the
installed macOS 26.5 SDK because the 27.2 SDK's SwiftUI macros need a macro service this environment cannot launch.
Deployment target stays 26.2; nothing on the system was changed.

```sh
S=<scratch>                                     # derived data outside the repo
cd build/XCode/rise
xcodebuild -project rise.xcodeproj -scheme RISE-GUI -configuration Deployment \
  -derivedDataPath $S/dd -jobs 6 \
  'OTHER_SWIFT_FLAGS=$(inherited) -Xfrontend -sdk -Xfrontend /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk' \
  CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= CODE_SIGNING_REQUIRED=NO build
# same with -scheme rise for the CLI target
```

The signing overrides give an ad hoc signature (no identity, no team).

**Warnings.** The only warnings are the known linker ones: `building for macOS-26.2, but linking with dylib ...
built for newer version 27.0/27.2` for Homebrew OpenEXR, Imath, libpng, OpenVDB, TBB and blosc (the 26.2 release
gate in M1). There are no compiler warnings.

**Running.** Run the executable directly with the probe variables (§5): `$S/dd/Build/Products/Deployment/RISE-GUI.app/Contents/MacOS/RISE-GUI`.
The existing `viewport_reattach` probe passes on this build.

## 2. Project and build fixes (found by the first real Xcode build)

| Problem | Fix |
|---|---|
| `Fire/FireRunDriver.cpp` called `WriteProductionTemporalFrame`, which is declared only with `RISE_ENABLE_OPENVDB`. The GUI build has no OpenVDB, so this was a hard compile error. | A file-local stub, built only without OpenVDB, returns false, so publication fails there. The fire solver already refuses in such builds. The make build (OpenVDB) preprocesses identically. |
| The `rise-cache-import` helper (`scripts/build_macos_simulation_importer.py`) failed to link: `SimulationCache.cpp` now needs `MeshPayloadReferences` from `SimulationMesh.cpp` | Added `Simulation/SimulationMesh.cpp` to the helper's source list |
| The `rise` target compiled `Utilities/FireCase.cpp` without the per-file flags that the GUI target and make give it (fast-math kept, `-fno-lto`, so LTO cannot move the identity-bearing case IDs) | Gave the `rise` target the same `COMPILER_FLAGS` |
| The other agent's new `Simulation/SimulationProcess.cpp` and `Simulation/SimulationBakeManager.cpp` (and headers) were not in the project, so the link was undefined | Added both: file references, group, both Sources phases, Headers phase. The other agent added `Agent/AgentSimulation.*` itself. |

**Checked and found correct.**

- Strict-FP flags (`-fno-fast-math -ffp-contract=off`) are on the same 21 files in both targets, matching make's
  strict rules: the 7 cloth sources, the 5 strict fire-driver sources, and the `FireProduction*` and `OslNoise` files.
- No duplicate build files in either Sources phase.
- The GUI target picks up new Swift files on its own (`RISE-GUI` is a synchronized folder).

**Known noise.** The `rise` target prints Xcode's advisory `tasks in 'Copy Headers' are delayed by unsandboxed
script phases`. It was there before and is not a compiler warning.

## 3. What was built

**Library (additive).**

- `SceneSimulationStatus::meshConsumers`: the `simulated_mesh_geometry` elements bound by `source`. It is filled in
  `Job::DescribeSimulations`. A live run is addressed by its geometry.
- New `RISE_API` block, "Simulation M5 (GUI shell support)", at the end of `RISE_API.h` / `.cpp`:
  - `GetSimulationDetails`: `GetSimulation` plus solver id, backend, fidelity, consumer, time support (start,
    interval), upstream, inputs.
  - `AddTrustedCacheRoot`. On POSIX it also exports the roots in force to the process's
    `RISE_SIMULATION_TRUSTED_ROOTS`, because bakes run in `rise-sim` worker processes that read only the environment.
  - `CheckCacheLocation`: accepted or refused, plus the writer's reason and a suggested root. The suggestion is the
    deepest existing directory above the cache. It is empty when the cache directory itself is not private.
  - `GetTrustedCacheRoots`.
- The make build of `librise rise` is warning-free.

**Bridge** (`RISEViewportBridge.{h,mm}`, "Simulation M5" section). The methods are thin forwards:

- value snapshots `RISETimeVaryingAssetInfo`, `RISESimulationInfo`, `RISEPlaybackStatus`,
  `RISELiveSimulationStatus` and `RISESimulationBakeJob`;
- playback `play/pause/seek/status`, `timeVaryingAssets`, `simulations`;
- live `attach/detach/reset/record/resync/status`;
- bake `bake/resume/stop/jobs`, `changeCounter`, `refreshSimulationFrames`;
- trusted roots.

The blocking calls (attach, reset, record, resync, bake queueing) hold the bridge's existing preview-lifecycle mutex,
so `-shutdown` cannot destroy the controller under a background call.

**Swift.**

| File | What |
|---|---|
| `App/RenderViewModel.swift` | Play / Stop now call `PlaybackPlay(fps 0 = timeline spacing, loop)` / `PlaybackPause`, and a 20 Hz poll of `GetPlaybackStatus` moves the playhead. The Swift 30 Hz stepping task is gone. Every scrub (drag, jump, range reconcile) reads back `LastSceneTime`, the time a live run clamped to. The 2 Hz poll forwards the CST revision (edits) and the bake change counter to the simulations model. |
| `App/SimulationsModel.swift` (new) | Rows, live runs, bake jobs, timeline ranges; actions; the 1 Hz poll, which runs only while a bake, a live run or a call is in flight; trusted-root preference store; `LibraryBakeService` (the real API) and an `UnavailableBakeService` fallback; `SimulationLiveRunner` (off-main calls) |
| `App/SimulationsPanel.swift` (new) | Right-panel section (after the Outliner), shown only when the scene declares simulations; the trust prompt |
| `App/TimelineSlider.swift` | `BakedRangesTrack`: one 3 px row per asset under the trough; solid = start→availableEnd, faint = →end; colour = state; tooltip with frame counts |
| `App/SimulationProbes.swift` (new) | `SimulationBootstrap` (`RegisterBuiltinSolvers` + persisted trusted roots, once at process start) and the `simulation_m5` probe |
| `App/RISEApp.swift`, `App/ContentView.swift` | Bootstrap and probe entry; the panel in the right column |

**Panel rows.** Each row shows:

- a state dot, the name, the solver id (or the keyword for `simulation_cache`), and the state (missing / baking /
  partial / complete / stale / failed, or live);
- the fidelity label (or "no cache"), frames available / total, the time support, and the cache path (middle-truncated,
  full path in the tooltip);
- an active bake job's summary (state, frames, ETA, what a queued job waits for);
- the buttons:
  - Bake, Rebake (force), Stop or Resume, depending on the bake job and cache state;
  - for an interactive solver with a consumer, Live, or Stop live / Reset / Record while live.

Selecting a row expands keyword, backend, consumer, scene inputs, upstream (and "out of date"), the live-run frame
count, and the library's error text.

**Live.** Live attaches off the main thread, then restarts the clock so it starts the new run's solver. Record checks
the cache location first, then runs `LiveSimulationRecord` into the simulation's declared cache directory.

**Edits.** When the CST revision changes, the model re-reads the states and, if a run is live, calls
`LiveSimulationResync` off the main thread. An edit never starts a bake; the probe checks this.

**Trust prompt.** Bake, Resume and Record first ask `CheckCacheLocation`. When the location is refused, an alert shows:

- the library's reason;
- what trusting the suggested root gives up, in the words of the contract;
- "Trust this location" and Cancel.

Trust calls `AddTrustedCacheRoot`, saves the root under `trustedSimulationCacheRoots` in the app's preferences,
checks the location again, and continues the action. Nothing is trusted without that click. When no root can help
(the cache directory itself is not private), the alert explains that instead.

## 4. Properties panel: not wired (needs a controller category)

The brief expected the producer chunks' descriptors to make `cloth_simulation` / `fire_simulation` editable in the
existing properties panel. They do not reach it:

- The panel is fed by `SceneEditController`'s category system (`SceneEditCategory_*` 0–11 and the per-category
  switches in `SceneEditController.cpp`).
- That system has no Simulation category, so a producer chunk is not listed in the Outliner and cannot be selected.

The generic surface such a category would use already exists: `CstIntrospection::Inspect(doc, job, name, "simulation",
…)`, the same call Geometry uses. Adding the category touches only the controller, plus three platform enum mirrors:

- the enum value;
- `RoleKindSuffixForCategory` (suffix `simulation`; this matches `cloth_simulation` and `fire_simulation`, not
  `simulation_cache`);
- the entity count and name (`IJob::EnumerateSimulationNames` exists);
- the property-snapshot case (`CstIntrospection::Inspect`);
- the set-property path, which routes to the CST param edit like Geometry.

`SceneEditController` was being edited by the bake-manager agent during this session and was off limits, so this
was not done. Until it is, the panel's expanded row says to edit the chunk in the Scene file tab. An edit by any path
(scene text, agent, chat) marks the simulation stale and starts nothing: the probe's `edit` step checks this through
the in-app agent path.

## 5. Headless probes

The probe drives the real `RenderViewModel` / `SimulationsModel` / `RISEViewportBridge` stack: scene load through
`loadScene(untitled:)`, the viewport render thread, the library clock, and the bake manager. It runs before SwiftUI,
like `viewport_reattach`.

Variables:

- `RISE_GUI_HEADLESS_PROBE=simulation_m5`;
- `RISE_GUI_HEADLESS_PROBE_STEPS`: a subset of `list,trust,bake,live,scrub,edit`;
- `RISE_GUI_HEADLESS_PROBE_EXPECT`: the expected state of `flag`;
- `RISE_GUI_HEADLESS_PROBE_OUT`: where PNG snapshots go;
- `RISE_GUI_HEADLESS_PROBE_SCENE`: default `scenes/Tests/Simulation/cloth_flag_scene.RISEscene`;
- `RISE_SIM_WORKER`: the `rise-sim` the bake manager runs (a copy of `bin/tools/rise_sim`);
- `RISE_MEDIA_PATH=<repo>/`.

**Trust.** The checkout is on a 0775 mount (`/Volumes/SSD`), so the scene-relative cache needs a trusted root.

- The `trust` step runs without one and verifies the refusal and the prompt.
- The other steps run with `RISE_SIMULATION_TRUSTED_ROOTS=<repo>/scenes/Tests/Simulation`.
- The trust step writes only a probe-private defaults suite, deleted afterwards. The user's preferences were checked
  untouched.

Cloth only, CPU. No fire solver, bake or live session ran.

**The clean sequence.** `caches/` was removed first.

Run 1, untrusted: `list,trust,live`, expecting `missing`. **36/36 PASS.**

- **List.** One row: `flag`, `cloth_simulation`, `cloth.shell`, backend cpu, interactive, consumer `geo_flag`, 91
  frames at 30 Hz from 0, cache `…/scenes/Tests/Simulation/caches/flag`, input `pole`, state missing, no timeline
  range.
- **Trust.** The library refuses ("ancestor other users can modify: /Volumes/SSD") and suggests
  `<repo>/scenes/Tests/Simulation`.
  - Record raises the prompt instead of writing, and nothing is trusted before the click.
  - After Trust the root is in force and persisted, the location is accepted, and the action is forwarded.
- **Live, no pre-bake** (unbound consumer). Attach completes.
  - The clock plays: scene time 0 → 1.967 s in 2 s; 60 clock frames shown, 0 dropped, 0 solver waits; the
    viewport delivered 62 frames.
  - The timeline range turns `live`.
  - The playhead is not past the latest published frame (91/91 published, latest at 3.0 s).
  - Scrubbing back to 0.5 and on to 2.5 shows those times exactly; detach completes.

Run 2, trusted: `list,bake,live,scrub,edit`. **46/46 PASS.**

- **Bake from the app.** The library bake manager ran a `rise-sim` worker. The job appeared and finished in 1.5 s;
  frames written never decreased; a partial range was seen while it ran. The state ends complete, with a complete
  91-frame range.
- **Live over the baked cache.** As run 1: 0 → 2.000 s, 61 shown, 0 dropped.
- **Scrub the baked cache.** t = 0.5 and t = 2.5 are shown exactly (LastSceneTime).
  - The mean luminance difference between the two viewport images (96×54) is 0.0214. That is the change.
  - Going back to t = 0.5 reproduces the first image exactly (difference 0.0000).
  - Live-run frames at 0.5 and 2.5 matched these baked frames exactly in an earlier run (difference 0.0000).
- **Ordinary playback on the library clock.** 0.5 → 1.967 s, 45 shown, 0 dropped, not live; stopped cleanly.
- **Edit.** `propose_patch flag substeps 12` (full re-derive) turns the simulation stale within the 2 Hz poll. The
  stale cache still has 91 frames and its timeline range turns `stale`. No bake job was started.

Further runs passed as well:

- `list` against the baked cache, expecting `complete` (fidelity `preview`, 91/91, complete range);
- the first combined run;
- untrusted `list,trust,bake`: after the in-app Trust, the app's bake ran and completed. Its `rise-sim` worker
  inherited the root through the exported environment, with no `RISE_SIMULATION_TRUSTED_ROOTS` set by the caller.

**Snapshots** (480 × 270, outside git): `rendered/simulation_m5/`

- `baked_t0.5.png`, `baked_t2.5.png`: the baked cloth scrubbed to two times;
- `live_t0.5.png`, `live_t2.5.png`: the live run scrubbed to the same times;
- `live_after_2s.png`: live playback paused at about 2 s;
- `ui_after_bake_*`, `ui_live_*`, `ui_stale_selected_*` (`_panel`, `_timeline`): SwiftUI `ImageRenderer` renders of
  the real Simulations panel and timeline bar at those points: complete / live (purple track) / stale with the row
  expanded. They are offscreen renders, not window screenshots: the alert and hover states are not covered.

The logs are in the session scratch (`m5b/logs/probe-run1.log`, `probe-run2.log`).

**Reproduce.** From the repo root, with the app built as in §1:

```sh
export RISE_MEDIA_PATH="$PWD/" RISE_SIM_WORKER=<copy of bin/tools/rise_sim> \
       RISE_GUI_HEADLESS_PROBE=simulation_m5 RISE_GUI_HEADLESS_PROBE_OUT=$PWD/rendered/simulation_m5
RISE_GUI_HEADLESS_PROBE_STEPS=list,trust,live RISE_GUI_HEADLESS_PROBE_EXPECT=missing <app>/Contents/MacOS/RISE-GUI
RISE_SIMULATION_TRUSTED_ROOTS=$PWD/scenes/Tests/Simulation RISE_GUI_HEADLESS_PROBE_STEPS=list,bake,live,scrub,edit \
  RISE_GUI_HEADLESS_PROBE_EXPECT=missing <app>/Contents/MacOS/RISE-GUI
```

Each run prints `SimulationProbe: ALL PASS` and exits 0. The bake writes
`scenes/Tests/Simulation/caches/flag`, which is gitignored; delete it to repeat from missing.

## 6. What a human should look at

The probes test the model layer and the viewport pixels. No window was opened, so these were not seen:

- **Panel layout** in the 344 pt right column: row wrapping of the detail line, button fit (Live / Stop live / Reset
  / Record on one line), the alert text length.
- **Baked-range track**: colours in light and dark themes, alignment with the trough, tooltip, behaviour with
  several assets (each adds 5 pt to a 58 pt bar).
- **Playback feel**: playback is now **real time at the authored frame spacing**. Previously the shell stepped one
  frame per 33 ms whatever the animation's frame rate. A 30 fps animation is unchanged; a scene whose `num_frames`
  implies another rate plays at that rate now. During playback the viewport renders at the scrub preview scale (M3c
  §5).
- **Live dynamics**: `live_after_2s.png` (t ≈ 1.97 s) shows the flag swung up near horizontal and darker (its back
  face). The 0.5 s and 2.5 s frames hang down. Live and baked frames are identical, so this is the solver's motion,
  not a shell artefact. Worth a look in motion.
- **Bake flow with a saved, edited scene**: the bake manager bakes the file on disk ("save first" refusals appear
  as the row message).
- **Trust prompt** wording and the persisted root, after an app restart.

## 7. Windows (Qt) mirror — not edited

| Qt file (`build/VS2022/RISE-GUI/`) | Change |
|---|---|
| `ViewportBridge.{h,cpp}` | Same forwards as the macOS bridge:<br>• `PlaybackPlay` / `Pause` / `Seek` / `GetPlaybackStatus`;<br>• `GetTimeVaryingAssetCount` / `GetTimeVaryingAsset`;<br>• `GetSimulationCount` + `GetSimulationDetails`;<br>• `LiveSimulationAttachSimulation` / `Detach` / `Reset` / `Record` / `Resync` / `GetLiveSimulationStatus` / `Count`;<br>• `SimulationBake` / `BakeResume` / `BakeStop` / `GetSimulationBakeJobCount` / `GetSimulationBakeJob` / `SimulationBake_ChangeCounter` / `RefreshSimulationFrames`;<br>• `AddTrustedCacheRoot` / `CheckCacheLocation` / `GetTrustedCacheRoots`.<br>The blocking ones run on a worker (`QtConcurrent`), guarded against shutdown. |
| `ViewportTimeline.{h,cpp}` | Replace `m_playTimer` / `onPlayTick` stepping with `PlaybackPlay` + a ~20 Hz status poll. Read `LastSceneTime` after every scrub. Paint the per-asset range rows under the slider. |
| `MainWindow.{h,cpp}` | Call `RegisterBuiltinSolvers` and re-apply persisted trusted roots (`QSettings`) at start. Host the Simulations dock; poll the bake change counter and the CST revision (re-read, `LiveSimulationResync`). |
| new `SimulationsPanel.{h,cpp}` (+ `RISE-GUI.vcxproj`) | The panel and the trust `QMessageBox`, mirroring `SimulationsPanel.swift`. The proposal (§5 M5) asks Windows for monitoring and playback: Live needs no GPU solver, and Bake runs a `rise-sim` worker if one is built for Windows. |
| `ViewportProperties.{h,cpp}` | Nothing until the controller's Simulation category exists (§4). |

On Windows the cache writer has no ancestor policy, so `CheckCacheLocation` accepts and the trust prompt never
shows.

## 8. Not verified, and limits

- **Configurations and signing.** Only `Deployment` was built. `Opto` (full LTO, fast-math) was not built; nothing in
  this change is floating-point sensitive. The bundle is ad hoc signed; no notarization or hardened-runtime check.
- **No window.** Only offscreen renders of the panel and timeline, and the viewport pixels (§5, §6).
- **Concurrency.** No ThreadSanitizer run of the shell's off-main calls. They hold the bridge's lifecycle mutex, as
  the preview fetches do.
- **Bake actions not exercised** from the app: Stop, Resume, force Rebake. The probe bake finished in 1.5 s, before a
  Stop could land. Record was exercised only up to the library's refusal (no live run).
- **No fire.** No fire simulation was listed, baked or played (GPU bake in progress). The panel lets a user bake
  fire; it is a `rise-sim` worker under the bake manager's GPU lock.
- **The `edit` step** used the agent commit path. The Scene-file-tab path reaches the same CST revision check but was
  not driven.
- **Trusted roots reach bake workers through the environment.** `AddTrustedCacheRoot` exports the roots to the
  app's `RISE_SIMULATION_TRUSTED_ROOTS`, which `rise-sim` workers inherit (verified by the `list,trust,bake` run). If
  the bake manager later passes the roots to its workers itself, the export is redundant but harmless.
- **The other agent's files.** They were changing while this was built. The final project check is in the report
  that accompanies this note.
