# Fire simulation in the scene format and UI: proposal

2026-11-22. **Proposal for owner review. Nothing here is built** except follow-mode rendering (phase 0), which is
in progress separately. Sources: a read of the GUI, job, timeline, scene-format and agent code, and
`FIRE_SMOKE_DESIGN.md` §3.9, §7.3, §8, §9.

## Recommendation

Treat a fire as a **bake with a cache**, run by a **separate simulator process** that the UI starts and watches:

- The scene describes the fire (`fire_simulation`) and the medium plays back its cache (`fire_medium`).
- A simulator executable writes frames and checkpoints into the cache. The renderer and UI only ever read the cache.
- Frames are usable as soon as they are written, so the timeline fills in while the bake runs.

This matches the existing design (§7.3: "standalone offline tool under `tools/`, not linked into the renderer") and
the non-goal at line 74 (no real-time simulation; the renderer consumes cached grids).

## What exists today

| Area | State |
|---|---|
| Simulator | No library API. The stepping loop, checkpoints, frame export and manifest sealing are about 5–6k lines inside `tests/FireSequenceTest.cpp` (`RunMethaneFrameProbe` and friends). The case is hard-coded (0.30 m pool, 33 kW) and tuned by about 97 `RISE_FIRE_*` environment variables. `tools/fire_simulator.cpp` is a 205-line bring-up. |
| Case description | `FireCase.h` already has the authored case (`AuthoredV1`: fuel, pool or patch, HRR or mass flux, envelope, duration, quality, seed, output rate) and what is derived from it. §3.9 calls this "final as case schema v1". No scene chunk uses it. |
| Playback | `fire_medium { sequence_manifest … }` works in the command-line renderer. Scene load fails if the manifest is missing or incomplete, and `PreflightAllFrames` hashes every frame at load. Time mapping comes from the manifest (`MapSceneTime`), not the chunk. |
| GUI apps | macOS (SwiftUI) and Windows (Qt) share `SceneEditController` through the C API. Neither has fire code. **The macOS app cannot load a fire sequence today:** OpenVDB is enabled only in the make build. |
| Long-running work | One render slot, with progress and cancel through `IProgressCallback`. There is no precedent for a multi-hour background task, and no on-disk cache with invalidation other than the fire manifest itself. |
| Timeline | The scrubber appears when the scene has keyframed elements. A fire medium has none, so a fire-only scene probably shows no scrubber (inferred, not tested). |
| Platforms | The solver is Metal only; other builds compile stubs. The fp64 CPU path is a reference oracle, not a throughput path. |
| Agent | `render`, `render_status`, `render_wait`, `render_cancel` exist; a bake could mirror them. |

Cost at the standard grid (tier 8), 25 s of fire: certified about 16 h, fast mode about 5–6 h (r275, projected from
short samples). Rendering is about 14 s per frame at 256×384.

## Why a separate process

- **It is what the code already is.** The solver owns the GPU, holds its state there, and is written to run alone.
  Receipts record an exclusive-host rule.
- **A bake outlives the app.** Hours-long work should survive closing a document or a crash of the UI, and should
  be resumable from its checkpoints. A child process with a cache directory gives that for free.
- **It keeps the single render slot simple.** The controller needs no second kind of job; it needs a bake monitor.
- **The alternative** (solver as a library job inside the app) means extracting the same code *and* solving GPU
  sharing, cancellation and crash isolation in-process. It buys nothing the cache does not.

## Design

### 1. Scene

```
fire_simulation
{
    name        campfire
    fuel        methane
    source      pool 0.30            # diameter, m
    intensity   hrr 33               # kW
    duration    25
    quality     standard             # draft | standard | high
    mode        fast                 # fast | certified
    cache       caches/campfire      # relative to the scene file
}

fire_medium
{
    name        campfire_medium
    simulation  campfire             # instead of sequence_manifest
    ...
}
```

- `fire_simulation` is a data-only chunk (the `hair_guides` chunk is the precedent) whose fields are the existing
  `FireCase::AuthoredV1`, plus the options that are environment variables today and belong to the case (soot yield,
  radiation closure, mode).
- `sequence_manifest <path>` stays valid for pre-baked sequences.
- Placement, scale and time offset of the fire in the scene stay on `fire_medium`, so moving the fire or the camera
  never invalidates the cache.

### 2. Cache

