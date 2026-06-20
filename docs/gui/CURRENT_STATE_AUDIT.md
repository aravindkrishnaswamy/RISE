# RISE GUI / Editor Stack — Current-State Capability Audit

**Status:** GROUND TRUTH (code-verified, 2026-06-19). This document supersedes
any `Status:` header in the seven `docs/gui/` specs and `docs/GUI_ROADMAP.md`
where they disagree. It was produced by reading `src/` and `tests/` directly —
**plan-doc status headers were treated as suspect and re-derived from code.**
**Owner:** Aravind Krishnaswamy
**Scope:** The interactive scene-editor / GUI substrate: round-trip save, entity
creation, introspection, property editing, region render, picking, environment,
cameras, present/display, the auto-router probe surface, parse/validate, and the
MCP/LLM surface. Every claim cites `file:line`. `.claude/worktrees/` was ignored.

## Capability-state legend

Each capability is scored on three axes:

- **(L)** implemented in the shared C++ library (`src/Library/`)
- **(G)** actually wired into a platform GUI (macOS `build/XCode/rise/RISE-GUI`,
  Windows `build/VS2022/RISE-GUI`, Android `android/app`)
- **(A)** absent everywhere

Many capabilities are **L-but-not-G** (library has it, no GUI calls it) or
**partially-G** (some platforms wire it, others don't).

---

## Executive summary (read this first)

1. **Round-trip scene save IS implemented** — not "pending / Phase-0 future" as
   four of the seven specs claim. Two-mode engine
   (`src/Library/SceneEditor/SaveEngine.cpp`, 1,898 lines, "Phase 6.4"): Mode A
   in-place byte splice + Mode B managed-override block, **plus** Phase B
   property re-emit and Phase C created-entity emit. Heavily tested
   (`tests/SaveEngineTest.cpp`, 41 tests; `tests/SceneEditControllerSaveTest.cpp`).

2. **Phase C created-entity persistence is CAMERAS ONLY** — confirmed at
   `SaveEngine.cpp:1351` ("V1: only cameras are creatable"), filter at `:1362`
   and `:1373` (non-camera chunk inside the managed block → **Refused**). Hand-
   pasted non-camera chunks in the block are a hard refusal, not silent drop.

3. **The only entity-creation path anywhere is `CloneActiveCamera`** — clone the
   active camera (`SceneEditController.cpp:3103`; the lone creation op
   `SceneEdit::AddCamera`, `SceneEdit.h:212`). There is **no add / delete /
   import** for objects, lights, materials, or media in the controller or any GUI.

4. **Picking is SHARED C++, and it IS wired on desktop** — `SceneEditController::PickAt`
   (`SceneEditController.cpp:2322`) casts the ray, intersects, resolves the
   object name, routes through `SetSelection`. Only raw pointer-event capture is
   platform-specific (macOS `ViewportView.swift:83-91` → `bridge.pointerDown` →
   controller). The "hit-testing is irreducibly per-platform" claim in
   APPROACHABILITY_FOUNDATION is **wrong**.

5. **Round-trip save is wired into macOS + Windows GUIs, NOT Android** —
   macOS `RISEViewportBridge.mm:657` (`saveScene`), Windows
   `ViewportBridge.cpp:402` (`saveSceneTo`). Android's only "save" is
   `nativeSaveAs` (`rise_jni.cpp:166`) which encodes the rendered **image**
   (TGA/PPM/EXR via the FrameStore encoder), not the scene — no scene round-trip
   on Android.

6. **Region/ROI render: library supports it; NO GUI wires it** —
   `Job::RasterizeRegion` (`Job.cpp:8806`) builds a `Rect` and passes it.
   All pixel integrators honor `pRect` via `PixelBasedRasterizerHelper::BoundsFromRect`
   (`PixelBasedRasterizerHelper.h:308`). **MLT (Pel + spectral) ignores `pRect`**
   (`MLTRasterizer.cpp:1226`, parameter commented out) — region render silently
   no-ops under MLT. No GUI exposes a region; every controller `RasterizeScene`
   call passes `pRect = nullptr`.

7. **Descriptor / `ValueKind` does NOT encode a color-vs-scalar "pipe"** —
   confirmed. Enum has Bool/UInt/Double/DoubleVec3/DoubleVec4/DoubleMat4/String/
   Filename/Enum/Reference (`ChunkDescriptor.h:110`), no painter-type field.
   Color (`IPainter`) vs physical-scalar (`IScalarPainter`) routing is hardcoded
   downstream in `Job::ResolveOrDiagnoseScalar` (`Job.cpp:2718`) and
   `MaterialIntrospection` slot dispatch.

8. **Introspection covers 7 entity families with edit + read-only split** —
   Camera (4 types), Light (4 types), Material (20+ types; composed materials
   read-only), Media (Homogeneous editable, Heterogeneous read-only), Object
   (standard_object), Film, Rasterizer. This is the substrate for the property
   panels and a future MCP schema. Well-tested (Phase4DescriptorDriven, Light/
   Media full-coverage, Film, ThinFilm).

9. **Environment / HDRI swap is L-but-not-G** — `SetGlobalRadianceMap`
   (`IScenePriv.h:131`) **swaps** the map; `GetGlobalRadianceMapMutable`
   (`IScenePriv.h:259`) gives mutable access to **scale** the existing one
   (backs `radiance_scale`). Neither is wired into any GUI; no HDRI-load UI.

10. **RISE is a CPU path tracer — no GPU render path.** Present surfaces differ:
    macOS CAMetalLayer with EDR (`MetalEDRView.swift`), Windows DXGI swap chain
    fp16 scRGB HDR (`HDRRenderWidget.cpp`), Android CPU RGBA8 ByteBuffer →
    Compose `ImageBitmap` (SDR only). The Metal/DXGI layers display a
    CPU-computed framebuffer; they do not render the scene.

11. **Auto-router probe: only scalars are exposed externally** —
    `ResolvedIntegratorName()` / `ResolveReason()` / `LastProbeSeconds()` /
    `LastProbeRenders()` (`AutoRasterizer.h:151-168`). The per-pixel
    luminance/variance `ProbeResult` (`AutoRasterizer.h:192`) is a **private**
    struct — there is **no heatmap surface today**. SPECTRAL_DIFFERENTIATORS D5
    states this correctly; MCP_TOOL_SURFACE's `rise://render/variance` overstates it.

12. **No side-effect-free parse/validate path exists** — parsing constructs
    Job/Scene entities immediately as chunks finalize
    (`AsciiSceneParser.cpp:9867`, `ParseChunk` → `Finalize` → `pJob.Add*`). The
    command `> quit` calls `exit(1)` (`AsciiCommandParser.cpp:182`); `render`,
    `load`, `run` all have hard side effects. **`Job` does NOT retain scene
    source text** — `SourceSpanIndex` stores byte offsets only; SaveEngine
    re-reads the file from disk at save time.

13. **MCP server + LLM agent runtime are ENTIRELY ABSENT (greenfield)** — no
    `src/Library/MCP`, no agent runtime, no JSON tool surface, no provider
    adapters. (`src/DRISE/MCPClientConnection.*` is "Master Control Program"
    distributed-render plumbing, unrelated to Model Context Protocol.) All AI
    specs are net-new.

14. **The dominant stale assumption across the specs is "round-trip save is
    unbuilt."** It contaminates CAMERAS_AND_VIEWS (worst — "designed-not-built",
    "won't survive reload" camera warning), MATERIAL_EDITOR, LLM_AGENT_RUNTIME,
    and MCP_TOOL_SURFACE (entire "needs RT-save" legend). Only GUI_ROADMAP §2 was
    self-corrected (2026-06-19). The punch-list at the end reconciles the rest.

