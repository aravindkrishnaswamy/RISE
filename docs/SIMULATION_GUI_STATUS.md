# Simulation in the apps: status of the application-facing side

2026-11-26, branch `simulation-liquid` from `763d654d6`; loose ends of §1.4 closed 2026-11-27 (from `895950cd7`, see §1.5). The simulation library (solvers `fire.methane`,
`cloth.shell`, `rigid.bodies`, `liquid.mac`; scene chunks; the bake manager; live stepping; caches) was built
and tested through the command line and unit tests. This note checks what the macOS app and the agent/chat
layer expose of it, records what was added, and records what was driven headlessly through the app's probes.
Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §4.5. Earlier app work:
[M5b](SIMULATION_M5B_RESULTS.md), [M5c](SIMULATION_M5C_RESULTS.md).

## 1. Inventory

"Before" is `763d654d6`; "added" is this pass.

### 1.1 macOS app (`build/XCode/rise/RISE-GUI/`)

| Capability | Where | State |
|---|---|---|
| List the scene's simulations | Simulations panel (`SimulationsPanel.swift`, model `SimulationsModel.swift`); rows for every `*_simulation`, `simulation_group` and `simulation_cache` | Before. Generic over solver: liquid, rigid, cloth and fire rows appear with no solver-specific code |
| Parameters with schema | Inspector, category "Simulation" (`SimulationRow` selects the entity); rows come from the solver's schema (`CstIntrospection`), repeatable fields as `<field>[i]`, reference pickers | Before. Group chunks resolve too (19 to 21 rows) |
| Bake start / stop / resume / rebake | Row buttons through the bake-manager C API; trust prompt for a refused cache location | Before. **Added:** a `simulation_group` row now has the buttons (it had none: `isProducer` was false for it) |
| Bake progress | Row text (state, frames, ETA, what a queued job waits for), baked-range track on the timeline | Before |
| Live start / stop / reset / record | Row buttons when `interactive` and a `simulated_mesh_geometry` is bound | Before |
| Cache status and location | Per-row state, frames, time support, estimate, stale reason; the directory with Copy path / Reveal in Finder | Before |
| Cache default location | The row shows the library's directory (`<base>/scene-caches/<scene>-<hash>/<name>`) | Before. **Added:** `RISEViewportBridge.simulationCacheBase()` (the base) for probes and later UI |
| Label `preview` / `predictive` | Row line 2 shows `row.fidelity` | Before: the bare word. **Added:** the sentence the label covers (from the cache manifest) with its validation-case count, and why `predictive` is not earnable (the solver's `fidelityLimit`), on the row and in its tooltip |
| Backend | Detail line "backend" from `capabilities.backend` | Before. Blank for fire (never configured in the app, so its backend is not stated); see §1.4 |
| Group members and ports | Members were shown as "upstream" | **Added:** the detail says "members" for a group and lists its `connect`, `owned` and `port_source` lines |
| Panel for a rigid or liquid solver | None needed (generic) | Verified by the probes (§3) |

### 1.2 C API and bridge

`RISE_API.h` "Simulation M3c / M4 / M5 / M5c" blocks, mirrored by `RISEViewportBridge` (`simulations`,
`simulationBakeJobs`, `liveAttach…`, trusted roots). **Added:** `RISE_API_SceneEditController_GetSimulationEvidence`
(label scope, case count, fidelity limit, connections) and `RISE_API_GetSimulationCacheBase`, with
`RISESimulationInfo.{evidenceScope, evidenceCases, fidelityLimit, connections}` and
`+[RISEViewportBridge simulationCacheBase]`.

Still not exposed to any shell: solver ports (what each member offers and accepts, as opposed to the group's
authored `connect` lines), the individual validation cases (ids, errors, tolerances), `earnableFidelity`.

### 1.3 Agent / RPC layer (`AgentSimulation.{h,cpp}`, `AgentRpc.cpp`, `AgentMcpAdapter.cpp`)

| Verb | Autonomy | State |
|---|---|---|
| `sim_list`, `sim_status`, `sim_wait` | read-safe | Before. **Added to `sim_list` rows:** `evidenceScope`, `evidenceCases`, `fidelityLimit`, `connections`, `meshConsumers` |
| `sim_bake`, `sim_cancel`, `sim_live_start`, `sim_live_stop`, `sim_seek` | commit, owner | Before |
| `sim_cache` | read-safe | **Added.** Cache base, whether the base comes from `RISE_SIMULATION_CACHE_DIR`, per-scene folder, trusted roots, and for each simulation: directory, exists, default location, whether the writer accepts it and why not |
| `sim_param_get` | read-safe | **Added.** The solver's schema (type, default, range, enum values, units, keyed) with the chunk's value and whether it is set; chunk-only fields with every occurrence |
| `sim_param_set` | commit, owner | **Added.** Schema-validated, then applied through `AgentSession::ProposePatch` |

Groups and coupling: a group bakes as one job (`sim_bake` of a member bakes its group); `sim_list` shows the
group as a row (`keyword simulation_group`, members in `upstream`). Fire's CPU backend is a scene parameter
since M8d (2026-11-26, M8 results §12.4): `backend cpu` on the `fire_simulation` chunk, keyed only as `cpu`, so
every existing fire scene keeps its key; the app and an agent select it through the chunk field like any other
(`sim_param_set`). There is no host-level override outside the key.

### 1.4 What is missing for the newer solvers

| Gap | Effect | Status |
|---|---|---|
| `transform_source` objects (rigid bodies drawn by `standard_object`) were keyframed from the cache **only when the scene was derived**; `RefreshSimulationFrames` rebound simulated meshes only | After an in-app bake of a rigid simulation or a group containing one, the bodies stayed at their authored pose until the scene was reopened. Found by the probe (§3.3) | **Fixed (2026-11-26).** `Job` keeps each driven object's body, authored pose and a stamp of the cache generation its keys came from; `RefreshSimulationFrames` calls `Job::RefreshTransformSources` under the render park, which drops the object's `position`/`orientation` timelines (`Animator::RemoveKeyframes`), restores the authored pose and re-derives the keys through the scene-load code, then re-poses the scene at the current time (invalidating the top-level structure and per-object state, as a scrub does). A bake that advances or finishes, a new epoch, and a cache that is gone (authored pose again) are all covered. The panel's "reopen the scene" notice (`reloadSuggestion`) is removed. Live in-process rigid runs drove nothing then; **fixed 2026-11-27**, §1.5. Tests: `SimulationBakeManagerTest` B; the probe's body-pose checks (§3.1) |
| A group member's stale reason is "configuration changed" | A member baked by a group has `rise-sim.group.cbor`, not a job record | **Fixed (2026-11-27)**, §1.5 |
| Fire row has no backend and no label before a bake | Expected (prediction path) | The backend is now a scene parameter (`backend`, above; M8d); the row's backend detail still comes from `capabilities.backend`, which a predicted fire does not state, so it stays blank before a bake (not changed) |
| Group rows show one member's evidence | `Describe` keeps the last `preview` member's label for the group | Left: matches the contract (a group is as good as its weakest member) |
| Liquid `interactive:true` with a bound mesh offers **Live** | Live stepping of a liquid was not exercised (§3) | Not verified |
| A cache deleted outside the bake manager | Noticed only at the next bake event; frames and poses stayed | **Fixed (2026-11-27)**, §1.5 |

### 1.5 Loose ends closed 2026-11-27 (from `895950cd7`)

**Cache deleted outside the manager.** Before, `DescribeSimulations` already read the disk (so a panel refresh
showed "missing"), but nothing triggered that refresh, the manager kept a `finished` job, and a bound mesh kept
serving frames (from its prepared-frame memo, or an error that left the old frame installed). Now:

- `SimulationBakeManager::VerifyCaches()`: one `stat` of `manifest.cbor` for the newest finished (or stopped with
  frames) job of each cache directory, confirmed with `InspectCache` only when the manifest is absent (a cache a
  worker is writing is never called gone). A hit sets `BakeJob::cacheMissing`, clears `resumable`, writes a `lastError`
  ("... was deleted from disk; bake the simulation again"), and moves the change counter and the scene's frames
  generation. The monitor thread runs it at most once a second (no new thread); `Attach` (every panel list) runs it
  at once. The app's existing 2 Hz counter poll then re-reads the rows (state "missing") and calls
  `RefreshSimulationFrames`.
- `RefreshSimulationFrames` unbinds a disk-bound mesh whose cache is gone (hidden, not stale frames) and reports
  "the simulation cache ... was deleted from disk; bake `<sim>` again" in its error; transform-source objects already
  returned to their authored pose.
- Playback: `SimulatedMeshGeometry::PrepareAssetForRender` stats the manifest of a disk-bound source and fails with
  the same message (a render or scrub reports it) instead of serving frames that no longer exist.
- Test: `SimulationBakeManagerTest` D.

**Group member stale reasons.** A group's recorded members now carry `solver`, `version` and the effective
`parameters` in `rise-sim.group.cbor` (caches baked before this keep "configuration changed"). `Describe` derives, per
member: "solver version changed", "parameters changed (<names>)" for its own configuration; "member <other>: parameters
changed (<names>)" when another member's differs; "coupling changed (connections, policy or adapters)" when the group
identity differs with all members equal; "scene input changed (<inputs>)". The group row lists its stale members'
reasons ("drop: ...; sheet: member drop: ..."). Test: `SimulationGroupSceneTest` (rigid mass edit, cloth substeps edit,
group tolerance edit).

**Live rigid stepping.** Why it drove nothing: `LiveSimulation` was built around a `simulated_mesh_geometry`
(`PrepareSceneLive_` refused a rigid solver with "no simulated_mesh_geometry binds ..."), and rigid output is
`transform.v1` for objects, whose keyframes came only from a cache on disk. Now `rigid.bodies` (any transform.v1
producer) runs live through the same `InProcessHost`/`LiveSimulation` attach, play, reset, detach, resync and playback
clock limit as cloth, addressed by its first follower object's name (`SceneSimulationStatus::meshConsumers` lists the
followers of a transform producer, so the unchanged app shows Live / Reset / Record for it). `Job::SetLiveTransformProvider`
makes `transform_source` keys derive from the run's in-memory publication; `OnTimeScrub` folds frames that landed since
the last scrub in before setting the time (cheap: only when a live transform run exists), detach goes back to the disk
cache or the authored pose. Test: `SimulationSceneLiveTest` "live rigid bodies" (free fall matches y = y0 - g t^2/2, the
object moves only along gravity, reset, edit restarts the run, detach restores the authored pose). Costs/limits: keys
are re-derived from all published frames at each scrub that sees new frames (linear in frames); a group containing a
rigid member has no live mode; `Record` for a transform run uses the generic recorder, not exercised.