- A directory holding frames, checkpoints, an incremental manifest and a small status file.
- Keyed by the case record id (already a hash of the authored and derived case), the solver options and the
  producer build. States: `missing`, `baking`, `partial`, `complete`, `stale` (key no longer matches the scene).
- A stale cache still plays back, marked stale, until the user re-bakes. Look development should never be blocked.

### 3. Loading a partial or missing cache

Needs three changes to the renderer, all behind the existing fail-closed rule for predictive sequences:

- An incremental manifest the solver appends to (phase 0 is adding this).
- Verify a frame when it is first used, not all frames at load.
- A missing frame renders as empty space with a warning in preview fidelity, and remains a hard error in predictive
  fidelity.

### 4. Simulator executable

Extract the driver from the test file into `tools/fire_simulator` with a case file (or scene + simulation name) as
input, a cache directory as output, and three verbs: `bake`, `resume`, `status`. Progress is the status file plus
the existing step log. Cancel is a signal that writes a checkpoint and exits. First acceptance test: byte-identical
frames and checkpoints against the current harness on the same case.

### 5. UI (macOS first)

- **Bake panel** on a selected `fire_simulation`: state, simulated time reached, estimated time left, frames
  written, Bake / Stop / Resume. This is custom UI: chunk descriptors have no "action" kind.
- **Timeline:** show the scrubber for any scene with a fire medium, with the baked range drawn on it. Scrubbing
  into the unbaked range shows the last baked frame, dimmed.
- **Viewport:** the latest baked frame while a bake runs. Whether the interactive rasterizer honours fire media at
  all is not yet known and must be checked first.
- **Edits:** changing a simulation field marks the cache stale through the existing parameter-edit path; nothing
  re-simulates until the user asks.
- **Render during a bake:** allowed. The path tracer is CPU-side; what it costs the solver is being measured by
  the phase-0 follow run.

### 6. Agent

`bake`, `bake_status`, `bake_wait`, `bake_cancel`, mirroring the render verbs.

### 7. Quality presets

| Preset | Grid | Mode | 25 s of fire (estimate) | Use |
|---|---|---|---|---|
| Draft | tier 6 | fast | about 1.5–2 h | blocking, timing, look |
| Standard | tier 8 | fast | about 5–6 h | finals |
| Certified | tier 8 or 10 | certified | 16 h and up | validation, reference |

The draft figure is scaled from tier 8 by cell count and step size, not measured.

## Phases

| Phase | Work | Check |
|---|---|---|
| 0 (in progress) | Follow-mode rendering script, incremental manifest | Frames of a live run rendered as they appear |
| 1 | Extract the driver into `tools/fire_simulator`; case file input; `bake` / `resume` / `status` | Byte-identical output against the test harness |
| 2 | Partial-sequence loading; per-frame verification; fire frames readable by the GUI build | A half-baked cache loads and renders in the macOS app |
| 3 | `fire_simulation` chunk, cache keying and states, `fire_medium simulation <name>`, command-line `bake` | Scene round-trips; stale detection test |
| 4 | macOS bake panel, timeline range, viewport frame | End-to-end bake from the app |
| 5 | Agent verbs; Windows panel (playback and monitoring only) | |

Phase 1 is the large one: the driver is entangled with test scaffolding and environment variables. Phases 2 and 3
are moderate. I have not estimated them in days.

## Decisions for the owner

1. **Separate process or in-library job.** Recommended: separate process, for the reasons above.
2. **How the GUI reads frames.** Either ship OpenVDB with the apps, or add a small native frame format written
   alongside the VDB so the apps need no new dependency. Recommended: native format; it also removes OpenVDB from
   the render path on Windows and Android.
3. **Default mode in the UI.** Recommended: fast, labelled uncertified, with certified as an explicit choice.
   Today both produce `preview_only` sequences in any case.
4. **Which environment options become case fields**, and which stay developer-only.
5. **Where caches live** by default: beside the scene (recommended) or in a per-user cache directory.
6. **Windows and Android.** Recommended: playback of baked caches only; simulation stays on the Mac until there is
   a non-Metal solver.

## Open questions

- Does the interactive viewport rasterizer render fire media, and does it use the same prepare-and-freeze path?
- How much does a concurrent render slow the solver, and does OIDN compete for the GPU?
- Whether the fuel list beyond methane is ready to expose (the soot yield is currently an opt-in fit).
- Long-run behaviour of fast mode and the larger time step (the r275 validation run).