---

## 1. Round-trip save — by mode and entity type

**Architecture.** `SaveEngine` (`SaveEngine.h:83`) takes borrowed
`IJobPriv`, `SourceSpanIndex`, `OverrideSpanIndex`, two `TransformSnapshot`
(base + loaded), a `DirtyTracker`, and the scale-from-anchor set. `Save(filePath)`
returns a `SaveResult` with four states: **Saved / NoOp / Refused / Failed**
(`SaveEngine.h:45`). The engine **re-reads the source file from disk** at save
time (`SaveEngine.cpp:869`) — it does not keep an in-memory copy; correctness
signal is byte-identity, not a counter.

**Two modes** (`SaveEngine.h:8-18`):
- **Mode A** — in-place line rewrite: splices the new value into the file's bytes
  for direct, Euler-authored, single-visit, non-shadowed, decomposable parameter
  lines.
- **Mode B** — managed `override_object` block at a sentinel-bracketed offset, for
  entities the source can't represent in-place (FOR-generated, matrix/quaternion-
  authored, ScaleObjectFromAnchor-touched, shadowed).

| Pass | What it saves | Entity types | Code |
|---|---|---|---|
| **Transforms (Mode A/B)** | position/orientation/quaternion/scale/matrix | Object (transforms), Camera (transform params splice as Mode A) | `SaveEngine.cpp` transform pass; keyword guard `:1135` |
| **Phase B — property re-emit** | non-transform editable properties, diffed vs parse-time snapshot, Mode-A spliced; no Mode B (un-spliceable → Refused) | **Camera, Light, Material, Medium**, + Object bindings (material/shader/shadow/interior-medium) | `SaveEngine.cpp:1119-1336`; `InspectEntity` dispatch `:620-660` |
| **Phase C — created-entity emit** | full fresh chunk rendered from introspection, placed inside the managed block, re-emitted on every save | **CAMERAS ONLY** | `SaveEngine.cpp:1338-1402`; `RenderCreatedCameraChunk` `:544` |

