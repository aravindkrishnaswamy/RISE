# M5c results: the gaps left by M5

2026-11-23. Follows [M5a](SIMULATION_M5A_RESULTS.md) (bake manager) and [M5b](SIMULATION_M5B_RESULTS.md)
(macOS app). Five gaps closed: simulation parameters in the properties panel, the bake worker shipped in the app,
memory and cost estimates, the Simulations panel polish, and the playback rate of ordinary animations.
The solver contract is unchanged (Appendix C has the additive host-side rows). The Windows app is not edited (§6).

## 1. Simulation parameters in the properties panel

**Library.** `SceneEditController::Category::Simulation = 12` (`kNumCategories` 13), mirrored as
`SceneEditCategory_Simulation` in `RISE_API.h` and `RISEViewportCategorySimulation` in the bridge.

| Piece | Where |
|---|---|
| Enumeration (count, name, bulk names) | `IJob::EnumerateSimulationNames`, the Medium shape |
| Chunk addressing | `RoleKindSuffixForCategory` → `simulation` (matches `cloth_simulation`, `fire_simulation`); `Cst::RoleMatchesKindConstraint` gains the registry arm `simulation` → `ChunkCategory::Simulation`, so `simulation_cache` resolves too |
| Rows | `InspectSimulationChunk_`: `CstIntrospection::Inspect(…, "simulation")` (the solver schema's descriptor, as Geometry) plus one editable row per occurrence of a repeatable field, `<field>[i]` (`pin[1]`, `collider_object[0]`, `follow_object[0]`). Object references carry `referenceCategories` and the scene's objects as presets, so the Inspector shows its reference picker (`ReferenceChipCell`'s preset menu) and Jump to Definition |
| Edits | `SetPropertyInner_` Simulation arm → `ApplyAgentParamEditInner_(name, "simulation", …)` (undo/redo, dirty, re-derive, like Geometry). An occurrence row is validated against the descriptor and the document first (refused when the field is not repeatable or the index is absent) and routed with that occurrence |
| Live runs | `SetProperty`, `SetPropertyForCategory`, `Undo` and `Redo` call `LiveSimulationResync` after a successful edit when a run is live (the agent path already did). Resync restarts only runs whose configuration fingerprint changed |
| Primary snapshot | Simulation is routed like Painter/Geometry, so the single-panel path is not empty |
| Jump targets | `ChunkCategory::Simulation` maps to the category (a `source <simulation>.<output>` row jumps to its producer) |

An edit marks the simulation stale and starts nothing: the bake manager only runs on a Bake request.

**Shell.** Outliner row "Simulations" (tag SIM); Inspector title and glyph; selecting a Simulations-panel row also
selects the simulation in the scene (`jumpToEntity(.simulation, …)`), so the Inspector shows it. The panel's expanded
row now says the parameters are in the Inspector.

**Stale reason** (library, `SceneSimulationStatus::staleReason`): the cache's job record (`rise-sim.job.cbor`:
solver, version, effective parameters) against the configuration.

- `parameters changed (substeps)`: names the effective-parameter members that differ. A moved or resized collider
  shows as cloth's `colliders` member, because cloth folds the collider geometry into its effective parameters.
- `scene input changed (pole)`: parameters equal, key different, so a scene input's identity changed.
- `upstream out of date`: the upstream is not complete (also replaces a "scene input changed" caused by it).
- `solver version changed`, `configuration changed` (no job record), or the prediction error when a fire key is
  unknown.

## 2. The worker ships in the app

**Xcode target `rise-sim`** (command-line tool, `tools/rise_sim.cpp`):

- Sources: the `rise` target's Sources phase minus `commandconsole.cpp`, with the same per-file build settings, so the
  strict-FP files (`-fno-fast-math -ffp-contract=off`) and `FireCase.cpp`'s flags carry over. The phase was generated
  from the current project (453 files, including the concurrent agent's new Rigid and SimulationTransform files).
- Fire: `-DRISE_ENABLE_OPENVDB` and OpenVDB/TBB/blosc (`-isystem $(RISE_OPENVDB_PREFIX)/include`,
  `-lopenvdb -ltbb -lblosc`), as the make build links `bin/tools/rise_sim`. The app target still has no OpenVDB.
- Settings copied from the `rise` target's four configurations, with `DEPLOYMENT_LOCATION = NO`, `SKIP_INSTALL = YES`
  (the `rise` target installs into a fixed home directory), `MACOSX_DEPLOYMENT_TARGET = 26.2` (the app's), runpath
  `@executable_path/../Frameworks` plus the Homebrew paths the app uses.
- `RISE-GUI` depends on it and embeds it with a Copy Files phase "Embed rise-sim worker" (Executables →
  `Contents/MacOS/rise-sim`, Code Sign On Copy). With the M5b signing overrides it is ad hoc signed.

The bake manager finds `Contents/MacOS/rise-sim` first (beside the executable), so `RISE_SIM_WORKER` /
`SetWorkerPath` are not needed in the app. The probe step `worker` checks this with no override set.

**Shared-file note.** The project now has three Sources phases. `SourceHygieneTest` requires every library file in
every phase: a new library file must be added to all three (the `rise`, `RISE-GUI` and `rise-sim` phases).

**Release scripts** (not run):

- `scripts/create_macos_release.sh`: requires `Contents/MacOS/rise-sim`, passes it to `bundle_macos_runtime.sh`,
  admits it in the unprocessed-Mach-O scan, runs `verify_architectures` / `verify_dependencies` / `verify_rpaths` on
  it, and signs it (ad hoc, or Developer ID with hardened runtime) before the app. `OPENVDB_PREFIX` / `TBB_PREFIX` /
  `BLOSC_PREFIX`, when set, are passed to the build as `RISE_OPENVDB_PREFIX` / `RISE_TBB_PREFIX` /
  `RISE_BLOSC_PREFIX`.
- `scripts/bundle_macos_runtime.sh`: unchanged. It already takes any list of executables and already bundles
  OpenVDB/TBB/blosc for `rise-cache-import`.

**What a release still needs.**

- An OpenVDB (and its TBB/blosc) built for macOS 26.2. Homebrew's OpenVDB declares `minos 27.0`, so
  `verify_architectures` fails on both `rise-cache-import` and `rise-sim` at the 26.2 deployment target (this already
  applied to the importer). Stage version-pinned libraries and set the three prefixes, or raise the deployment target.
- A Developer ID identity and notarization; the worker is signed with the same identity and hardened runtime as the
  app. A sandboxed app would need the worker as an embedded helper with an inherited sandbox (the app is not
  sandboxed).
- Opto was not built with the new target.

## 3. Memory and cost estimates

The fields are the existing `Capabilities::memoryEstimate` (peak bytes) and `costEstimate` (wall seconds per
simulated second). They are not keyed. `SceneSimulationStatus::wallEstimate` = cost × (start + interval × (frames
− 1)), as a bake simulates from 0. `FormatSimulationEstimate` gives "≈ 77 min, 10.2 GB". The bake manager's memory
budget now counts both solvers.

**Cloth** (`ClothSolver::Configure`, the only cloth lines changed):

```
memoryEstimate = 1.25 × (10 MiB + 13 KiB × vertices)
costEstimate   = k × vertices × substeps × frame_rate,  k = 12 µs (bake), 3 µs (interactive)
```

Measured with `rise-sim bake cloth.shell` (peak footprint and RSS from `/usr/bin/time -l`), a sheet pinned at two
corners over a sphere (barrier contact), M1 Max, default threads (4):

| Vertices | Mode | Peak footprint | Estimate | Wall / simulated s | k per vertex-substep |
|---:|---|---:|---:|---:|---:|
| 121 | bake / interactive | 9.7 / 10.1 MB | 15 MB | | |
| 441 | bake / interactive | 15.5 / 15.3 MB | 20 MB | | |
| 1,681 | bake (4 s) | 28.1 MB | 41 MB | 3.0 | 6.8 µs |
| 1,681 | interactive (1 s) | 23.2 MB | 41 MB | 1.0 | 3.3 µs |
| 6,561 | bake (2 s) | 102 MB | 122 MB | 17.5 | 10.8 µs |
| 6,561 | interactive (4 s) | 77 MB | 122 MB | 1.15 | 2.5 µs |
| 14,641 | bake (0.23 s) | 157 MB (RSS 190) | 257 MB | 85 | 23 µs |
| 14,641 | interactive (0.23 s) | 187 MB (RSS 195) | 257 MB | 5.4 | 2.7 µs |

The per-vertex slope is 10–14 KiB. Wall time depends on contact and Newton iterations: bake k ranges 7–23 µs
(the stated 12 µs is −40 %/+90 %), interactive 2.5–3.3 µs (±30 %). Self-collision and meshes above 14,641 vertices
were not measured (the estimate extrapolates linearly).

**Fire** (`FireEstimateCapabilities` in `FireParameterSchema.cpp`; no solver run, no GPU):

- Memory: the production driver's host-memory admission reserve for the case grid,
  `FireProductionProjectedHeunMetalObserverWorkingSetBytes(shape, prescribedBottomFlux = true, observe = false)` +
  2 GiB, exactly what `ProductionOwnerObservationReserveBytes` + `FireRunHostMemory::ProductionPolicy` reserve for a
  scene bake. The owner certificate counts the certified 24-level EOS prefix table, 2.15 GB; fast mode builds only the
  10-level one (130 kB), so fast subtracts the difference.
  - Tier 8 certified: 10,201,038,848 bytes, equal to `next_step_reserve_bytes` in the admission record the running
    tier-8 bake wrote (`SimulationSceneTest` M checks it). Fast: 8.05 GB. Tier 6 and 10 use the capstone grids
    (52×52×80, 86×86×132).
  - This is the number the driver refuses to start without (it needs min(RAM/2, 32 GiB) above it), so the manager's
    default budget (half of RAM) agrees with it. It is an upper bound: measured peak footprints are 4.3 GB (tier 8
    certified, the running bake) and 2.2–2.4 GB (fast), 1.3–1.5 GB (tier 6 before r274).
- Cost: tier 8 measured in [FIRE_SMOKE_SPEED_R274.md](FIRE_SMOKE_SPEED_R274.md) (r275 table, quiet machine): 2324
  (certified) and 1121 (fast) wall s per simulated s. Tier 10 = 1.29 × tier 8 (the measured full-window ratio,
  r237/r251, 5571 / 4324 s per simulated s). Tier 6: unknown (0). Approximate: the fast full-window run with a
  2.5 ms step cap measured 720.
- The prediction path (fire is never configured in the app) gets them through the new
  `ParameterSchema::describeUnkeyed` hook, which also states the time support (frame origin and cadence; the panel
  showed "0–16 s @ 1 Hz" before). `FireSolver::Configure` calls the same function with its launch case's grid.

**Fire lines changed:** `Fire/FireSolver.cpp` +2 (an include inside the existing `RISE_ENABLE_OPENVDB` block and one
call after `capabilities.backend`); `Fire/FireParameterSchema.{h,cpp}` (the estimate function and the hook). Keys
and outputs cannot change: `Capabilities` and the cache time support are not inputs of `MakeCacheKey` (solver,
version, effective parameters, input identities). `SimulationSceneTest` M checks the predicted tier-8 key equals
the independent `MakeCacheKey` of the completed parameters, with 17 frames.

**Where it shows.** The Simulations panel row ("bake estimate ≈ 2 s, 13 MB" while no bake runs) and `sim_list`
(`memoryEstimate`, `costEstimate`, `wallEstimateSeconds`, `estimate`, `staleReason`).
`RISE_API_SceneEditController_GetSimulationEstimate` serves the panel.

## 4. Simulations panel polish

- The row buttons use the app's outline pill (`PillButtonStyle`, now internal): Theme text and border colours,
  legible in both appearances. The system `.bordered` style rendered dark labels on the dark panel.
- The cache path stays middle-truncated with the full path in the tooltip, and has a context menu: Copy path, Reveal
  in Finder (the nearest existing ancestor when the cache does not exist yet).
- A stale row shows the library's reason ("stale: parameters changed (substeps)").
- The probe renders the panel and the timeline in both appearances (`ui_<point>_{panel,timeline}_{dark,light}.png`
  in `rendered/simulation_m5/`) through an in-memory theme override that persists nothing.

## 5. Playback rate

**Before M5** (`RenderViewModel.startPreviewPlay`, commit 5015c4d39): a Swift task slept 33 ms and advanced one
authored frame (`(end − start) / (frames − 1)`), from the start, wrapping at the end. Every animation therefore
showed its authored frames at about 30 per second, whatever span they covered. M5b made the Play button real time
at the authored spacing, so a 15-frame, 1 s animation played in 1 s instead of 0.5 s.

**Rule now** (`SceneEditController::PlaybackPlay(fps ≤ 0)`, library): the clock runs on the timeline's frame grid.

- A scene with simulation assets (a declared simulation, a time-varying asset, or a live run) plays in real time.
- Any other scene steps its authored frames at 30 per wall second (`kFrameSteppingPlaybackFps`): the clock's new
  time scale is 30 / grid fps.

`PlaybackClock::Play` takes the time scale (scene seconds per wall second, default 1). `PlaybackStatus::timeScale`
and `RISE_API_SceneEditController_GetPlaybackTimeScale` report it. One difference from the old loop remains: Play
continues from the current time (from the start when at the end) rather than always restarting at the start.

## 6. Evidence

**Builds.** `make all tests` warning-free. Xcode Deployment (M5b §1 command, `-jobs 6`): RISE-GUI and the new
rise-sim target build with no compiler warnings; the only warnings are the known "built for newer version 27.x"
linker ones, now also for OpenVDB, TBB and blosc in rise-sim. The bundled worker is ad hoc signed, lists
`cloth.shell fire.methane`, and has runpaths `extlib/oidn`, `/opt/homebrew/lib`, `@executable_path/../Frameworks`.

**Library tests** (`tests/SimulationSceneLiveTest.cpp`, 59 checks; `tests/SimulationSceneTest.cpp` section M, 478
in all): the category end to end (enumerate, rows, a 21-vertex `vertices_u` row, `collider_object[0]` as a
reference offering `ball`, bake in process, edit `substeps` → stale "parameters changed (substeps)", undo →
complete with the original key, `pin[1]` occurrence edit and an absent occurrence refused, a collider resize →
"parameters changed (colliders)", a live run resynced by a panel edit and by undo); the clock at time scale 2
(1 s of scene in ≈0.5 s); `PlaybackPlay(0)` on a 10-frame, 3 s keyframed scene → fps 3, scale 10, ends in < 2 s; on
a simulation scene → scale 1; fire tier-8 reserve equal to the running bake's admission record; the fire key and
frame count unchanged; cloth formulas.

**App probes** (the bundled app, `RISE_SIM_WORKER` unset; `caches/flag` removed first):

| Run | Steps | Result |
|---|---|---|
| 1, untrusted | `worker,list,trust,live`, expect missing | 39/39 PASS |
| 2, trusted | `worker,list,bake,live,scrub,props,playrule` | 61/61 PASS: the bake ran the bundled `Contents/MacOS/rise-sim` (1.5 s); props: 52 rows, `follow_object[0]` kind Reference offering `flag_object`, `pole`; `substeps` 12 → stale "parameters changed (substeps)", no bake started, undo → complete; estimate "≈ 1 s, 29 MB" before the edit; playrule: scale 1 |
| 3, trusted | `list,edit` from complete | 24/24 PASS (the agent edit path still marks stale, starts nothing) |
| 4 | `playrule` on `scenes/Tests/Animation/animation_exposure.RISEscene`, expect stepping | 7/7 PASS: 15 frames over 1 s → grid 14 fps, time scale 2.1429, 30 frames shown in about 1 s |

**Renders**, `rendered/simulation_m5/ui_{after_bake,live,props_stale,stale_selected}_{panel,timeline}_{dark,light}.png`:
the buttons are legible in both appearances; the stale row shows its reason.

**Fast tier** (non-fire filter): 424 built, 424 passed (including `SourceHygieneTest` with the third Sources phase).

**Found on the way: the Inspector's kind enum was stale.** `PropertyKind` in `PropertiesPanel.swift` still used the
values from before `ValueKind::DoubleVec2` was inserted at 3, so every row from DoubleVec3 on was misrouted
(Reference rows, 10, fell back to plain text; Enum rows, 9, were drawn as reference chips; Vec3 rows got the Vec4
well). Fixed (raw values match `ChunkDescriptor.h` again). The Qt panel has the same problem (`p.kind == 9` in
`ViewportProperties.cpp`).

**Not verified.** No window was opened (offscreen renders only: context menu, hover, alert not seen). No fire was
listed, baked or played; the fire estimates were checked arithmetically and against the admission record only.
Opto and the release script were not run. Stop/Resume from the app was not exercised beyond M5b.

## 7. Windows (Qt) mirror, not edited

| Qt file (`build/VS2022/RISE-GUI/`) | Change |
|---|---|
| `ViewportBridge.{h,cpp}` | Category 12 in the int↔enum maps; `GetSimulationEstimate` (memory, wall, summary, stale reason); `GetPlaybackTimeScale` |
| `ViewportOutliner.{h,cpp}` | A "Simulations" category row (tag SIM) |
| `ViewportProperties.{h,cpp}` | The Reference test `p.kind == 9` must be 10 (`ValueKind::DoubleVec2` shifted the tail; Enum is 9); title/glyph for category 12; rows come through the existing per-category snapshot; `<field>[i]` rows and reference presets work as for painters |
| `SimulationsPanel.{h,cpp}` (the M5b mirror) | Row click also selects the simulation (`SetSelection(12, name)`); the estimate line; the stale reason; Copy path / Reveal in Explorer on the cache path; Theme-coloured buttons |
| `ViewportTimeline.{h,cpp}` | Nothing: `PlaybackPlay(0)` applies the rule in the library |
| `RISE-GUI.vcxproj` / packaging | A `rise-sim.exe` built from `tools/rise_sim.cpp` beside `RISE-GUI.exe` (no VS2022 project exists for it yet) |