## 2. In-app chat tool catalog

The chat catalog is `kToolDefs` in `AgentChatCodecs.cpp` (one provider-neutral table; the Anthropic, OpenAI,
xAI and Gemini codecs render it). It had **no** simulation verbs; only the MCP adapter (`AgentMcpAdapter.cpp`)
and the dispatcher had them. Parameter edits were possible only through `propose_patch` with no schema check.

**Added to `kToolDefs`** (before `ask_user`): `sim_list`, `sim_status`, `sim_wait`, `sim_bake`, `sim_cancel`,
`sim_live_start`, `sim_live_stop`, `sim_seek`, `sim_cache`, `sim_param_get`, `sim_param_set` (eleven). **Added to the
MCP adapter and dispatcher:** `sim_cache`, `sim_param_get`, `sim_param_set`.

Rules followed:

- Declared the way existing verbs are: a `NeutralToolDef` per verb whose text mirrors the MCP entry (shorter,
  because it ships with every API call); the verb names map one to one onto dispatcher verbs.
- Read-only verbs (`sim_list`, `sim_status`, `sim_wait`, `sim_cache`, `sim_param_get`) are on `IsReadSafeVerb`.
- Mutating verbs are on neither the read nor the propose allowlist, so they are refused under Read and Propose,
  and for an external session (owner only) like `sim_bake`. `sim_param_set` validates first, then calls
  `AgentSession::ProposePatch` with the target's kind `simulation`: the same undoable, dirty-marking,
  re-deriving edit `propose_patch` makes (the probe's Undo reverts it). It starts no bake.
- Chat guard rails in the texts: `sim_wait` asks for `timeoutMs` of 2000 or less (the chat runs tools on the
  main thread); `sim_bake` says not to bake `fire_simulation` unprompted.
- The chat's retry gate (`ChatViewModel.retriableEditVerbs`) is derived by `SourceHygieneTest` from
  `IsProposeSafeVerb`; `sim_param_set` is not on it, so a busy-editor refusal is reported (`retriable` is in the
  result) rather than retried by the driver.

**Tests.** `SimulationBakeManagerTest` section V (RPC end to end, headless): 141 checks, 0 failed, of which the new ones cover
evidence fields and consumers in `sim_list`; `sim_cache` (base, trusted roots, directory, exists, accepted);
`sim_param_get` (schema of `substeps`, every parameter, the `pin` occurrences, an unknown parameter and an
unknown simulation, each naming what exists); `sim_param_set` (range refusal naming `[1, 100000]`, type refusal,
unknown parameter, absent occurrence, missing params, none of which changes the document), an applied edit
(stale with reason "parameters changed (substeps)", dirty, no bake started, read back), a `pin` occurrence edit,
and the way back to a current cache; the reads under Read autonomy and `sim_param_set` refused under it.
Catalog pin tests: see §4.

## 3. Probes

The macOS app's headless probe is `RISE_GUI_HEADLESS_PROBE=simulation_m5` (`App/SimulationProbes.swift`): it
runs before SwiftUI, drives the real `RenderViewModel` / `SimulationsModel` / `RISEViewportBridge` stack (scene
load, viewport render thread, library clock, bake manager, bundled `rise-sim`) and needs **no window server**.
It ran here without one. **Added:** `RISE_GUI_HEADLESS_PROBE=simulation_scenes`, a scene-generic probe, plus a
label-and-evidence check in the existing bake step. Variables and steps are in the file's header comment.

Build: `xcodebuild -project rise.xcodeproj -scheme RISE-GUI -configuration Development -arch arm64
-derivedDataPath /Volumes/SSD/claude-tmp/gui-scratch/dd -jobs 4 'OTHER_SWIFT_FLAGS=$(inherited) -Xfrontend -sdk
-Xfrontend /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk' CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual
DEVELOPMENT_TEAM= CODE_SIGNING_REQUIRED=NO build`: succeeded, no compiler warnings (only the known linker
"built for newer macOS" and SwiftUICore notices). Run environment, every run: `HOME`, `RISE_SIMULATION_CACHE_DIR`,
`RISE_SIMULATION_TRUSTED_ROOTS` and `RISE_SIM_GPU_LOCK` under `/Volumes/SSD/claude-tmp/gui-scratch/` (the bake
manager's job logs and the GPU lock live under the per-user root, so `HOME` had to move too); nothing under the
real `~/Library/Caches/RISE` or `~/Library/Preferences` changed during the runs.

Scenes at reduced size are copies in the scratch (`frames 4`; resolution 16 for the dam break, 24 for the coupled
scenes); the committed scenes are untouched. Wall times are on a machine loaded by other work (load 9 to 14).

### 3.1 Results, scene by scene

| Scene | Steps | Checks | What ran |
|---|---|---|---|
| `cloth_flag_scene` (`flag`, cloth) | `worker,list,bake,live,scrub,props,playrule`, then `list,edit` | 62 and 24 PASS | Bake through the bundled worker (15.6 s loaded); live run (attach 0.22 s, 38 of 91 frames in 2 s under load, 90 solver waits, live frames equal baked at t = 0.5); scrub; props (53 rows); play rule; edit makes it stale and starts nothing. Label `preview`, evidence text shown |
| `liquid_dam_break` (`pool`, liquid, res 16, 4 frames) | `list,props,bake,show,agent,ui` | 33 PASS | Bake 35.7 s; label `predictive` with its evidence sentence; surface mesh visible (differs from the unbaked view by 0.019, from first to last frame by 0.008 mean luminance); in-app `sim_*` verbs incl. validated `sim_param_set` and Undo |
| `liquid_rigid_bodies` (`drop` rigid + `pool` liquid, group `tank`, res 24, 4 frames) | same | 46 PASS (an earlier run had 1 FAIL: the same-process motion check; after the 2026-11-26 fix, 55 checks pass, see below) | Group row with members `pool, drop` and its four coupling lines; the group's Inspector fields; one group bake (64 s); label `preview`, rigid evidence. The earlier failing check was the defect of §1.4 (fixed): after the in-app bake the bodies now move |
| same, second process over the baked caches | `list,show` | all pass | Bodies move (first vs last 0.0055) |
| same scene, after the fix (`RISE_GUI_HEADLESS_PROBE_BAKE=tank`, hermetic: `HOME`, cache dir, trusted roots, GPU lock under a scratch) | `list,props,bake,show,agent,ui` | ALL PASS | The probe poses `buoy`, `crate`, `anchor` at the last frame's time before the bake (authored pose), bakes the group (76.5 s), and with no reopen and no scrub the three bodies are at the cache's pose (they moved 0.005 to 0.05 m); at the first frame they are at the authored pose again. The first-to-last viewport check (0.0055) is now unconditional |
| `liquid_cloth_catch` (group `catch`: `cloth` + `pool`) | same | 44 PASS | Group bake 59.9 s; couplings `cloth.surface_state -> pool.sheet_state` … `owned pool cloth`; both meshes shown; `preview` with the cloth's evidence |
| `liquid_cloth_wave` (group `flume`) | same | 44 PASS | Group bake 44.2 s; as above |
| `fire_campfire_scene` (`campfire`) | `list,props,ui` | 10 PASS | Loads; row missing, 0 of 33 frames, estimate "≈ 4.7 GB", cache in the default location, backend blank, Inspector 29 rows. **Not baked or stepped**; the probe refuses to bake a fire simulation |
| Live cloth flag | `live` step of the flag run | included above | As above |

The cache directory shown was in every run `<base>/scene-caches/<scene>-<16 hex>/<name>` with the base being
`RISE_SIMULATION_CACHE_DIR`. Rendered panel and timeline images (both appearances) and viewport frames are in
`/Volumes/SSD/claude-tmp/gui-scratch/out/`.

### 3.2 Not verified without a person at a screen

- Layout in the real 344 pt column: wrapping of the new evidence lines, the group row's buttons, the
  (the "reopen the scene" notice is gone); only offscreen `ImageRenderer` renders were looked at.
- The trust prompt (an alert), hover tooltips, context menus, keyboard flows, the live viewport's look in motion.
- Live stepping of a liquid, and a group's live behaviour (only the cloth flag was run live in the app probe).
- Live rigid stepping in the real app (2026-11-27): verified at library level only (`SimulationSceneLiveTest`); the Live
  button's appearance for a rigid row (its "shown by" line now names an object), the motion in the viewport while playing,
  and the app probe were not run. The Xcode project was not rebuilt (no Swift or bridge change).
- Deleting a cache in Finder while the app shows it: the library path is tested; the row turning to "missing" within
  about a second, and the viewport dropping the frames, were not watched on screen.
- Group-member reason text in the real panel (wrapping of the longer strings).
- Any fire bake, live run or render (reserved).
- The in-app chat with a real model: the catalog is tested by the codec tests and the in-app dispatcher by the probe,
  but no provider call was made.

## 4. Pins touched

- **Chunk keyword count 191** (`SceneEditorSuggestionsTest`) and **`RewireConnectionTest` 6**: not touched, no chunk
  keyword or rewire rule was added.
- **Chat tool table**, `kToolDefs`: 38 to **49** in `AgentChatLoopTest` (OpenAI, xAI, Anthropic, Gemini bodies) and
  `AgentSkillsTest` (three providers).
- **MCP `tools/list`**: 51 to **54** in `AgentMcpAdapterTest` (three counts and the expected-names list),
  `AgentAutonomyPolicyTest` (three counts) and `AgentMcpStdioSmokeTest`.
- **Read-refusal annotation count** under Read in `AgentAutonomyPolicyTest`: 28 to **29** (`sim_param_set`).
- `SourceHygieneTest` agent verb-set parity: `AgentRpc.h` restates the read-safe list in two places; both now
  include `sim_cache` and `sim_param_get`.
- No new library files, so the five build lists are unchanged.

## 5. Windows GUI mirror (by inspection; not implemented)

`build/VS2022/RISE-GUI/` has **no simulation code at all**: no file under it mentions simulation, the playback
clock or a bake. Relative to the macOS app it lacks:

- **Startup:** `RegisterBuiltinSolvers`, and re-applying persisted trusted cache roots (`MainWindow`).
- **Bridge (`ViewportBridge.{h,cpp}`):** the `Category` enum stops at `Geometry = 11` (no `Simulation = 12`);
  forwards for playback (`PlaybackPlay/Pause/Seek/GetPlaybackStatus`), time-varying assets, simulations and
  their details, estimate, evidence and cache base, live runs, bake manager (start, resume, stop, jobs, change
  counter, `RefreshSimulationFrames`), trusted roots and cache location checks.
- **Panel:** no Simulations dock (rows, state, label and evidence, Bake / Stop / Resume, Live / Reset / Record,
  trust prompt, Copy path / Reveal in Explorer).
- **Timeline (`ViewportTimeline.cpp`):** still the 33 ms `m_playTimer` stepping one frame per tick (`onPlayTick`)
  rather than the library clock with the 20 Hz status poll; no baked-range track; no `LastSceneTime` read-back
  after a scrub.
- **Outliner and Inspector (`OutlinerWidget`, `ViewportProperties`):** no "Simulations" category (tag SIM), no
  title or glyph for it; the Reference test is `p.kind == 9` in three places where the kind enum now has
  Reference at 10 (a defect shared with any category with reference rows; see M5c §6).
- **Packaging:** no `rise-sim.exe` target (no VS2022 project for `tools/rise_sim.cpp`), so `sim_bake` cannot
  find a worker on Windows.
- **Chat (`ChatPanel.cpp`):** it uses the shared `AgentChatLoop`, so the eleven new verbs are advertised there
  too. `sim_bake` will answer that no worker is found; `sim_live_start` reaches a controller but the Qt timeline
  does not poll the library clock; the read verbs work. The cache writer has no ancestor policy on Windows, so
  the trust prompt would never show.

## 6. Reproduce

```sh
S=/Volumes/SSD/claude-tmp/gui-scratch; R=<repo>
# scenes: copies of scenes/Tests/Simulation/*.RISEscene with `frames 4` and a small `resolution` in $S/scenes
env HOME=$S/home RISE_MEDIA_PATH=$R/ RISE_SIMULATION_CACHE_DIR=$S/caches RISE_SIMULATION_TRUSTED_ROOTS=$S \
  RISE_SIM_GPU_LOCK=$S/home/gpu.lock RISE_GUI_HEADLESS_PROBE=simulation_scenes \
  RISE_GUI_HEADLESS_PROBE_SCENE=$S/scenes/liquid_dam_break.RISEscene \
  RISE_GUI_HEADLESS_PROBE_STEPS=list,props,bake,show,agent,ui \
  RISE_GUI_HEADLESS_PROBE_ROWS=pool=liquid_simulation:liquid.mac:missing \
  RISE_GUI_HEADLESS_PROBE_OUT=$S/out/dam $S/dd/Build/Products/Development/RISE-GUI.app/Contents/MacOS/RISE-GUI
# groups: RISE_GUI_HEADLESS_PROBE_BAKE=<group name>   (a member bakes its group; do not name every member)
```

Each run prints `SimulationProbe: ALL PASS` and exits 0, or `FAILURE` and exits 1.