**`EntityCategory`** (`DirtyTracker.h:46`): `Object, Camera, Light, Material, Medium`.
(Film / Rasterizer are NOT in this enum — they are not entity-dirty-tracked for
save; Film edits go through `IJobPriv::SetFilm` live but are not round-tripped to
the file by the save engine.)

**What is refused / deferred** (each returns `Status::Refused`, original file
untouched):
- Property edit on an entity with **no editable source chunk** (FOR-generated, or
  chunk in a `> load`-ed file) — `:1164`.
- **Cross-file** chunk (chunk lives in a different file than the saved one) —
  `:1185`, `:1413`.
- FOR-generated / chunk-revisited entity property edit — `:1178`.
- Parameter **not captured at parse time** (value-conditional descriptor row) —
  `:1221`.
- Camera `orientation` edit when source uses `pitch`/`roll`/`yaw` scalar overrides
  — `:1261`.
- `$(...)` **symbolic** source value — `:1305`.
- **Non-camera chunk inside the managed block** (Phase C V1 can't re-emit it) —
  `:1373`.
- Destructive reload commands (`> clearall`, `> remove object`) that would erase a
  managed override — `:1424`.
- External modification of the file since load (span guard) — tested
  `SaveEngineTest.cpp:766`.

**Mode A vs Mode B selection** is driven by the `>` BARRIER command
classification and the transform-authoring form (Euler → A; FOR/matrix/quaternion/
anchor-scale → B).

**Tests** (`tests/SaveEngineTest.cpp`, all 41 run in `main` `:2331-2369`):
NoOp byte-identity, Mode-A position splice, drag-undo NoOp, FOR→Mode B, Mode-B
re-save byte-identity, matrix/quaternion→matrix override, scale-from-anchor→
override, Mode-A insert-new-line, multi-field value-length-delta, external-mod
refuse, Save-As to new/over-unrelated path, **camera property** round-trip + 2nd-save
NoOp (`:1418`), **medium property** round-trip (`:1523`), **light property**
round-trip + **light color sRGB** conversion (`:1601`), **created-camera**
round-trip + 2nd-save-NoOp + undo-omits-camera (`:1690`), **camera orbit/roll**
round-trip, managed-block camera **survives reload+resave** (`:2368`), dirty-tracker
cleared on save. Controller-level integration: `SceneEditControllerSaveTest.cpp`
(Saved on dirty, NoOp on clean byte-identical, Failed on bad path → `LastSaveError`).
**Gap:** no dedicated `TestMaterialPropertySaveRoundTrips` in the runner — the
material Phase-B path exists and is exercised indirectly, but lacks a named test
(camera/light/medium each have one).

---

## 2. Entity-creation surface

**The controller can CREATE exactly one thing: a clone of the active camera.**

- `SceneEdit::Op` (`SceneEdit.h:110-257`) — the full mutation vocabulary. The
  **only creation op is `AddCamera`** (`:212`); every other op mutates an
  existing entity (transform, material/shader/geometry/medium binding, light/
  camera/medium/material property, time). There is **no** `AddObject`,
  `RemoveObject`, `AddLight`, `AddMaterial`, `Import*`, etc.
- `SceneEditController::CloneActiveCamera` (`SceneEditController.cpp:3103`) is the
  sole creation entry point: cancel-and-park, snapshot the active camera
  (`CameraIntrospection::CaptureCameraSnapshot`), dedup the name, apply
  `AddCamera`. Clonable types: non-ONB Pinhole / ThinLens / Fisheye /
  Orthographic (`:3137`).
- Underlying factories `IJob::Add{Pinhole,Thinlens,Fisheye,Orthographic}Camera`
  exist; `RemoveCamera` exists (used by `AddCamera` undo). These are NOT exposed
  as a general "add camera from scratch" — only clone.

**Persistence:** a cloned camera persists through save→reload (Phase C). The
header notes the persistence caveat is now satisfied for cameras
(`SceneEditController.h:640-644` predates Phase C and still says "round-trip
pending" — stale comment; Phase C `SaveEngineTest.cpp:1690` proves persistence).

**Per-platform exposure of camera-clone:**

| Platform | Exposed? | Code |
|---|---|---|
| macOS | **Yes** — "Add Camera" button | `PropertiesPanel.swift:400` → `bridge.addCameraFromActive` → `RISEViewportBridge.mm:838` |
| Windows | **Yes** | `ViewportBridge.cpp:524` / `.h:361` `addCameraFromActive` |
| Android | **No** | no `nativeViewport*AddCamera*` in `rise_jni.cpp` |

---

## 3. Introspection surfaces

`src/Library/SceneEditor/*Introspection.{h,cpp}` are the read + editable property
substrate. Shared shape: `CameraProperty { name, kind (ValueKind), value,
description, editable (bool), presets, unitLabel }` (`CameraIntrospection.h`).

| Surface | Entity types | Editable props | Read-only props | Has SetProperty/SetSlot? | Test |
|---|---|---|---|---|---|
| **Camera** | 4: Pinhole, ThinLens, Fisheye, Orthographic | location/lookat/up/exposure/scanning_rate/pixel_rate/theta/phi/pitch/roll/yaw/orientation + per-type lens params (~28) | width/height/pixelAR (const-bound), type-unavailable rows | Yes (`SetProperty`, `CameraIntrospection.cpp:626`) | Phase4DescriptorDriven, FilmIntrospectionTest |
| **Light** | 4: Point/Omni, Spot, Directional, Ambient | power/color/position/target/inner/outer/shootphotons (type-gated; ~8-12) | Name, Type, out-of-tree types | via `SceneEdit::SetLightProperty` keyframe path (no direct SetProperty) | LightFullCoverage, Phase4DescriptorDriven |
| **Material** | 20+ concrete types | painter/scalar-painter slots (1-9/type) | Name, Type, **composed** materials (PBRMetallicRoughness, GGXEmissive) all-read-only, unregistered painters, construction-only params | Yes (`SetSlot`, `MaterialIntrospection.cpp:772`; rejects composed/unregistered/wrong-pipe) | Phase4DescriptorDriven, ThinFilmIntrospectionTest |
| **Media** | 2: Homogeneous, Heterogeneous | Homogeneous: absorption, scattering | Homogeneous: emission, phase (construction); **Heterogeneous: ALL** (baked majorant grid) | `SetSlotValue` Homogeneous-only (`MediaIntrospection.cpp:133`) | MediaFullCoverage, Phase4DescriptorDriven |
| **Object** | 1: standard_object | material, shader, interior_medium + transform fields | geometry, name, shadow flags, modifier, radiance_* | via `SceneEdit` ops | Phase4DescriptorDriven |
| **Film** | 1: film | width, height, pixelAR (all editable) | none | Yes (`FilmIntrospection.cpp:65` → `IJobPriv::SetFilm`) | FilmIntrospectionTest |
| **Rasterizer** | N (per registered rasterizer) | params present in `Job::RasterizerParams` snapshot | params absent from snapshot → "(scene-file only)", defaultshader, Type | via `IJob::SetRasterizerParameter` | Phase4DescriptorDriven |

---

## 4. Property editing — descriptor-driven panel path; the "pipe" question

**Path.** Panel reads `PropertyCount/Name/Value/Kind/Editable[For]` off the
controller's snapshot (`SceneEditController.h:569-592`), rebuilt by
`RefreshProperties`. Edits go through `SetProperty` / `SetPropertyForCategory`
(`:619/:629`), which route to the right introspection `SetProperty`/`SetSlot` or
`SceneEdit` op. Categories editable vs read-only follow the table in §3; composed
materials and Heterogeneous media are read-only by design.

**The color-vs-scalar pipe is NOT encoded in the descriptor — confirmed.**
- `ValueKind` enumerators (`ChunkDescriptor.h:110-122`): `Bool, UInt, Double,
  DoubleVec3, DoubleVec4, DoubleMat4, String, Filename, Enum, Reference`. No
  painter-type discriminator.
- `ParameterDescriptor` fields (`ChunkDescriptor.h:296-310`): name, kind,
  required, repeatable, enumValues, referenceCategories, tupleKinds, presets,
  description, defaultValueHint, unitLabel, apply. **No `pipe` field.** A painter
  binding (color OR scalar) parses as a `Reference`-kind parameter.
- **Routing happens downstream**, hardcoded by slot knowledge:
  `Job::ResolveOrDiagnoseScalar` (`Job.cpp:2718-2777`) is called by material
  factories for scalar slots (tau/ior/roughness/scattering); it consults
  `IScalarPainterManager` and, on a mistaken `IPainter` binding, emits the
  IScalarPainter-refactor diagnostic (`Job.cpp:2748`). Callers at
  `Job.cpp:2593,2798,2836,2875,2919,2953,3011` each hardcode which manager to use.
  `MaterialIntrospection::MaterialSlotRef::Kind { None, Painter, ScalarPainter }`
  (`MaterialIntrospection.h:53`) carries the pipe knowledge in the introspection
  layer, not the grammar.

(This matches MATERIAL_EDITOR §1 and GUI_ROADMAP §10.5, which both state it
correctly.)

---

## 5. ROI / region render — per-integrator support matrix

`Job::RasterizeRegion` (`Job.cpp:8806-8833`) takes inclusive l/t/r/b bounds,
builds `Rect rc(top,left,bottom,right)` (`:8828`), calls
`RasterizeScene(scene, &rc, seq)` (`:8829`). The interface signature is
`IRasterizer::RasterizeScene(IScene&, const Rect* pRect, IRasterizeSequence*)`
(`IRasterizer.h:70`, "if NULL, rasterizes the entire scene"). Pixel-based
rasterizers clip the loop via `PixelBasedRasterizerHelper::BoundsFromRect`
(`PixelBasedRasterizerHelper.h:308-329`), called from `RasterizeScenePass`
(`PixelBasedRasterizerHelper.cpp:844`).

| Integrator | Derives from PixelBasedRasterizerHelper? | Honors `pRect`? | Citation |
|---|---|---|---|
| `pixelpel_rasterizer` | Yes | **Yes** | inherits BoundsFromRect clip (`PixelBasedRasterizerHelper.cpp:844`) |
| `pathtracing_pel_rasterizer` | Yes | **Yes** | via PixelBasedPelRasterizer chain |
| `pathtracing_spectral_rasterizer` | Yes | **Yes** | via PixelBasedSpectralIntegratingRasterizer chain |
| `bdpt_pel_rasterizer` | Yes | **Yes** | BDPTRasterizerBase → RasterizeBlocksForPass with pRect |
| `bdpt_spectral_rasterizer` | Yes | **Yes** | same path as Pel |
| `vcm_pel_rasterizer` | Yes | **Yes** | VCMRasterizerBase → PixelBasedRasterizerHelper |
| `vcm_spectral_rasterizer` | Yes | **Yes** | same chain |
| `mlt_pel_rasterizer` | **No** (→ `Rasterizer`) | **NO** | `MLTRasterizer.cpp:1226` `const Rect* /*pRect*/` (commented out) |
| `mlt_spectral_rasterizer` | **No** (→ MLTRasterizer) | **NO** | inherits the pRect-ignoring override |

**Why MLT ignores it:** Metropolis-Hastings mutations explore the whole image
plane; clipping to an arbitrary rect would break the chain's ergodicity. A region
render request under MLT silently renders the full frame.

**Filter / OIDN border behavior:** the reconstruction filter
(`FilteredFilm::Resolve`, `FilteredFilm.cpp:106`) and OIDN denoise
(`PixelBasedRasterizerHelper.cpp:1301`, full `width×height`) always operate on the
**full frame**, not the sub-rect — splat tails from samples inside the rect can
land just outside it, and denoise AOVs trace from every pixel. Interior pixels
are statistically identical to a full render; the rect edges differ (zero input
from outside).

**No GUI wires region render.** The controller's `DoOneRenderPass`
(`SceneEditController.cpp:3793`) and production path (`:1692`) pass `pRect = 0`.
There is no `SetRenderRegion` / region UI on any platform.

---

## 6. Picking

**Shared** in `SceneEditController::PickAt` (`SceneEditController.cpp:2322`):
`GenerateRay` through the pixel (with a Y-flip for the rasterizer's screen-space
inversion, `:2360`), `IObjectManager::IntersectRay` (`:2371`), recover the hit
object's registered name via manager walk (`:2377`), then `SetSelection(Object,
name)` — which auto-expands the Objects accordion and auto-syncs the Materials
section. No hit → `SetSelection(None)`.

**Platform-specific = raw pointer events only.** Bridges forward
window/HiDPI-normalized coords to `OnPointerDown` (`SceneEditController.h:267`),
which calls `PickAt` when the Select tool is active (`SceneEditController.cpp:867`).

- macOS: **wired** — `ViewportView.swift:83-91` `onPointerDown` →
  `bridge.pointerDown` → `RISEViewportBridge.mm:561`; default tool is `.select`
  (`ViewportView.swift:64`), and the comment at `:86` ("Pointer-down on Select
  tool may have just picked an object") confirms the pick path is live. (The
  sub-audit's "Phase 5 not-done" note was stale/misread — picking is wired.)
- Windows: pointer events at `ViewportBridge.cpp:341-343`.
- Android: pointer events at `rise_jni.cpp` / `RiseNative.kt:315-317`.

---

## 7. Environment / HDRI

| API | Semantics | Citation |
|---|---|---|
| `IScenePriv::SetGlobalRadianceMap(const IRadianceMap*)` | **SWAP** — replace the environment map | `IScenePriv.h:131` |
| `IScenePriv::GetGlobalRadianceMapMutable()` | mutable access to **SCALE** the existing map (backs `Job::SetActiveRasterizerRadianceScale` → `IRadianceMap::SetScale`) | `IScenePriv.h:259` (doc `:249-258`) |

**HDRI swap is wired into NO GUI.** Grep of macOS/Windows/Android GUI trees for
`SetGlobalRadianceMap` / `RadianceMap` / `HDRI` / `environment_map` →
zero matches. There is no load-HDRI / swap-environment UI on any platform.
**State: L-but-not-G.**

---

## 8. Camera management

- `ICameraManager` backs a **multi-camera** scene; active camera via
  `IScene::GetActiveCameraName` / `SetActiveCamera`.
- Controller exposes cameras as accordion `Category::Camera`
  (`SceneEditController.h:210`); `SetSelection(Camera, name)` calls
  `SetActiveCamera` through cancel-and-park (`:318-326`). `CategoryEntityCount/Name`
  + `CategoryActiveName` drive the dropdown.
- Add: clone-only (`CloneActiveCamera`, §2). Active-switch + clone exposed on
  macOS + Windows; Android exposes the camera list + active-switch but not clone.
- All three GUIs render the Camera category and switch active camera.

---

## 9. Named views / axis views / split view

**ABSENT on all platforms (A).** No canonical front/top/side axis views, no
quad/split viewport, no saved named views in the controller or any GUI. Grep of
`src/` and GUI trees for axis/split/quad-view machinery → nothing. (GUI_ROADMAP §2
correctly lists these as "not built on either desktop platform yet"; CAMERAS_AND_VIEWS
designs them as future B-direction work.)

---

## 10. Present / display path per platform

**RISE is a CPU path tracer — there is no GPU render path.** Each platform's
GPU/graphics layer only **displays** a CPU-computed framebuffer.

| Platform | Present surface | SDR/HDR | Citation |
|---|---|---|---|
| macOS | `CAMetalLayer` (raw Metal blit of the CPU frame) | **EDR** — `wantsExtendedDynamicRangeContent = true`, `layer.colorspace` set; user toggle "EDR Preview" | `MetalEDRView.swift:323, 176-178`; `RISEApp.swift:93` |
| Windows | DXGI swap chain | **HDR** — `DXGI_FORMAT_R16G16B16A16_FLOAT` scRGB, `IDXGISwapChain3`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` | `HDRRenderWidget.cpp:323, 348, 356` |
| Android | CPU `ByteBuffer` (RGBA8) → Compose `ImageBitmap.makeFromRGBA8` | **SDR only** — exposure/tone applied at readback | `RiseNative.kt:126`; `ViewportPane.kt:400+` |

---

## 11. Auto-router probe — heatmap substrate

`AutoRasterizer` (`AutoRasterizer.h:70`) is the shipped auto-dispatcher.
**Externally exposed (the cross-UI query surface):**
- `ResolvedIntegratorName()` — "pt"/"bdpt"/"vcm"/"auto" (`:158`)
- `ResolveReason()` — one-line reason (`:159`)
- `LastProbeSeconds()` / `LastProbeRenders()` — probe cost (`:166-168`)
- `IsAutoDispatcher()` → true (`:157`)

The GUIs surface "Auto → PT/BDPT/VCM" after a render using these scalars.

**Internal only — NOT a usable heatmap surface:** the per-candidate `ProbeResult`
(`AutoRasterizer.h:192-200`: `medianLum, meanLum, robustMeanLum, meanVar,
rasSeconds`) and the `ProbeFrame`'s per-pixel `lum` vector
(`AutoRasterizer.h:128`, "per-pixel luminance, row-major") are **private** structs
consumed to scalars inside `RunProbe`/`ProbeCandidate` and discarded. There is no
per-pixel σ²·T / median-lum / reach readback accessor today. A heatmap feature is
net-new additive read-back (SPECTRAL_DIFFERENTIATORS D5 states this correctly).

---

## 12. Validation / parse

**No side-effect-free parse/validate path exists.**
- Scene parsing constructs Job/Scene entities immediately as each chunk's
  `Finalize` runs `pJob.Add*/Set*` (`AsciiSceneParser.cpp:9867`, generic
  descriptor dispatch ~`:725`; main loop `:10908`). `LoadAsciiScene`
  (`Job.cpp:9221`) unconditionally triggers full construction. There is no
  parse-only / validate-only API, and no string-input parser.
- Command parser (`AsciiCommandParser.cpp`): the command table (`:42-48`) maps
  `render`→ParseRasterize, `renderanimation`, `run`→ParseRun, `quit`→ParseQuit.
  **`> quit` calls `exit(1)`** (`:182`) — it terminates the process. `render`
  kicks a full render; `load` loads a scene file; all carry hard side effects.
- **`Job` does NOT retain scene source text.** It holds
  `std::unique_ptr<SourceSpanIndex>` (`Job.h:116`) — byte-range metadata only
  (`SourceSpanIndex.h:50-73`, offsets/begin/end, no text buffers). SaveEngine
  round-trips by **re-reading the file from disk** at save time
  (`SaveEngine.cpp:869`), not from memory. There is no `GetSceneText()`.

(Consequence for any future MCP/validate tool: scene-text retention on `Job` and a
side-effect-free parse are both net-new — see GUI_ROADMAP §10.5 / MCP §3.1, §4.6,
which state this correctly.)

---

## 13. MCP / LLM

**Entirely absent (greenfield).** Grep of `src/` for MCP server, agent runtime,
LLM/provider, `tool_call`, `jsonrpc` → nothing relevant:
- No `src/Library/MCP` directory; no `MCP`/`LLM`/`agent` under `src/Library/`.
- `src/DRISE/MCPClientConnection.{h,cpp}` is **"Master Control Program"**
  distributed-render plumbing — unrelated to Model Context Protocol.
- The scene is ASCII `.RISEscene` text; there is **no JSON scene serialization**
  and no structured tool surface an LLM could call. The Job API is C++ only.

All of LLM_AGENT_RUNTIME, MCP_TOOL_SURFACE, and the AI rows of
CROSS_PLATFORM_ARCHITECTURE are net-new design.

---

## Cross-platform GUI wiring matrix

Controller capability → which GUI actually calls it.

| Capability | Library | macOS | Windows | Android |
|---|---|---|---|---|
| Tool state machine / pointer events | ✓ | ✓ `RISEViewportBridge.mm:488,561` | ✓ `ViewportBridge.cpp:260,341` | ✓ `RiseNative.kt:240,315` |
| Picking (Select tool → `PickAt`) | ✓ `:2322` | ✓ | ✓ | ✓ |
| Selection / accordion categories (all 8) | ✓ | ✓ `PropertiesPanel.swift:79-88` | ✓ `ViewportBridge.h:279-289` | ✓ `ViewportPane.kt:217` |
| Property edit (`SetProperty[ForCategory]`) | ✓ | ✓ `RISEViewportBridge.mm:921` | ✓ `ViewportBridge.cpp:604` | ✓ `RiseNative.kt:388` |
| Gizmo handles | ✓ | ✓ | ✓ | ✓ |
| Timeline scrub | ✓ | ✓ | ✓ | ✓ |
| **Clone active camera** | ✓ `:3103` | ✓ `:838` | ✓ `:524` | ✗ |
| **Round-trip Save scene** | ✓ `SaveEngine` | ✓ `RISEViewportBridge.mm:657` | ✓ `ViewportBridge.cpp:402` | ✗ (only image Save-As `rise_jni.cpp:166`) |
| HasUnsavedChanges / dirty listener | ✓ | ✓ | ✓ | ✗ |
| Production render | ✓ | ✓ | ✓ | ✓ `RiseNative.kt:358` |
| HDRI / environment swap | ✓ (lib) | ✗ | ✗ | ✗ |
| Region / ROI render | ✓ (lib, non-MLT) | ✗ | ✗ | ✗ |
| Named / axis / split views | ✗ | ✗ | ✗ | ✗ |
| HDR/EDR display | — | ✓ EDR | ✓ scRGB | ✗ SDR |

**Capabilities in the library/controller that NO GUI calls:** HDRI environment
swap, region/ROI render, the auto-router per-pixel probe buffers. **Android-only
gaps:** scene save, camera clone, unsaved-change tracking.

---

## Stale assumptions in the seven `docs/gui/` specs (correction punch-list)

> **Status: APPLIED (2026-06-20).** These corrections were made in the correction round — the seven specs were revised and five foundation specs added. This punch-list is retained as a **historical record of what was fixed**, not an open to-do; current ground truth is the Executive summary + body above.

Verified contradictions between spec text and code (as found 2026-06-19); each has since been reconciled to this audit.

### Theme 1 — "round-trip save is unbuilt / Phase-0-future" (DOMINANT ERROR)
Save IS implemented incl. created-camera persistence (§1). Fix in:

- **CAMERAS_AND_VIEWS.md** (worst):
  - "round-trip save … Phase 6, **designed-not-built**" (preamble) → built.
  - §3.4 "durable promotion needs the round-trip save's camera re-emit … **Until
    that lands**, 'Promote' still works in-memory … 'won't survive reload'
    warning" → camera re-emit (Phase B) + created-camera (Phase C) shipped and
    tested; drop the "won't survive reload" camera warning.
  - §8 phasing soft-blocks **B1b/B3/B4a** on "round-trip save (camera re-emit)" →
    already satisfied for camera/property cases (B4b `animation`-chunk extension
    is a legit remaining soft-block).
  - §11 #6 "B1b/B4b … depend on round-trip save's camera re-emit … land them
    first so Direction B delivers value **before save lands**" → camera half moot.
- **MATERIAL_EDITOR.md**: §3.7 / §9 **C2.B** "C2 Phase B depends on **Phase 0
  round-trip save** … until that lands, rewrite the material block wholesale" →
  Phase-B material property re-emit shipped; only graph-*topology* serialization
  is genuinely new.
- **LLM_AGENT_RUNTIME.md**: §1 "MVP requires no … round-trip save"; §7.1 "**before
  round-trip save lands (Phase 0)** … wholesale text rewrite"; §10 **A2** "Depends
  on: Phase 0 (round-trip save)" → structured save (property/transform/created-
  camera) exists; restate the dependency to "non-camera creation persistence only".
- **MCP_TOOL_SURFACE.md**: the entire "**needs RT-save**" persistence legend
  (§4) is stale for property/transform/created-camera classes. Specifically:
  - §4.1 `set_property` "needs RT-save (transforms persist today … other fields
    via wholesale)" → non-transform **property** re-emit persists today too.
  - §4.2 `add_entity` "needs RT-save (creation is Phase-0)" → created **cameras**
    persist (Phase C); only non-camera creation is the real gap.
  - §4.2 `clone_entity` "today for camera … persists via §9.5 caveat" → it's a
    real implemented Phase C path, not a wholesale caveat.
  - §4.5 `save_scene` "wholesale today" → structured saves ship today.
  - §9 **M2** "once Phase 0 lands" → largely landed.

### Theme 2 — picking mislabeled platform-specific
- **APPROACHABILITY_FOUNDATION.md**: §4 table lists drag hit-test under
  "Platform-specific (thin shell)"; §7 A3 calls hit-testing "**irreducibly
  per-platform**" and locates the pick "in the Mac/Win bridges." → `PickAt` is
  shared C++ (§6); only raw pointer capture is per-platform.
  (CROSS_PLATFORM_ARCHITECTURE §1.2 gets this right.)

### Theme 3 — variance/heatmap surface overstated
- **MCP_TOOL_SURFACE.md**: §3 `rise://render/variance` reads as backed by an
  existing variance/RMSE map → the per-pixel `ProbeResult` is **private**, no
  per-region variance surface today (§11). (`rise://render/autoroute` is fine — it
  only uses the public name/reason/seconds/renders.) Model the correction on
  SPECTRAL_DIFFERENTIATORS D5, which already calls this net-new additive readback.

### Theme 4 — HDRI swap readiness overstated
- **APPROACHABILITY_FOUNDATION.md**: §7 A3 cites `GetGlobalRadianceMapMutable()`
  as if the swap is near-wired → the mutable accessor only **scales**; **swap** is
  `SetGlobalRadianceMap`, and neither is wired into any GUI (§7).

### Theme 5 — omission to ADD (not a contradiction)
- No spec carries the **MLT-ignores-`pRect`** caveat. Add to
  APPROACHABILITY_FOUNDATION §8 (A4, region render) and MCP_TOOL_SURFACE §4.4
  (`set_region`): region render silently no-ops under MLT (§5).

### Already-correct (no change needed)
- **GUI_ROADMAP.md §2** — self-corrected 2026-06-19: round-trip save "largely
  IMPLEMENTED", "genuine gap is creation, not persistence",
  "`CloneActiveCamera` is the only creation path". This is the reference the other
  six should match. (Minor internal lag: §11 **A2** / §13 #6 still imply creation
  persistence is wholly Phase-0 — tighten to "non-camera creation only".)
- **CROSS_PLATFORM_ARCHITECTURE.md** — correctly treats `SaveEngine` as shipped
  (§1.1, §8) and MCP/LLM as greenfield (§2); §1.2 gets picking right. (Reconcile
  the SaveEngine size note: "85 KB" here vs "1,898 lines" in roadmap — both real.)
- **SPECTRAL_DIFFERENTIATORS.md** — D5 correctly states the probe exists but
  per-pixel buffers are "consumed to scalars and discarded"; §10 correctly bans a
  `GetColorNM` on `IScalarPainter`. Caveat: D3 thin-film facts are on the
  unpushed `feature/thin-film-interference` branch, not master — add the branch
  caveat where it says "already in the tree".
- **MATERIAL_EDITOR.md §1** + **GUI_ROADMAP §10.5** — correctly state `ValueKind`
  does not encode the pipe and routing is in `Job::ResolveOrDiagnoseScalar` (§4).
- All E-direction docs correctly treat MCP server + agent runtime as absent (§13).
