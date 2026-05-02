# RISE Interactive Scene Editor Plan

**Status**: Phases 1-5 shipped (Library + macOS + Windows + Android viewports, production-render integration, descriptor-driven properties panel, multi-level adaptive scaling with idle refinement). Phase 6 (round-trip save) not yet started.
**Owner**: Aravind Krishnaswamy
**Scope**: Add a true interactive 3D viewport on top of the existing Mac, Windows, and Android RISE apps. Toolbar-driven object/camera/timeline mutation, live preview rendering as the user drags, undo/redo, and round-trip save back to `.RISEscene` (transform-only in this initiative; full re-serialize deferred).

**Progress at a glance** (see §19 for the full log):

| Phase | Status | Key delivery |
|---|---|---|
| 1 | shipped | `SceneEditor` + `EditHistory` + `InteractivePelRasterizer` + `SceneEditorBasicsTest` |
| 2 | shipped | `SceneEditController` + `CancellableProgressCallback` + cancel-restart `SceneEditorCancelRestartTest` |
| 3 | shipped | macOS viewport: `RISEViewportBridge`, SwiftUI `ViewportView`, toolbar with cursor binding, picking, drag-to-move/orbit/scrub, properties panel |
| 4 | shipped | Windows (`ViewportBridge`/`ViewportWidget`/Qt) + Android (`RiseBridge` viewport methods + JNI + Compose `ViewportPane`) parity |
| 5 | shipped | "Render" button stops viewport, runs scene-declared rasterizer on mutated state, restarts viewport. Always-on viewport (no toggle). |
| 6 | not started | Round-trip save (in-place transform rewrite) |

**Cross-cutting requirements added during implementation** (these were not in the original plan):

- **Always-on viewport** instead of an "Interact" toggle. The viewport is the default editing surface as soon as a scene loads. Production render disables interaction; clicking Render stops the viewport, runs the rasterizer, and restarts it. (See §19.5.)
- **No-throttle preview dispatch.** Every frame the rasterizer produces reaches the screen, including partial buffers from cancelled passes — during fast manipulation the cancel flag trips on every pointer move, and dropping cancelled frames makes the viewport feel throttled (only post-pause refinement frames would land). Center-out tile order keeps partial buffers visually usable. *(Reversal of an earlier "cancel-aware sink drop" requirement that proved to be too aggressive — see §19.6.)*
- **Persistent IRasterImage on every rasterizer.** `PixelBasedRasterizerHelper` holds the rasterizer's framebuffer across `RasterizeScene` calls instead of allocating fresh per call. Two consumers benefit: (1) the interactive viewport's cancel-restart loop reuses the previous frame's pixels so cancelled passes degrade gracefully, and (2) production rasterizers can serve the final image to a future "save image to any format" UI feature via `GetLastRenderedImage()` without re-rendering. (§19.8)
- **Skip random-pastel clear for interactive renders.** The default `PrepareImageForNewRender` clears to a random pastel (a debug visual that highlights uncovered tiles); during interactive cancel-restart this produces visible flashes of colour. `InteractivePelRasterizer` overrides the hook to no-op so previous content stays under the in-progress tiles. (§19.8)
- **Center-out tile order for interactive renders.** The interactive rasterizer's `Config.tileOrder = TileOrder_CenterOut` was originally documented but not wired up — the base helper unconditionally created a Morton (Z-curve) sequence that filled tiles starting from the upper-left. Added a virtual `CreateDefaultRasterSequence(tileEdge)` factory; the interactive override returns a `BlockRasterizeSequence` honouring `Config.tileOrder` (defaults to centre-out). Partial buffers from cancelled passes now show useful image content from the middle out. (§19.9)
- **Post-release 4-SPP polish pass with elevated max-recursion.** After `OnPointerUp`, the controller runs the regular 1-SPP scale=1 final pass, then chains a single 4-SPP scale=1 polish pass that swaps in a max-recursion-2 ray caster (one bounce of glossy / reflected / refracted rays) for higher-quality reflections, refractions, and glossy highlights in the at-rest image. Any user edit during the polish cancels the chain via `KickRender`. (§19.10)
- **Toolbar pared down to camera-only manipulation.** Object Translate / Rotate / Scale tools and the standalone Scrub-Timeline tool are dropped from the toolbar — object editing carries too much complexity for the current state of the app, and timeline scrubbing is driven directly by the bottom timeline bar. Only Select, Orbit-Camera, Pan-Camera, Zoom-Camera surface in the UI. The C++ `SceneEditController::Tool` enum keeps all values so the controller is forward-compatible if the object tools come back later. (§19.14)
- **Tool cursor restricted to the rendered-image area.** The platform viewports compute the aspect-fit draw rect for the current image; the tool cursor (orbit hand, etc.) only takes effect inside that rect, and the system arrow returns whenever the pointer crosses out into the dark surround or leaves the widget. macOS uses `addCursorRect(drawRect, …)`; Windows uses `setCursor` / `unsetCursor` on `mouseMoveEvent` + `leaveEvent`; Android (touch UI) is a no-op. (§19.14)
- **Right-side panel is a four-section accordion.** Sections (top-to-bottom): Cameras, Rasterizer, Objects, Lights.  Single selection across the whole panel — picking a row in any section auto-collapses the others, expands this one, highlights the row, and surfaces the entity's property rows directly under it.  Object click-on-image picking auto-expands the Objects section and highlights the picked row.  Camera-tool gestures (Orbit / Pan / Zoom / Roll) auto-expand the Cameras section.  Camera and Rasterizer selections also activate the named entity (call `SetActiveCamera` / `SetActiveRasterizer`); Object and Light selections are pure UI state.  Selection state is the `(Category, entityName)` tuple stored in `SceneEditController`; `CurrentPanelMode()` returns `{None=0, Camera=1, Rasterizer=2, Object=3, Light=4}` (= `Category` cast to int).  Picking lives in `OnPointerDown` for the Select tool (calls `IObjectManager::IntersectRay`, recovers the name via `EnumerateItemNames`, routes through `SetSelection(Object, name)`).  (§19.15)
- **Pointer events mapped to image-pixel space.** Each platform's bridge converts viewport-relative click coordinates into the rasterizer's image-pixel coordinate system before passing them to the controller. Two adjustments per platform: aspect-fit offset (subtract draw-rect origin) and pixel-density rescale (divide by draw-rect points, multiply by image pixels). HiDPI is handled implicitly. Picking and orbit / pan / zoom now operate in the same coord space the camera's `GenerateRay` expects. (§19.16)
- **Camera-projection Y-flip in picker.** `PickAt` flips the click-y to `cam->GetHeight() - py` before calling `GenerateRay`. Rasterizer image space and the camera's projection-matrix screen space differ by a Y inversion (a property of the camera projection matrix); without the flip, clicking visually low picks objects rendered visually high. (§19.16)
- **Tool selection re-applied to controller after viewport restart.** When a new scene loads (or the controller is otherwise rebuilt) the freshly-constructed `SceneEditController` defaults to `Tool::Select`, but the platform UI's persisted toolbar selection survives. Each platform now re-pushes the toolbar's selection to the controller on viewport restart so they stay in sync (macOS via `.task(id: ObjectIdentifier(bridge))`, Windows in `MainWindow::onStateChanged`'s `SceneLoaded` branch, Android via a `viewportEpoch` `StateFlow` observed by `ViewportPane`). (§19.17)
- **Camera-control rates scaled by scene size.** `SceneEditor` now caches `SceneScale()` — the diagonal of the union of every object's bounding box — and uses it to scale pan / zoom rates instead of the camera-to-look-at distance. Small scenes get small absolute changes per pixel; large scenes get large ones, independent of where the camera is positioned. Orbit stays angular (rotation rates aren't a function of scene extent). (§19.17)
- **Camera angles are first-class, keyframable, and round-trippable.** Orbit and roll now mutate the camera's already-existing `target_orientation` (theta, phi) and `orientation.z` (roll) parameters rather than baking the result into `vPosition`. Both are declared in the parser descriptor (so they appear in the properties panel and round-trip through `.RISEscene`) and are keyframable through `CameraCommon`'s existing `TARGET_ORIENTATION_ID` / `ORIENTATION_ID` animator IDs (so animations interpolate angles, not derived positions). The toolbar gains a Roll tool (drag horizontally → roll around the camera→look-at axis); orbit is unchanged in feel but parameter-stored under the hood. Pan and Zoom continue to mutate `vPosition` / `vLookAt` since they're translations, not rotations. (§19.18)
- **Properties panel "location" shows REST position; pan/zoom math operates in rest space.** `CameraCommon::GetRestLocation()` returns `vPosition` (the value the user sets in `.RISEscene`, what pan/zoom mutate), distinct from `GetLocation()` which returns the post-orbit `frame.GetOrigin()` the rasterizer renders from. The properties-panel "location" row switched to the rest accessor so orbit no longer appears to "change the camera location" even though it moves the rendered eye. Pan now translates BOTH `vPosition` and `vLookAt` (commutes with orbit); Zoom translates `vPosition` along the rest-space forward `(lookAt - vPosition)` so after Recompute applies orbit, the post-orbit eye moves toward look-at as expected. Pre-fix both ops applied screen-space deltas to the post-orbit eye and wrote the result into vPosition — Recompute then re-applied orbit, double-rotating and breaking Undo. (§19.19)
- **Pointer events use stable full-resolution camera dims, not the rendered frame size.** `SceneEditController::GetCameraDimensions()` (and matching C-API `RISE_API_SceneEditController_GetCameraDimensions`) returns the canonical full-res dims captured BEFORE the preview-scale dim swap, refreshed at the start of every render pass. Each platform's bridge exposes a `cameraSurfaceDimensions` accessor; the viewport widget's `surfacePoint` uses these stable dims as the conversion target instead of the rendered image's dims. Pre-fix, `mLastPx` (captured at one scale level) and the next pointer event (in another scale level) lived in mismatched coord spaces, so the controller's `(px - mLastPx)` delta jumped 4×–32× whenever the preview-scale state machine stepped — manifesting as huge pan/orbit jumps during fast moves that "settled down" once the renders caught up to scale=1. (§19.19)
- **Pan X-axis flipped to "grab the world" feel.** Pre-fix, dragging right made the scene appear to slide LEFT (camera moved right). Y was already grab-the-world (drag-down made the scene drag down with the pointer). The X axis now matches: drag right → scene slides right → camera moves left. One-line fix to negate the dx sign in PanCamera. (§19.20)
- **Roll tool C-API bound check raised from ScrubTimeline (=7) to RollCamera (=8).** `RISE_API_SceneEditController_SetTool` validated the incoming tool against `tool > SceneEditTool_ScrubTimeline` — silently rejecting RollCamera (=8) at the API boundary. The bridge would call SetTool(8), the call would return false, and the controller's mTool stayed at whatever was selected before. Visually, clicking the Roll button looked like the previous tool (typically Zoom) was still active — drags moved the camera position instead of rolling. (§19.20)
- **Orbit theta clamped to ±89° to prevent gimbal lock and NaN propagation.** `AdjustCameraForThetaPhi` only clamps theta at +π/2 max and leaves the −π/2 side unbounded. Past the pole, the post-orbit forward becomes parallel to vUp and `ONB::CreateFromWV`'s `cross(vUp, forward)` collapses to the zero vector — the camera basis becomes degenerate. Subsequent pan/zoom edits computed `right`/`forward` from this degenerate basis and propagated NaN through vPosition, manifesting as the panel's "location" field reading "nan nan nan" after the user kept orbiting in one direction. SceneEditor now clamps target_orientation.x to ±1.553 rad (~±89°) inside the Orbit op so the basis stays well-conditioned. Pan/Zoom additionally have a `HasNaN` guard before writing back to vPosition as a defensive belt-and-braces. (§19.20)
- **Suppress-next-frame after production.** Each platform bridge exposes `suppressNextFrame()`; the host calls it before restarting the viewport after a production render so the production image stays visible until the user actually starts dragging. (§19.6)
- **Multi-level adaptive scaling with idle refinement.** Six preview-scale levels (1, 2, 4, 8, 16, 32), three feedback loops: during-motion adaptation (×2 step, ×4 jump on >100ms), resume-after-pause snap on `OnPointerMove`, idle refinement that walks the scale toward 1 over ~half a second after the user stops moving. (§19.7)
- **Descriptor-driven properties panel.** Camera property table built by reusing the existing `IAsciiChunkParser` descriptor metadata; reflected via `RISE_API_SceneEditController_Property*` C-API surface. (§19.4)
- **State-driven UI enablement.** Viewport controls disabled (greyed) during `Loading` / `Rendering` / `Cancelling` so edits cannot race the production rasterizer. Mirrored across all three platforms. (§19.5)

---

## 1. Executive Summary

**The problem.** The three RISE GUI apps ([build/XCode/rise/RISE-GUI](../build/XCode/rise/RISE-GUI), [build/VS2022/RISE-GUI](../build/VS2022/RISE-GUI), [android/app/src/main](../android/app/src/main)) are scene-text editors plus passive render viewers today. They load `.RISEscene`, hand the resulting `IJob` to a rasterizer, and display the framebuffer. None of them lets the user move objects, orbit the camera, or scrub a timeline. Each app independently subclasses `IProgressCallback` / `IJobRasterizerOutput` / `ILogPrinter` (~700 LOC of near-duplicate adapter glue across the three platforms).

**The plan.** Six phases ending with round-trip save to disk. Each phase is independently buildable on all four build systems and each ends with an adversarial code review.

| Phase | Deliverable | What ships at the end |
|---|---|---|
| 1 | `SceneEditor` + `EditHistory` + `InteractivePelRasterizer` (Library only) | Library compiles; new test exercises the full mutation invariant chain via API, no UI yet |
| 2 | `SceneEditController` + cancel-restart render thread (Library only) | Library compiles; scripted test fires high-frequency edits, asserts cancel-restart correctness, no UI yet |
| 3 | macOS interactive viewport (vertical slice) | Full-fidelity 3D editor on macOS — toolbar, drag-to-translate, orbit camera, timeline scrubber, picking. Save still stub |
| 4 | Windows (Qt) + Android (Compose+JNI) adapters | All three platforms have identical interactive editors. Save still stub |
| 5 | Production-render integration | "Render" button uses scene's declared rasterizer (PT/BDPT/VCM/MLT/etc.) on the in-memory mutated scene, with correct photon-map regeneration |
| 6 | Round-trip save (Phase A: in-place transform rewrite) | File > Save rewrites `position` / `orientation` / `scale` / `stretch` lines on dirty objects; everything else passes through verbatim |

**Goals**:
1. Single shared C++ class (`SceneEditController`) owns toolbar state, edit history, render orchestration, and cancel-restart logic. Three platforms consume it identically. Behavioral drift across platforms ruled out by construction.
2. Scene immutability invariant ([docs/ARCHITECTURE.md](ARCHITECTURE.md)) preserved. All mutations happen between renders, never during. The existing `RasterizeBlockDispatcher::cancelled` flag and `IProgressCallback::Progress() == false` contract are the only synchronization primitives.
3. Live preview at interactive framerates on a dragged object — target 30Hz on `pixelpel` (no integrator, no GI) at half-resolution.
4. Production rasterizer (whichever the scene declared — `pixelpel` / `pathtracing` / `bdpt` / `vcm` / `mlt`) renders the in-memory mutated state correctly when "Render" is invoked. Photon maps are regenerated exactly once before a production render, never during a scrub.
5. Round-trip save: dirty object transforms written back to the original `.RISEscene` text in place, preserving comments and surrounding whitespace.

**Non-goals (this initiative)**:
- Material / shader / painter editing in the viewport. The text editor remains the only path for those for now.
- Vertex-level mesh editing.
- Phase B/C round-trip serialization (descriptor-driven re-emission for cameras/lights, full topo-sorted re-serialize). Both deferred to follow-on initiatives — see §15.
- Multi-select or group operations (single-object selection only in V1).
- Animation keyframe authoring (read-only timeline scrubbing — adding keyframes is text-editor-only).

---

## 2. Pinned Design Decisions

These are locked. Changing any of them ripples across multiple phases.

| Question | Decision |
|---|---|
| Scene mutation API | `SceneEditor` friend class + `SceneEdit` value records. Closed-set tagged values; `EditHistory` owns forward+inverse for undo and dirtiness for write-back. |
| Render-vs-mutation sync | Cancel-restart via existing `RasterizeBlockDispatcher::cancelled` atomic. No new mutexes around scene or rasterizer. |
| Live preview rasterizer | New `InteractivePelRasterizer` subclass of `PixelBasedPelRasterizer`. Lives alongside (never replaces) the production rasterizer. |
| Cross-platform shared code | Library-side `SceneEditController` + thin per-platform adapter sinks. Toolbar state machine in C++. |
| macOS bridge name | `RISEViewportBridge` (existing `RISESceneEditorBridge` is the **text-editor** suggestion bridge — distinct concern). |
| Round-trip scope | Phase A only (in-place transform rewrite). B and C deferred. |
| Timeline scrubbing without photon regen | New `IScene::SetSceneTimeForPreview()` skips photon-map regeneration. Full `SetSceneTime` runs once on production-render dispatch. |
| Viewport activation | **Always-on once a scene is loaded** (no "Interact" toggle). Production render disables viewport interaction and tears off frame dispatch; on completion, viewport restarts with `suppressNextFrame()` so the production image is the visible frame until the user drags. (Decision made post-Phase-3, replaces an earlier "Interact mode toggle" design.) |
| Preview-frame dispatch contract | Sink dispatches **every** `OutputImage` call to the UI, including partial buffers from cancelled passes. The only exception is `SuppressNextFrame()`, a one-shot used right after production-render to keep the production image up until the user starts dragging. (Earlier design dropped cancelled passes; that proved to throttle the viewport during motion — see §19.6.) |
| IRasterImage lifetime | `PixelBasedRasterizerHelper` holds a persistent buffer across `RasterizeScene` calls. Acquire/Release are virtual hooks but the default impl is now persistent (was: allocate-fresh per call). Reallocates only on dimension change. Released in the rasterizer destructor. Public `GetLastRenderedImage()` accessor exposes the buffer for "save image" UI flows. |
| Interactive-render polish | `InteractivePelRasterizer` exposes `SetSampleCount(unsigned int)` (1 = single-ray, n>1 = MultiJittered kernel + non-progressive single-pass) and `SetPolishRayCaster(IRayCaster*)` (optional secondary caster swapped in for `SetSampleCount(>1)`). The macOS bridge constructs both casters at scene-load (preview maxR=1, polish maxR=2); the controller swaps via `SetSampleCount` for one final 4-SPP polish pass after pointer-up. |
| Toolbar surface | Only Select + the three camera tools (Orbit / Pan / Zoom) are surfaced in the toolbar UI. Object Translate / Rotate / Scale and the standalone Scrub-Timeline tool exist in the C++ `Tool` enum but are not exposed — too much UX complexity for the current app state, and timeline scrubbing is the bottom-bar's job anyway. |
| Cursor scoping | Tool cursor (orbit hand, etc.) applies only inside the aspect-fit rectangle of the rendered image. Outside that rect (the dark letterbox / pillarbox surround), the system arrow returns. macOS via `addCursorRect`, Windows via per-`mouseMoveEvent` swap + `leaveEvent` reset. |
| Right-panel content | Driven by the accordion's selection tuple `(Category, entityName)`. `CurrentPanelMode()` ∈ {`None=0`, `Camera=1`, `Rasterizer=2`, `Object=3`, `Light=4`} — the category whose section is currently expanded. Sections collapse when nothing in their category is selected. The property rows live under whichever section is expanded. Click-on-image picking sets `SetSelection(Object, hitName)` so the Objects section auto-expands. |
| Adaptive preview resolution | Six levels (1, 2, 4, 8, 16, 32). Three feedback loops: (1) during-motion adaptation ×2 step / ×4 jump above `kSlowMs` / /2 step below `kFastMs`; (2) `OnPointerMove` resume-after-pause snap; (3) `wait_for(kRefineWakeMs)` idle refinement that walks toward scale=1 over ~500ms after the user stops moving. Refinement passes skip the during-motion adaptation to prevent yo-yo. |
| Properties panel | Descriptor-driven, reusing `ChunkParserRegistry`'s `IAsciiChunkParser::Describe()` metadata. Edits route through `SetCameraProperty` op so they participate in undo/redo. Surfaced over C-API as `RISE_API_SceneEditController_PropertyN` family. |

---

## 3. Architecture Overview

```
┌────────────────────────────────────────────────────────────────────┐
│                          Platform UI                                │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐            │
│  │ macOS SwiftUI│   │ Windows Qt   │   │ Android      │            │
│  │ Viewport     │   │ Viewport     │   │ Compose      │            │
│  │ + Toolbar    │   │ + Toolbar    │   │ Viewport     │            │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘            │
│         │ pointer events,  │ tool changes,    │ Save               │
│         ▼                  ▼                  ▼                    │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐            │
│  │RISEViewport  │   │RenderEngine  │   │ViewportJni   │            │
│  │Bridge.mm     │   │.cpp (Qt)     │   │.cpp          │            │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘            │
└─────────┼──────────────────┼──────────────────┼────────────────────┘
          │   C-API in RISE_API.h (RISE_API_SceneEditController*)    
          ▼                  ▼                  ▼                    
┌────────────────────────────────────────────────────────────────────┐
│                      RISE Library                                   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              SceneEditController                            │   │
│  │  — Tool state machine (Select/Translate/Rotate/.../Scrub)   │   │
│  │  — Render thread + cancel-restart loop                      │   │
│  │  — Pointer-event reinterpretation per tool                  │   │
│  │  — Owns: SceneEditor, InteractiveRasterizer, sinks          │   │
│  └────────┬───────────────────────┬────────────────────────────┘   │
│           │                       │                                 │
│           ▼                       ▼                                 │
│  ┌──────────────────┐   ┌──────────────────────────────────┐       │
│  │  SceneEditor     │   │  InteractivePelRasterizer        │       │
│  │  — Apply(edit)   │   │  — 1 SPP, half-res, center-out   │       │
│  │  — Invariant     │   │  — CancellableProgressCallback   │       │
│  │    chain         │   │  — Idle progressive refinement   │       │
│  │  — EditHistory   │   │                                  │       │
│  │  — PickObject    │   │                                  │       │
│  └────────┬─────────┘   └────────────┬─────────────────────┘       │
│           │                          │                              │
│           ▼                          ▼                              │
│  ┌────────────────────────────────────────────────────────────┐    │
│  │         IScenePriv / IObjectPriv / IObjectManager          │    │
│  │  + new IScene::SetSceneTimeForPreview()                    │    │
│  │  (existing Library — minimal new surface)                  │    │
│  └────────────────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────────────────┘
```

Key flow on a pointer-move during drag:

1. Platform UI passes raw `Point2 px` into `SceneEditController::OnPointerMove(px)`.
2. Controller looks at current tool, computes a `SceneEdit` (e.g., `TranslateObject` with a world-space delta).
3. `mEditor.Apply(edit)` runs the invariant chain (`SetPosition` → `FinalizeTransformations` → `ResetRuntimeData` → `InvalidateSpatialStructure`) and pushes onto `EditHistory`.
4. Controller sets `edit_pending = true`, trips `cancellableProgress->RequestCancel()`, signals condvar.
5. Render thread's in-flight `RasterizeScene` returns within ~1ms (next tile boundary). Loop wakes, calls `RasterizeScene` again on the now-mutated scene. `PrepareForRendering` rebuilds spatial structures internally (cheap if nothing changed; O(n log n) on a moved object).
6. New tiles flow through `IRasterizerOutput` → platform sink → UI thread → screen.

If the user is dragging at 60Hz, only the last edit completes a full render; intermediate frames are abandoned mid-tile. This is exactly the "discard intermediate frames" semantics interactive renderers want, with **zero new locking primitives** beyond the atomic that already exists.

---

## 4. Component Design

### 4.1 `SceneEdit` — value-typed mutation record

[src/Library/SceneEditor/SceneEdit.h](../src/Library/SceneEditor/SceneEdit.h) (new, ~80 LOC)

```cpp
namespace RISE {

struct SceneEdit {
    enum Op {
        // Object transforms
        TranslateObject,        // payload: v3a = world delta
        RotateObjectArb,        // v3a = axis (unit), s = angle radians
        SetObjectPosition,      // v3a = new world pos
        SetObjectOrientation,   // v3a = euler XYZ
        SetObjectScale,         // s = uniform scale
        SetObjectStretch,       // v3a = per-axis stretch

        // Camera
        SetCameraTransform,     // v3a = pos, v3b = target/look-at
        OrbitCamera,            // v3a.x = yaw, .y = pitch (radians)
        PanCamera,              // v3a = world delta
        ZoomCamera,             // s = factor

        // Time
        SetSceneTime,           // s = absolute time

        // Composite (drag bracketing)
        CompositeBegin,         // objectName = label for UI
        CompositeEnd
    };

    Op       op;
    String   objectName;        // empty for camera/time/composite
    Vector3  v3a, v3b;
    Scalar   s;

    // Inverse for undo. Filled in by SceneEditor::Apply before pushing
    // to EditHistory.
    Vector3  invV3;
    Scalar   invS;
};

}  // namespace RISE
```

`SceneEdit` is **trivially copyable**, ~64 bytes. Cheap to drop into a ring buffer; cheap to serialize for replay logs (post-V1 feature).

Why a tagged value type rather than a polymorphic command class:
- The operation set is closed and small (12 ops total including composite markers).
- Drag-as-composite composes naturally: pointer-down emits `CompositeBegin`, every move emits a delta op, pointer-up emits `CompositeEnd`. Undo collapses one composite to one history entry.
- A polymorphic hierarchy (`class ITranslateCommand : public ISceneEdit`) adds vtable + heap allocation per drag tick (potentially 60 × multi-second drags). Disqualified.

### 4.2 `EditHistory` — undo/redo and write-back dirtiness

[src/Library/SceneEditor/EditHistory.h](../src/Library/SceneEditor/EditHistory.h) (new, ~60 LOC)
[src/Library/SceneEditor/EditHistory.cpp](../src/Library/SceneEditor/EditHistory.cpp) (new, ~120 LOC)

```cpp
namespace RISE {

class EditHistory {
public:
    EditHistory(unsigned int maxEntries = 1024);

    void Push(const SceneEdit& edit);
    bool PopForUndo(SceneEdit& outInverse);   // returns false if at start
    bool PopForRedo(SceneEdit& outForward);   // returns false if at end
    void Clear();

    // For round-trip save (§8 Phase 6).
    bool IsObjectDirtyVsBaseline(const String& name) const;
    void EnumerateDirtyObjects(IEnumCallback<String>& cb) const;

    // For UI display
    unsigned int UndoDepth() const;
    unsigned int RedoDepth() const;
    String LabelForUndo() const;   // last composite's label
    String LabelForRedo() const;

private:
    std::deque<SceneEdit> mUndoStack;
    std::deque<SceneEdit> mRedoStack;
    std::set<String>      mDirtyObjects;   // names — for write-back
    unsigned int          mMaxEntries;
    int                   mCompositeDepth = 0;
};

}
```

Composite handling: when `Apply` sees `CompositeBegin`, it pushes a marker onto the undo stack and increments `mCompositeDepth`. Subsequent edits push normally. `CompositeEnd` decrements depth. `Undo` pops back to (and including) the matching `CompositeBegin`, replaying inverses in reverse order.

### 4.3 `SceneEditor` — invariant-enforcing mutator

[src/Library/SceneEditor/SceneEditor.h](../src/Library/SceneEditor/SceneEditor.h) (new, ~90 LOC)
[src/Library/SceneEditor/SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp) (new, ~400 LOC)

```cpp
namespace RISE {

class SceneEditor {
public:
    enum class DirtyScope {
        None,
        ObjectTransform,    // spatial structure invalidated, no photons
        Camera,             // no spatial invalidation
        Time,               // photons may need regen on production render
        TimeAndPhotons      // photons definitely need regen
    };

    SceneEditor(IScenePriv& scene, IJobPriv& job);
    ~SceneEditor();

    // Mutation. Inserts into EditHistory if successful. Returns false
    // and rolls back if the named object can't be found etc.
    [[nodiscard]] bool Apply(const SceneEdit& edit);

    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();
    void BeginComposite(const char* label);
    void EndComposite();

    DirtyScope LastDirtyScope() const { return mLastScope; }

    // Picking — see §6.
    [[nodiscard]] bool PickObject(const Point2& screenPx, String& outName) const;

    // For write-back — see §8.
    const EditHistory& History() const { return mHistory; }

    // Production-render preparation. Idempotent.
    void PreparePhotonsIfDirty(IProgressCallback* progress);

private:
    IScenePriv& mScene;
    IJobPriv&   mJob;
    EditHistory mHistory;
    DirtyScope  mLastScope = DirtyScope::None;
    bool        mPhotonsDirty = false;
    bool        mScenePhotonsExist;  // computed once at construction

    void ApplyTransform(IObjectPriv& obj, const SceneEdit& edit);
    void RunObjectInvariantChain(IObjectPriv& obj);
    // …
};

}
```

**The invariant chain inside `Apply` for a transform edit** is:

```cpp
bool SceneEditor::Apply(const SceneEdit& edit) {
    // Composite markers: just push and return.
    if (edit.op == SceneEdit::CompositeBegin) {
        mHistory.Push(edit);
        return true;
    }
    if (edit.op == SceneEdit::CompositeEnd) {
        mHistory.Push(edit);
        return true;
    }

    if (IsObjectOp(edit.op)) {
        IObject* base = mScene.GetObjects()->GetItem(edit.objectName.c_str());
        if (!base) return false;
        IObjectPriv* obj = dynamic_cast<IObjectPriv*>(base);
        if (!obj) return false;

        SceneEdit recorded = edit;
        FillInverseForUndo(recorded, *obj);   // computed BEFORE mutation
        ApplyTransform(*obj, edit);
        RunObjectInvariantChain(*obj);
        mHistory.Push(recorded);
        mLastScope = DirtyScope::ObjectTransform;
        return true;
    }

    if (IsCameraOp(edit.op)) {
        // … similar pattern with IScenePriv::GetCameraMutable() …
        mLastScope = DirtyScope::Camera;
        return true;
    }

    if (edit.op == SceneEdit::SetSceneTime) {
        // Scrubbing path: skip photon regen.
        mScene.SetSceneTimeForPreview(edit.s);
        // Spatial structure may be invalidated by animator-driven transforms;
        // signal so the controller invalidates before next render.
        mLastScope = mScenePhotonsExist
            ? DirtyScope::TimeAndPhotons
            : DirtyScope::Time;
        if (mScenePhotonsExist) mPhotonsDirty = true;
        // Don't push to history — scrubbing during drag is transient.
        // Final time is captured on CompositeEnd if inside a composite.
        return true;
    }
    return false;
}

void SceneEditor::RunObjectInvariantChain(IObjectPriv& obj) {
    obj.FinalizeTransformations();
    obj.ResetRuntimeData();
    mScene.GetObjects()->InvalidateSpatialStructure();
    // PrepareForRendering happens in the rasterizer, not here — it's
    // O(n log n) and we'd thrash it on a 60Hz drag.
}
```

`FillInverseForUndo` reads the object's current state (`GetPosition`, `GetOrientation`, etc.) and stores it in `recorded.invV3` / `invS` so undo can restore it without re-deriving from the forward op.

### 4.4 `InteractivePelRasterizer` — live preview

[src/Library/Rendering/InteractivePelRasterizer.h](../src/Library/Rendering/InteractivePelRasterizer.h) (new, ~80 LOC)
[src/Library/Rendering/InteractivePelRasterizer.cpp](../src/Library/Rendering/InteractivePelRasterizer.cpp) (new, ~250 LOC)

Subclass of `Implementation::PixelBasedPelRasterizer`. Configuration plus a frame budget — not a new algorithm.

```cpp
namespace RISE { namespace Implementation {

class InteractivePelRasterizer : public PixelBasedPelRasterizer {
public:
    struct Config {
        unsigned int liveSamplesPerPass     = 1;     // 1 SPP during drag
        Scalar       previewScale           = 0.5;   // half-res surface
        Scalar       targetFrameMs          = 33.0;  // budget hint
        bool         progressiveOnIdle      = true;  // refine when idle
        unsigned int idleMaxPasses          = 16;    // cap when idle
        BlockOrder   tileOrder              = BlockOrder::CenterOut;
        bool         useFilteredFilm        = false; // 1×1 box only
        bool         useOIDN                = false;
        bool         usePathGuiding         = false;
        bool         useAdaptiveSampling    = false;
    };

    InteractivePelRasterizer(IRayCaster* caster, const Config& cfg);

    // Override to install Config defaults regardless of what
    // ProgressiveConfig the parent might honor.
    void RasterizeScene(const IScene&,
                        const Rect*,
                        IRasterizeSequence*) const override;

    // Mutate the live config (e.g., switch to idle progressive mode
    // when the user releases the pointer).
    void SetMode(bool idle);

private:
    Config mCfg;
    mutable bool mIdleMode = false;
};

}}
```

**Three implementation choices that matter**:

1. **Center-out tile sequence by default.** Use existing `BlockRasterizeSequence` with `eBlock_CenterOut` — the user sees the part of the image they're pointing at first.

2. **Resolution scaling via parallel preview surface.** The rasterizer has no scaling knob (resolution comes from the camera). The controller maintains a half-resolution `IRasterImage` and a half-resolution shadow camera (the loaded camera with `xres`/`yres` halved); preview renders use the shadow surface; the platform UI nearest-neighbor-upscales for display.

3. **Progressive idle refinement.** When the user releases the pointer, `SetMode(idle=true)` flips to multi-pass mode that runs additional 1-SPP passes via the existing `ProgressiveConfig` until `idleMaxPasses` or the next edit. Progress reported as `currentPass / idleMaxPasses`.

### 4.5 `CancellableProgressCallback` — the cancel hook

[src/Library/SceneEditor/CancellableProgressCallback.h](../src/Library/SceneEditor/CancellableProgressCallback.h) (new, ~50 LOC)
[src/Library/SceneEditor/CancellableProgressCallback.cpp](../src/Library/SceneEditor/CancellableProgressCallback.cpp) (new, ~40 LOC)

```cpp
namespace RISE {

class CancellableProgressCallback : public IProgressCallback {
public:
    CancellableProgressCallback(IProgressCallback* inner /* may be null */);

    void RequestCancel();      // called from UI thread on every Apply
    void Reset();              // called by render thread before each pass

    // IProgressCallback: returns false (cancel) if either inner says false
    // OR our atomic is set.
    bool Progress(const double progress, const double total) override;
    void SetTitle(const char* title) override;

private:
    IProgressCallback* mInner;
    std::atomic<bool>  mCancelled{false};
};

}
```

This composes with each app's existing `IProgressCallback` adapter. The render thread installs it on the rasterizer at construction; UI thread calls `RequestCancel()` from `OnPointerMove`. The dispatcher polls via the existing `Progress()` contract — no other changes to the render path.

### 4.6 `SceneEditController` — cross-platform brain

[src/Library/SceneEditor/SceneEditController.h](../src/Library/SceneEditor/SceneEditController.h) (new, ~120 LOC)
[src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp) (new, ~600 LOC)

```cpp
namespace RISE {

class SceneEditController {
public:
    enum class Tool {
        Select,
        TranslateObject,
        RotateObject,
        ScaleObject,
        OrbitCamera,
        PanCamera,
        ZoomCamera,
        ScrubTimeline
    };

    explicit SceneEditController(IJob& job);
    ~SceneEditController();   // joins render thread

    // Lifecycle
    void Start();
    void Stop();

    // Sinks — set once before Start().
    void SetPreviewSink(IRasterizerOutput* sink);
    void SetProgressSink(IProgressCallback* sink);
    void SetLogSink(ILogPrinter* sink);

    // Tool state machine
    void SetTool(Tool t);
    Tool CurrentTool() const;

    // Pointer events — platform sends raw screen-pixel coords
    // in the platform's surface coordinate system.
    void OnPointerDown(const Point2& px);
    void OnPointerMove(const Point2& px);
    void OnPointerUp(const Point2& px);

    // Direct controls
    void OnTimeScrub(Scalar t);
    void OnTimeScrubBegin();
    void OnTimeScrubEnd();
    void Undo();
    void Redo();

    // Production render — uses the scene's declared rasterizer.
    void RequestProductionRender();

    // Selection
    String SelectedObjectName() const;

    // Round-trip save (§8)
    [[nodiscard]] bool WriteBack(const String& path) const;

    // For platform UI display
    bool CanUndo() const;
    bool CanRedo() const;
    String UndoLabel() const;
    String RedoLabel() const;

private:
    IJob&                     mJob;
    SceneEditor               mEditor;
    InteractivePelRasterizer* mInteractive;
    IRasterizer*              mProduction;
    Tool                      mTool = Tool::Select;
    String                    mSelected;
    Point2                    mLastPx;
    Scalar                    mDepthAtPick = 0;

    IRasterizerOutput*        mPreviewSink = nullptr;
    IProgressCallback*        mProgressSink = nullptr;
    ILogPrinter*              mLogSink = nullptr;

    CancellableProgressCallback mCancelProgress;
    std::thread               mRenderThread;
    std::mutex                mMutex;
    std::condition_variable   mCV;
    std::atomic<bool>         mRunning{false};
    std::atomic<bool>         mEditPending{false};
    std::atomic<bool>         mRendering{false};
    std::atomic<bool>         mIdle{false};   // true after pointer-up

    void RenderLoop();
    void KickRender();   // sets edit_pending, cancels in-flight, notifies
    Vector3 ScreenToWorldDelta(const Vector2& dpx, Scalar depth) const;
    SceneEdit BuildEditForPointerMove(const Vector2& dpx);
};

}
```

**The toolbar state machine lives entirely here.** Pseudocode for `OnPointerMove`:

```cpp
void SceneEditController::OnPointerMove(const Point2& px) {
    if (!mPointerDown) return;
    Vector2 delta = px - mLastPx;
    mLastPx = px;

    SceneEdit edit;
    switch (mTool) {
      case Tool::TranslateObject:
        if (mSelected.empty()) return;   // no target
        edit.op = SceneEdit::TranslateObject;
        edit.objectName = mSelected;
        edit.v3a = ScreenToWorldDelta(delta, mDepthAtPick);
        break;
      case Tool::RotateObject:
        if (mSelected.empty()) return;
        edit.op = SceneEdit::RotateObjectArb;
        edit.objectName = mSelected;
        edit.v3a = ComputeRotationAxis(delta, mEditor);   // cam-relative
        edit.s   = delta.Length() * kPixelsToRadians;
        break;
      case Tool::OrbitCamera:
        edit.op = SceneEdit::OrbitCamera;
        edit.v3a = Vector3(delta.x * kPixelsToRadians,
                           delta.y * kPixelsToRadians, 0);
        break;
      // … pan, zoom, scale, scrub, select-no-op …
    }
    if (mEditor.Apply(edit)) KickRender();
}

void SceneEditController::KickRender() {
    mEditPending.store(true, std::memory_order_release);
    if (mRendering.load(std::memory_order_acquire)) {
        mCancelProgress.RequestCancel();
    }
    mCV.notify_one();
}
```

`OnPointerDown` does picking (for object tools) or just records `mLastPx` (for camera tools). `OnPointerUp` closes any active composite via `mEditor.EndComposite()` and flips `mIdle = true` to enable progressive refinement.

`RenderLoop` runs on `mRenderThread`:

```cpp
void SceneEditController::RenderLoop() {
    while (mRunning.load()) {
        std::unique_lock<std::mutex> lk(mMutex);
        mCV.wait(lk, [&]{
            return mEditPending.load() || mIdle.load() || !mRunning.load();
        });
        if (!mRunning.load()) break;
        mEditPending.store(false);
        lk.unlock();

        mCancelProgress.Reset();
        mRendering.store(true);
        const IScene* scene = mJob.GetScene();
        // PrepareForRendering runs inside RasterizeScene if needed.
        mInteractive->SetMode(mIdle.load());
        mInteractive->RasterizeScene(*scene, /*pRect*/nullptr, /*seq*/nullptr);
        mRendering.store(false);

        // If another edit landed during the render, the cancelled flag
        // tripped and we'll loop back, picking up the latest scene state.
        // If we just finished an idle progressive pass, leave mIdle set
        // so we wake again on the next idle-passes-remaining tick.
    }
}
```

### 4.7 `IScene::SetSceneTimeForPreview` addition

Add a single new method to [src/Library/Interfaces/IScene.h](../src/Library/Interfaces/IScene.h):

```cpp
class IScene : public virtual IReference {
    // … existing pure virtuals …

    /// Like SetSceneTime, but skips photon-map Regenerate calls. Valid
    /// only for interactive-preview rendering with non-photon rasterizers
    /// (typically the InteractivePelRasterizer). The caller MUST invoke
    /// the full SetSceneTime() exactly once before any production render
    /// of a scene that uses photon maps. Default impl in Scene falls
    /// back to SetSceneTime for safety.
    virtual void SetSceneTimeForPreview(const Scalar time) const = 0;
};
```

Implementation in [src/Library/Scene.cpp](../src/Library/Scene.cpp): same as `SetSceneTime` (line 292) minus the `Regenerate(time)` calls on the six photon maps.

**ABI evolution**: this adds a virtual to an existing interface. Per [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md), this changes the vtable layout for any external `IScene` implementer. RISE's only `IScene` implementer is `RISE::Implementation::Scene`, in-tree. No external impls. Safe to add as pure virtual; if a concrete user-supplied `IScene` exists in the wild we are unaware of, they will fail to compile — which is the correct early-warning signal.

### 4.8 `RISE_API` C-API extension

Append to [src/Library/RISE_API.h](../src/Library/RISE_API.h) (currently 2527 lines):

```cpp
namespace RISE {
    // Forward declare the abstract controller so platform adapters can hold
    // an opaque pointer. Real interface is in Library headers.
    class ISceneEditController;

    // Construction / destruction
    bool RISE_API_CreateSceneEditController(
        IJob* pJob,
        ISceneEditController** ppOut
    );
    void RISE_API_DestroySceneEditController(ISceneEditController* p);

    // Lifecycle
    bool RISE_API_SceneEditController_Start(ISceneEditController* p);
    bool RISE_API_SceneEditController_Stop(ISceneEditController* p);

    // Sinks
    bool RISE_API_SceneEditController_SetPreviewSink(
        ISceneEditController*, IRasterizerOutput*);
    bool RISE_API_SceneEditController_SetProgressSink(
        ISceneEditController*, IProgressCallback*);
    bool RISE_API_SceneEditController_SetLogSink(
        ISceneEditController*, ILogPrinter*);

    // Tool state
    bool RISE_API_SceneEditController_SetTool(
        ISceneEditController*, int /*Tool*/);

    // Pointer events
    bool RISE_API_SceneEditController_OnPointerDown(
        ISceneEditController*, Scalar x, Scalar y);
    bool RISE_API_SceneEditController_OnPointerMove(
        ISceneEditController*, Scalar x, Scalar y);
    bool RISE_API_SceneEditController_OnPointerUp(
        ISceneEditController*, Scalar x, Scalar y);

    // Time
    bool RISE_API_SceneEditController_OnTimeScrub(
        ISceneEditController*, Scalar t);
    bool RISE_API_SceneEditController_OnTimeScrubBegin(ISceneEditController*);
    bool RISE_API_SceneEditController_OnTimeScrubEnd(ISceneEditController*);

    // Undo / redo / production / save
    bool RISE_API_SceneEditController_Undo(ISceneEditController*);
    bool RISE_API_SceneEditController_Redo(ISceneEditController*);
    bool RISE_API_SceneEditController_RequestProductionRender(ISceneEditController*);
    bool RISE_API_SceneEditController_WriteBack(
        ISceneEditController*, const char* path);

    // Query (for UI display)
    bool RISE_API_SceneEditController_GetSelectedName(
        ISceneEditController*, char* outBuf, unsigned int bufLen);
    bool RISE_API_SceneEditController_CanUndo(ISceneEditController*);
    bool RISE_API_SceneEditController_CanRedo(ISceneEditController*);
}
```

Per [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md): all additions go to the **end** of the header. New functions only — no signature changes to existing ones.

---

## 5. Cross-Platform Adapter Pattern

Each platform's adapter is now a thin file responsible for:

1. **Three sinks**: `IRasterizerOutput` (image), `IProgressCallback` (progress %), `ILogPrinter` (logs). Each marshals from the render worker thread to the UI thread using whatever the platform offers (Qt signals, GCD blocks, JNI callbacks).
2. **Pointer-event forwarding**: convert the platform's mouse/touch events to `Point2` in surface coordinates and pass to `OnPointerDown/Move/Up`.
3. **Tool toggling**: bind toolbar widgets to `SetTool(Tool::TranslateObject)` etc.
4. **Direct controls**: timeline slider widget binds to `OnTimeScrubBegin/Scrub/End`; menu items bind to `Undo`, `Redo`, `RequestProductionRender`, `WriteBack`.
5. **Viewport surface**: a platform-native widget that displays the half-resolution preview image, upscaled with nearest-neighbor.

That's it. No business logic. No state machine. No race-prone render orchestration.

Estimated adapter sizes after the controller is in place:

| Platform | File | Estimated LOC |
|---|---|---|
| macOS | `RISE-GUI/Bridge/RISEViewportBridge.{h,mm}` + Swift viewport view | ~400 (Obj-C++) + ~250 (Swift) |
| Windows | `RISE-GUI/SceneEditorWidget.{h,cpp}` + adapt `RenderEngine` | ~400 |
| Android | `cpp/SceneEditorJni.{h,cpp}` + `SceneEditorView.kt` | ~300 (C++) + ~250 (Kotlin) |

Compared to the ~700 LOC of duplicated render-orchestration glue today, the per-platform code shrinks meaningfully and only contains platform-idiomatic UI plumbing.

---

## 6. Object Picking

Use the existing intersection path. `IObjectManager::IntersectRay` ([src/Library/Interfaces/IObjectManager.h](../src/Library/Interfaces/IObjectManager.h)) takes a `RayIntersection&` whose `pObject` field is filled in on hit ([src/Library/Intersection/RayIntersection.h](../src/Library/Intersection/RayIntersection.h)).

Inside `SceneEditor::PickObject`:

```cpp
bool SceneEditor::PickObject(const Point2& screenPx, String& outName) const {
    // Convert from preview-surface coords (half-res) to camera coords
    // (full-res) — the controller handles this before calling.
    const ICamera* cam = mScene.GetCamera();
    Ray r;
    cam->GenerateRay(r, screenPx);

    // Defensively prepare spatial structure. No-op if already current.
    mScene.GetObjects()->PrepareForRendering();

    RayIntersection ri(r, RasterizerState{});
    mScene.GetObjects()->IntersectRay(
        ri,
        /*front*/ true,
        /*back*/  false,
        /*exit*/  false
    );

    if (ri.pObject) {
        outName = ri.pObject->GetName();
        return true;
    }
    return false;
}
```

A tiny `pickMutex` (microsecond hold) inside `PickObject` guards `IntersectRay` from racing with edits coming from `Apply`. The render thread does not contest this lock — it never picks.

---

## 7. Timeline Scrubbing

`Scene::SetSceneTime` ([src/Library/Scene.cpp:292](../src/Library/Scene.cpp:292)) calls `pObjectManager->ResetRuntimeData()` then conditionally calls `Regenerate(time)` on every populated photon map. Photon regeneration is **slow** (seconds for a global map). Calling it on every scrub tick is fatal.

The fix is the new `IScene::SetSceneTimeForPreview` (§4.7). Same path minus the `Regenerate` calls.

**Controller state machine for time scrub**:

```
  OnTimeScrubBegin:
      mEditor.BeginComposite("scrub time")
      mInteractive->SetMode(idle=false)

  OnTimeScrub(t):
      SceneEdit edit{op = SetSceneTime, s = t}
      mEditor.Apply(edit)   // calls SetSceneTimeForPreview
      KickRender()

  OnTimeScrubEnd:
      mEditor.EndComposite()
      mIdle = true
      // Photon regen happens later — only if RequestProductionRender fires
```

**Production-render dispatch**:

```cpp
void SceneEditController::RequestProductionRender() {
    Stop();   // join render thread, no more interactive
    mEditor.PreparePhotonsIfDirty(mProgressSink);
    // Now safe to invoke the production rasterizer.
    mProduction->RasterizeScene(*mJob.GetScene(), nullptr, nullptr);
    Start();   // resume interactive
}
```

The user sees a "Preparing photons…" progress indicator during `PreparePhotonsIfDirty` — unavoidable for correctness in photon-mapped scenes.

---

## 8. Round-Trip Save — Phase A (in-place transform rewrite)

[src/Library/Parsers/InPlaceSceneRewriter.h](../src/Library/Parsers/InPlaceSceneRewriter.h) (new, ~50 LOC)
[src/Library/Parsers/InPlaceSceneRewriter.cpp](../src/Library/Parsers/InPlaceSceneRewriter.cpp) (new, ~200 LOC)

**Algorithm**:

1. Read the original `.RISEscene` file into a string.
2. Build a chunk index by re-using `AsciiSceneParser`'s tokenizer (or a lighter dedicated chunk-finder) to record byte spans for each top-level `chunkName { … }`.
3. Walk dirty objects from `EditHistory::EnumerateDirtyObjects`. For each:
   a. Find the matching `object { name "<name>" … }` chunk by scanning indexed chunks.
   b. Within that chunk's body, locate `position`, `orientation`, `scale`, `stretch` lines.
   c. For each present line, replace its values in place; for missing lines we want, insert them just before the closing `}`.
   d. Preserve indentation, comments, blank lines, trailing newlines.
4. Stream the result to the target path. (Save-As: target = user-supplied. Save: target = original path; write to a temp file then atomic rename to avoid partial writes.)

```cpp
namespace RISE {
class InPlaceSceneRewriter {
public:
    InPlaceSceneRewriter(const String& originalText);

    // Returns a new text with the named object's transform fields
    // updated to the values from the live scene. No-op if the object's
    // chunk isn't found — caller should warn.
    [[nodiscard]] bool RewriteObjectTransform(
        const String& objectName,
        const Vector3& position,
        const Vector3& orientation,
        Scalar         scale,
        const Vector3& stretch
    );

    // Same idea for camera. Phase A only handles the camera and dirty
    // objects — lights/painters/etc. pass through verbatim.
    [[nodiscard]] bool RewriteCameraTransform(
        const Vector3& position,
        const Vector3& target,
        const Vector3& up
    );

    [[nodiscard]] String Result() const { return mText; }

private:
    String mText;
    struct ChunkSpan { size_t bodyStart, bodyEnd; String name, kind; };
    std::vector<ChunkSpan> mChunks;
    void IndexChunks();
};
}
```

**What Phase A explicitly does NOT handle**:
- New objects added via the editor (not a V1 feature anyway).
- Object deletions (not a V1 feature).
- Material / light / painter / shader edits (not a V1 feature).
- Animation keyframe edits (not a V1 feature).
- Cameras with non-position/target/up parameterization (e.g., FOV, focal length) — those still pass through verbatim, and we only rewrite transform-shaped fields.

These all ship in Phase B / C — see §15.

---

## 9. Phase 1 — `SceneEditor` + `InteractivePelRasterizer` Skeleton (Library only)

**Goal**: every Library-side primitive compiles and links on all four build systems. New tests prove the invariant chain and undo correctness without any UI.

### 9.1 New files

- [src/Library/SceneEditor/SceneEdit.h](../src/Library/SceneEditor/SceneEdit.h)
- [src/Library/SceneEditor/SceneEditor.h](../src/Library/SceneEditor/SceneEditor.h)
- [src/Library/SceneEditor/SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp)
- [src/Library/SceneEditor/EditHistory.h](../src/Library/SceneEditor/EditHistory.h)
- [src/Library/SceneEditor/EditHistory.cpp](../src/Library/SceneEditor/EditHistory.cpp)
- [src/Library/SceneEditor/CancellableProgressCallback.h](../src/Library/SceneEditor/CancellableProgressCallback.h)
- [src/Library/SceneEditor/CancellableProgressCallback.cpp](../src/Library/SceneEditor/CancellableProgressCallback.cpp)
- [src/Library/Rendering/InteractivePelRasterizer.h](../src/Library/Rendering/InteractivePelRasterizer.h)
- [src/Library/Rendering/InteractivePelRasterizer.cpp](../src/Library/Rendering/InteractivePelRasterizer.cpp)

### 9.2 Files modified

- [src/Library/Interfaces/IScene.h](../src/Library/Interfaces/IScene.h) — add `SetSceneTimeForPreview` virtual.
- [src/Library/Scene.h](../src/Library/Scene.h) and [src/Library/Scene.cpp](../src/Library/Scene.cpp) — implement `SetSceneTimeForPreview`.

### 9.3 Build files (per [CLAUDE.md](../CLAUDE.md)) — every new `.cpp`/`.h` requires:

- [build/make/rise/Filelist](../build/make/rise/Filelist)
- [build/cmake/rise-android/rise_sources.cmake](../build/cmake/rise-android/rise_sources.cmake)
- [build/VS2022/Library/Library.vcxproj](../build/VS2022/Library/Library.vcxproj) and `.filters`
- [build/XCode/rise/rise.xcodeproj/project.pbxproj](../build/XCode/rise/rise.xcodeproj/project.pbxproj) (4 sections × 2 targets — see CLAUDE.md)

### 9.4 Tests

New executable test: [tests/SceneEditorInvariantTest.cpp](../tests/SceneEditorInvariantTest.cpp).

```cpp
// Pseudocode
int main() {
    ParseScene("scenes/Tests/Geometry/shapes.RISEscene", job);
    SceneEditor editor(job.GetScene(), job);

    // Snapshot baseline transforms.
    std::map<String, Matrix4> baseline = SnapshotAllTransforms(job);

    // Apply 100 random edits.
    Random rng(12345);
    for (int i = 0; i < 100; ++i) {
        SceneEdit e = RandomEdit(rng, job);
        ASSERT(editor.Apply(e));
    }

    // Render once — proves spatial structure rebuild works.
    rasterizer->RasterizeScene(*job.GetScene(), nullptr, nullptr);

    // Undo all 100.
    for (int i = 0; i < 100; ++i) {
        ASSERT(editor.Undo());
    }

    // Compare to baseline.
    auto after = SnapshotAllTransforms(job);
    for (auto& [name, M0] : baseline) {
        ASSERT(MatrixEqual(M0, after[name], 1e-9));
    }
    return 0;
}
```

Wire into [tests/Makefile](../tests/Makefile) and the platform test runners ([run_all_tests.sh](../run_all_tests.sh), [run_all_tests.ps1](../run_all_tests.ps1)).

### 9.5 Validation gates (must pass to close Phase 1)

- All four build systems compile cleanly: `make -C build/make/rise -j8 all`, Xcode build of all targets, `msbuild build/VS2022/RISE.sln /p:Configuration=Release`, Android Gradle assemble.
- Header-only changes: run `make -C build/make/rise clean` first, per [CLAUDE.md](../CLAUDE.md).
- `tests/SceneEditorInvariantTest` passes on Linux, macOS, Windows.
- Existing test suite: `./run_all_tests.sh` and `.\run_all_tests.ps1` both pass with no regressions.
- Sample render still works:
  ```sh
  export RISE_MEDIA_PATH="$(pwd)/"
  printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
  ```

### 9.6 Adversarial review for Phase 1

Launch **three reviewers in parallel** per [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md). Orthogonal axes:

1. **Reviewer A — Invariant chain correctness.** Does `SceneEditor::Apply` enforce the full chain (`SetX` → `FinalizeTransformations` → `ResetRuntimeData` → `InvalidateSpatialStructure`) on every transform op? Does the inverse computed by `FillInverseForUndo` actually restore the prior state under floating-point comparison? Does the composite undo replay inverses in the correct reverse order? Does `SetSceneTimeForPreview` correctly skip exactly the photon regen calls and nothing else?
   - **Files**: [src/Library/SceneEditor/SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp), [src/Library/SceneEditor/EditHistory.cpp](../src/Library/SceneEditor/EditHistory.cpp), [src/Library/Scene.cpp](../src/Library/Scene.cpp).

2. **Reviewer B — ABI / API plumbing.** Did adding `SetSceneTimeForPreview` to `IScene` break the vtable layout for any existing implementer? Are all five build files updated for every new `.cpp`/`.h`? Does the `RISE_API_*` extension follow the existing C-API style? Are there derived-class name-hiding issues from new overloads in `SceneEditor`?
   - **Files**: [src/Library/Interfaces/IScene.h](../src/Library/Interfaces/IScene.h), [src/Library/RISE_API.h](../src/Library/RISE_API.h), [build/make/rise/Filelist](../build/make/rise/Filelist), [build/VS2022/Library/Library.vcxproj.filters](../build/VS2022/Library/Library.vcxproj.filters), [build/XCode/rise/rise.xcodeproj/project.pbxproj](../build/XCode/rise/rise.xcodeproj/project.pbxproj). Apply [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md).

3. **Reviewer C — Memory / lifetime / blast radius.** Does `EditHistory`'s `std::deque<SceneEdit>` correctly bound-cap at `mMaxEntries` without unbounded growth on long sessions? Does `SceneEditor::Apply` leak or double-free on the `IObjectPriv*` lookup failure path? Does `IsObjectDirtyVsBaseline` correctly distinguish "edited then undone back to baseline" (clean) from "edited" (dirty)? Find affected callers of `SetSceneTime` that must not call the new preview variant — list them.
   - **Files**: same plus [src/Library/Managers/GenericManager.h](../src/Library/Managers/GenericManager.h), [src/Library/ObjectManager.cpp](../src/Library/ObjectManager.cpp).

Each reviewer reports `severity / confidence / file:line / failure scenario` per finding, plus one named missing test. Word cap: 400.

### 9.7 Phase 1 risks

- **Animator-driven mutation race**: Pre-existing in `IAnimator::EvaluateAtTime`. Untouched in Phase 1; reviewer C should confirm we are not making it worse.
- **`dynamic_cast<IObjectPriv*>` failure path**: Some object classes may extend `IObject` but not `IObjectPriv`. Audit during Reviewer A pass.
- **`SetSceneTimeForPreview` is a new pure virtual**: external `IScene` implementers (if any exist out-of-tree) will fail to compile. Acceptable signal.

---

## 10. Phase 2 — `SceneEditController` + Cancel-Restart Wiring (Library only)

**Goal**: render thread, cancel-restart, controller API surface compile and pass scripted high-frequency-edit tests. Still no UI.

### 10.1 New files

- [src/Library/SceneEditor/SceneEditController.h](../src/Library/SceneEditor/SceneEditController.h)
- [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp)

### 10.2 Files modified

- [src/Library/RISE_API.h](../src/Library/RISE_API.h) — append the C-API wrappers for `SceneEditController` (§4.8).
- [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp) — implement the C wrappers.

### 10.3 Build files

Same five (Filelist, rise_sources.cmake, Library.vcxproj + filters, project.pbxproj).

### 10.4 Tests

New executable: [tests/SceneEditorCancelRestartTest.cpp](../tests/SceneEditorCancelRestartTest.cpp).

```cpp
int main() {
    ParseScene("scenes/Tests/Geometry/shapes.RISEscene", job);
    auto controller = std::make_unique<SceneEditController>(job);
    auto sink = std::make_unique<CountingRasterizerOutput>();
    controller->SetPreviewSink(sink.get());
    controller->Start();
    controller->SetTool(Tool::TranslateObject);

    // Pick a known object (workaround: no UI yet, just set selection).
    controller->ForTest_SelectObject("sphere1");

    // Fire 60 edits over 1 second.
    for (int i = 0; i < 60; ++i) {
        controller->OnPointerDown({100, 100});
        controller->OnPointerMove({100 + i, 100});
        controller->OnPointerUp({100 + i, 100});
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Wait for render thread to settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    controller->Stop();

    // Final image checksum should match a fresh render at the final state.
    ASSERT(sink->LastFrameChecksum() == CheckpointFreshRender(job));
    // At least N cancellations should have happened.
    ASSERT(sink->CancelCount() >= 30);
    return 0;
}
```

(`ForTest_SelectObject` is a `#ifdef RISE_TEST_HOOKS` accessor so the test can prime selection without picking. Keep it test-only.)

### 10.5 Validation gates

- All build systems compile.
- Existing tests pass.
- `tests/SceneEditorCancelRestartTest` runs deterministically — repeat 10× with `--repeat 10`, no flakiness.
- Run with `tsan`/`asan` on macOS and Linux (existing build flags) to catch races and use-after-free.

### 10.6 Adversarial review for Phase 2

Launch **three reviewers in parallel**.

1. **Reviewer A — Thread safety / memory ordering.** Are `mEditPending` / `mRendering` / `mRunning` atomics used with the right memory orderings? Is `KickRender` correctly synchronized with the condvar `wait`? Can `RequestCancel` race with `Reset` such that a freshly-started render thinks it should cancel? Does `Stop` correctly drain in-flight work and join the render thread? Is the `pickMutex` correctly scoped (not held during `RasterizeScene`)?
   - **Files**: [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp), [src/Library/SceneEditor/CancellableProgressCallback.cpp](../src/Library/SceneEditor/CancellableProgressCallback.cpp).

2. **Reviewer B — API/plumbing integrity.** Do all `RISE_API_SceneEditController_*` C wrappers correctly translate to/from the C++ class? Do the platform-bound enum values (Tool, severity, etc.) match between the C-API ints and the C++ enum class? Is the controller's pointer-event coordinate system (preview-surface vs. screen vs. window-points) consistently documented and consistently translated by the controller before reaching the camera? Are there missing `nullptr` checks in any wrapper?
   - **Files**: [src/Library/RISE_API.h](../src/Library/RISE_API.h), [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp), [src/Library/SceneEditor/SceneEditController.h](../src/Library/SceneEditor/SceneEditController.h).

3. **Reviewer C — Cancel-restart end-to-end.** Trace the cancel signal from `OnPointerMove` → `KickRender` → `mCancelProgress.RequestCancel` → `IProgressCallback::Progress` returns false → `RasterizeBlockDispatcher::cancelled` → `GetNextBlock` returns null → render returns. Does each stage propagate within bounded latency? What happens if the user fires 1000 edits/sec — does `mCancelProgress.Reset` get called before each render-loop iteration so the new render isn't pre-cancelled? What about the case where the render finishes naturally just as a cancel arrives?
   - **Files**: [src/Library/SceneEditor/CancellableProgressCallback.cpp](../src/Library/SceneEditor/CancellableProgressCallback.cpp), [src/Library/Rendering/RasterizeDispatchers.h](../src/Library/Rendering/RasterizeDispatchers.h), [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp).

### 10.7 Phase 2 risks

- **Spurious cancel of completed render**: if cancel arrives after the dispatcher loop already exited, the next render might still see `cancelled = true`. `Reset()` between iterations is essential — Reviewer A must confirm.
- **`PrepareForRendering` thrash on every restart**: cheap when `InvalidateSpatialStructure` wasn't tripped (camera-only edits), expensive when geometry moved. Mitigation: only call `InvalidateSpatialStructure` when an object actually moved (already the case in `SceneEditor`). Reviewer C audits.

---

## 11. Phase 3 — macOS Interactive Viewport (Vertical Slice)

**Goal**: macOS app demoably edits scenes interactively — toolbar, drag-to-translate, orbit camera, scrub timeline, picking. Save still emits a log line. This phase is the proof that the shared C++ `SceneEditController` is the right abstraction.

### 11.1 New files

- [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h](../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h)
- [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm](../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm)
- `build/XCode/rise/RISE-GUI/App/ViewportView.swift` — `NSViewRepresentable` that owns a Metal-backed `CAMetalLayer` (or `MTKView`) for upscaling the half-res preview.
- `build/XCode/rise/RISE-GUI/App/EditorToolbar.swift` — toolbar with the eight tool icons.
- `build/XCode/rise/RISE-GUI/App/TimelineSlider.swift` — bottom-of-window scrubber, hidden when the scene has no animator.

### 11.2 Files modified

- `build/XCode/rise/RISE-GUI/App/RISEApp.swift` — add a viewport pane next to the existing text editor (split view).
- `build/XCode/rise/rise.xcodeproj/project.pbxproj` — register all new Swift / Obj-C++ files.

### 11.3 Build files

- `build/XCode/rise/rise.xcodeproj/project.pbxproj` (new GUI files only, not Library — Library targets are unchanged from Phase 2).

### 11.4 macOS bridge — sample API

```objc
// RISEViewportBridge.h
@interface RISEViewportBridge : NSObject

- (instancetype)initWithJobHandle:(void*)jobOpaque;
- (void)start;
- (void)stop;

// Tool
- (void)setTool:(RISEViewportTool)tool;

// Pointer
- (void)pointerDownAtX:(CGFloat)x y:(CGFloat)y;
- (void)pointerMoveAtX:(CGFloat)x y:(CGFloat)y;
- (void)pointerUpAtX:(CGFloat)x y:(CGFloat)y;

// Time
- (void)scrubTimeBegin;
- (void)scrubTime:(double)t;
- (void)scrubTimeEnd;

// Subscriptions
@property (nonatomic, copy) void (^onPreviewImage)(NSData* rgba16, int w, int h);
@property (nonatomic, copy) void (^onProgress)(double pct);
@property (nonatomic, copy) void (^onLog)(NSString* line);

// Actions
- (BOOL)undo;
- (BOOL)redo;
- (void)requestProductionRender;
- (BOOL)writeBackTo:(NSString*)path;
@end
```

The `.mm` impl wraps a `RISE::ISceneEditController*` and translates Obj-C calls to `RISE_API_SceneEditController_*` wrappers.

### 11.5 Tests

Manual smoke test plan:

- Load `scenes/FeatureBased/Combined/tidepools.RISEscene` (per the working-tree status).
- Verify viewport shows the scene at half-resolution, refining when idle.
- Click an object — see the selection name appear in a status bar.
- Switch to Translate tool, drag — object follows the cursor; render updates at >15Hz.
- Switch to Orbit Camera tool, drag — camera orbits target; render keeps up.
- Drag the timeline slider on a scene with an animator — geometry updates without photon-map regen pause.
- Click "Render" — production rasterizer runs (Phase 5 wires this fully; Phase 3 just calls it and waits).

### 11.6 Validation gates

- macOS build succeeds: `xcodebuild -project build/XCode/rise/rise.xcodeproj -target RISE-GUI`.
- Linux/Windows builds untouched and still pass — Phase 3 modifies no Library files.
- Manual smoke test passes for a non-photon scene (e.g., shapes.RISEscene) and a photon scene (e.g., one of the photonmap scenes).
- Drag a complex (>1M tri) scene's object at 60Hz; preview must not drop to <5Hz.

### 11.7 Adversarial review for Phase 3

Launch **two reviewers in parallel** (smaller scope than Phase 1/2).

1. **Reviewer A — UI-thread / render-thread marshalling.** Does the bridge correctly marshal preview images from the render thread back to the UI thread (no UIKit-on-background-thread violations)? Are GCD blocks captured with the right lifetime (no dangling `self` references after bridge dealloc)? Does `pointerMoveAtX:y:` correctly translate from window points → backing pixels → preview-surface coordinates accounting for HiDPI?
   - **Files**: [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm](../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm), `App/ViewportView.swift`.

2. **Reviewer B — Coordinate systems and DPI.** macOS NSEvent coordinates are bottom-left origin in window points; Metal layer is top-left in pixels. Where does the inversion happen? Is HiDPI handled correctly when the preview is half-resolution and the display is 2× scale (so 1 preview pixel = 4 display pixels)? Does picking pass through the inverse correctly? Test on a Retina display and a non-Retina external monitor.
   - **Files**: [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm](../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.mm), `App/ViewportView.swift`, [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp).

### 11.8 Phase 3 risks

- **HiDPI coordinate confusion**: easy to get wrong; smoke tests on multiple display configurations are mandatory.
- **Metal-on-render-thread violation**: render thread must NOT touch CAMetalLayer; only push pixels into a CPU-side `NSData` buffer and signal the UI thread.

---

## 12. Phase 4 — Windows + Android Adapters

**Goal**: Windows (Qt) and Android (Compose+JNI) reach feature parity with macOS Phase 3. Mechanical work — the controller is already proven on macOS.

### 12.1 New files (Windows)

- `build/VS2022/RISE-GUI/SceneEditorWidget.h` — `QWidget` viewport with `paintEvent` that blits the current preview image.
- `build/VS2022/RISE-GUI/SceneEditorWidget.cpp` — pointer event forwarding to the controller.
- `build/VS2022/RISE-GUI/EditorToolbar.h/.cpp` — `QToolBar` with eight buttons.
- `build/VS2022/RISE-GUI/TimelineWidget.h/.cpp` — `QSlider` at the bottom.

### 12.2 Files modified (Windows)

- `build/VS2022/RISE-GUI/MainWindow.{h,cpp}` — add the viewport pane next to the text editor.
- `build/VS2022/RISE-GUI/RenderEngine.{h,cpp}` — adapt to be a thin shim over `SceneEditController` (or remove if `SceneEditorWidget` calls directly).
- `build/VS2022/RISE-GUI/RISE-GUI.vcxproj` and `.filters`.

### 12.3 New files (Android)

- `android/app/src/main/cpp/SceneEditorJni.h`
- `android/app/src/main/cpp/SceneEditorJni.cpp`
- `android/app/src/main/java/<pkg>/SceneEditorView.kt` — `View` with Compose interop.
- `android/app/src/main/java/<pkg>/SceneEditorViewModel.kt`.

### 12.4 Files modified (Android)

- `android/app/src/main/cpp/CMakeLists.txt` (add JNI sources).
- Existing `RiseBridge.cpp` may be split or extended.
- `android/app/build.gradle.kts` if any new resources.

### 12.5 Build files

- Library: unchanged.
- VS2022 GUI: `RISE-GUI.vcxproj`, `RISE-GUI.vcxproj.filters`.
- Android: `android/app/src/main/cpp/CMakeLists.txt`.

### 12.6 Tests

Manual smoke test on each platform (same checklist as Phase 3.5). Plus:

- Cross-platform consistency check: same scene, same drag delta, observe identical final transform values across all three platforms (numerical comparison via the round-trip save once Phase 6 lands; for Phase 4 just visual + edit history checksum).

### 12.7 Validation gates

- Windows: `msbuild build/VS2022/RISE.sln /p:Configuration=Release` succeeds. `.\run_all_tests.ps1` passes.
- Android: `./gradlew assembleRelease` succeeds. APK installs and runs on a test device (or emulator).
- macOS: still builds, no regression from Phase 3.

### 12.8 Adversarial review for Phase 4

Launch **two reviewers in parallel** per platform (4 total reviews) — Windows and Android each get their own pair.

**Windows axes**:
1. **A — Qt event-loop / thread integration.** Does `Qt::QueuedConnection` correctly marshal preview images from render thread to Qt main thread? Are `QImage` / `QPixmap` lifetimes handled correctly? Does `QWidget::mouseMoveEvent` deliver coordinates in the right system?
2. **B — Build / packaging.** Does the `.vcxproj` correctly link the Library? Are runtime dependencies (Qt DLLs) packaged? Does `RISE-GUI.exe` start without "missing DLL" errors on a clean Windows machine?

**Android axes**:
1. **A — JNI lifetime / threading.** Are `JNIEnv*` correctly attached/detached on the render thread? Are Java callbacks invoked from native code with `AttachCurrentThread` properly? Does the JNI bridge correctly handle the Compose view destruction and recreation on configuration change (rotation)?
2. **B — Touch event coordinate translation.** Android `MotionEvent` coordinates are in pixels in the View's coordinate space. Does this map correctly to the controller's expected screen-pixel coordinates given the half-resolution preview surface?

### 12.9 Phase 4 risks

- **Qt + render thread**: Qt has strict UI-thread rules. The render thread must NEVER touch `QPixmap` / `QImage` outside `QMutexLocker`s.
- **Android JNI thread attach leak**: forgetting `DetachCurrentThread` on render thread shutdown leaks the thread.
- **Compose recomposition on rotation**: the render thread must not be torn down on screen rotation; only the View is rebuilt.

---

## 13. Phase 5 — Production Rasterizer Integration

**Goal**: the "Render" button on each platform invokes the scene's declared rasterizer (`pixelpel`/`pathtracing`/`bdpt`/`vcm`/`mlt`/etc.) on the in-memory mutated scene, with correct photon-map regeneration and full-resolution output.

### 13.1 Files modified

- [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp) — implement `RequestProductionRender` per §7. Stop interactive thread, prep photons if dirty, run production rasterizer, restart interactive thread.
- [src/Library/SceneEditor/SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp) — implement `PreparePhotonsIfDirty`.
- Per-platform UI: bind a "Render" menu item / button to `RequestProductionRender`. Show a "Preparing photons…" modal during the prep step.

### 13.2 Tests

New executable: [tests/SceneEditorProductionRenderTest.cpp](../tests/SceneEditorProductionRenderTest.cpp).

```cpp
// Pseudocode
int main() {
    ParseScene(<a photon-mapped scene>, job);
    SceneEditController ctrl(job);
    ctrl.Start();
    ctrl.SetTool(Tool::ScrubTimeline);

    // Scrub. SetSceneTimeForPreview only — no photon regen.
    auto t0 = Clock::now();
    ctrl.OnTimeScrubBegin();
    for (Scalar t = 0; t <= 1.0; t += 0.01) ctrl.OnTimeScrub(t);
    ctrl.OnTimeScrubEnd();
    auto scrubMs = (Clock::now() - t0) / 1ms;
    ASSERT(scrubMs < 1000);   // would be 100s if photon regen ran every tick

    // Production render. Must regen photons exactly once.
    auto t1 = Clock::now();
    ctrl.RequestProductionRender();
    auto renderMs = (Clock::now() - t1) / 1ms;
    // Render time dominates; photon regen happens once inside.

    // Final image must match a fresh full-pipeline render at the same time.
    Image fresh = FreshFullRender(<scene>, /*time*/1.0);
    ASSERT(ImagesMatch(ctrl.LastProductionImage(), fresh, /*epsilon*/1e-3));
    return 0;
}
```

### 13.3 Validation gates

- All build systems pass.
- `tests/SceneEditorProductionRenderTest` passes.
- Manual smoke: edit a photon-mapped scene's object position, click Render — output matches a fresh render of the modified scene at the same SPP.

### 13.4 Adversarial review for Phase 5

Launch **two reviewers in parallel**.

1. **Reviewer A — Photon regeneration correctness.** Is `PreparePhotonsIfDirty` called exactly once per production-render dispatch? What happens if the user clicks Render twice rapidly — does the second invocation correctly skip regen if nothing changed since the first? Does the production rasterizer receive a properly prepared scene (spatial structure, photon maps, irradiance cache all current)? When the scene has an irradiance cache, does it get correctly reset / repopulated?
   - **Files**: [src/Library/SceneEditor/SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp), [src/Library/Scene.cpp](../src/Library/Scene.cpp), [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp).

2. **Reviewer B — Render-thread lifecycle on Stop/Start cycle.** `RequestProductionRender` calls `Stop()` to join the interactive thread, runs production, then `Start()` to spawn a new one. Does the new interactive thread correctly observe the post-production scene state? Are there leaked progress callbacks or sinks across the cycle? What happens if the user starts dragging during the production render (UI must disable interactive controls during this window)?
   - **Files**: [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp), each platform's UI gating logic.

### 13.5 Phase 5 risks

- **Interactive controls re-enabled too soon**: if the production render is async and the user starts dragging, the second `Start()` may race with the production rasterizer. Solution: production render is synchronous from the controller's perspective — `Stop` blocks until production finishes, then `Start`. UI shows a modal / progress dialog.
- **Spatial structure stale across mode change**: ensure `InvalidateSpatialStructure` was tripped if any object moved during scrub-mode and not yet rebuilt.

---

## 14. Phase 6 — Round-Trip Save (Phase A)

**Goal**: File > Save writes a `.RISEscene` file with the current dirty objects' transforms persisted in place. Everything else (camera, lights, materials, painters, etc.) passes through verbatim.

### 14.1 New files

- [src/Library/Parsers/InPlaceSceneRewriter.h](../src/Library/Parsers/InPlaceSceneRewriter.h)
- [src/Library/Parsers/InPlaceSceneRewriter.cpp](../src/Library/Parsers/InPlaceSceneRewriter.cpp)

### 14.2 Files modified

- [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp) — implement `WriteBack(path)`. Read the original scene text (the controller stores the path the scene was loaded from), invoke `InPlaceSceneRewriter` for each dirty object, write to a temp file, atomic-rename.
- [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp) — `RISE_API_SceneEditController_WriteBack` wrapper.
- Per-platform UI: bind File > Save / Save As menu items.

### 14.3 Build files

Library: Filelist, rise_sources.cmake, Library.vcxproj + filters, project.pbxproj.

### 14.4 Tests

New executable: [tests/SceneEditorRoundTripTest.cpp](../tests/SceneEditorRoundTripTest.cpp).

```cpp
int main() {
    ParseScene("scenes/Tests/Geometry/shapes.RISEscene", job);
    SceneEditor editor(job.GetScene(), job);

    // Apply known transforms.
    editor.Apply({.op = SceneEdit::SetObjectPosition,
                  .objectName = "sphere1",
                  .v3a = Vector3(1.5, 2.0, -3.0)});
    editor.Apply({.op = SceneEdit::SetObjectScale,
                  .objectName = "sphere1",
                  .s = 2.0});

    // Save.
    String tmpPath = TempFile();
    InPlaceSceneRewriter writer(LoadFile("scenes/.../shapes.RISEscene"));
    writer.RewriteObjectTransform("sphere1",
        Vector3(1.5, 2.0, -3.0), Vector3::Zero(), 2.0, Vector3::One());
    WriteFile(tmpPath, writer.Result());

    // Re-load.
    Job job2;
    ParseScene(tmpPath, job2);
    auto* obj2 = dynamic_cast<IObjectPriv*>(
        job2.GetScene()->GetObjects()->GetItem("sphere1"));
    ASSERT(VectorEqual(obj2->GetPosition(), Vector3(1.5, 2.0, -3.0), 1e-9));
    ASSERT(ScalarEqual(obj2->GetScale(), 2.0, 1e-9));

    // The non-target part of the file must be byte-identical.
    auto orig = LoadFile("scenes/.../shapes.RISEscene");
    auto saved = LoadFile(tmpPath);
    ASSERT(NonObjectChunksByteEqual(orig, saved, /*spareChunk*/"sphere1"));
    return 0;
}
```

### 14.5 Validation gates

- All builds pass.
- `tests/SceneEditorRoundTripTest` passes.
- Manual: load `tidepools.RISEscene`, drag a tidepool, save, diff the file — only the moved object's transform lines should differ; everything else (comments, blank lines, indentation) should be byte-identical.
- Re-load saved file in a fresh app instance; transforms should match.

### 14.6 Adversarial review for Phase 6

Launch **three reviewers in parallel**.

1. **Reviewer A — Parser/rewriter contract correctness.** Does `InPlaceSceneRewriter::IndexChunks` correctly identify chunk boundaries — including chunks with nested `{ }` (e.g., a material inside an object)? Are line endings (CRLF vs. LF) preserved correctly across platforms? What happens if the scene file uses tabs vs. spaces — is indentation preserved for inserted lines? Does the rewriter handle objects whose `position` line is missing entirely (insert before `}`)?
   - **Files**: [src/Library/Parsers/InPlaceSceneRewriter.cpp](../src/Library/Parsers/InPlaceSceneRewriter.cpp), [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp).

2. **Reviewer B — Round-trip semantic equivalence.** After Save → Re-load, are all object transforms numerically identical to the in-memory state pre-Save? What's the floating-point precision of the textual representation we write — does `%g` lose precision? Does the rewriter handle scientific notation correctly (`1e-3` vs `0.001`)? Run a fuzzer that applies 1000 random transforms, saves, reloads, and asserts equivalence.
   - **Files**: [src/Library/Parsers/InPlaceSceneRewriter.cpp](../src/Library/Parsers/InPlaceSceneRewriter.cpp), the writer's float formatter.

3. **Reviewer C — Filesystem safety.** Is the save atomic — does a crash mid-write leave the user with a half-written scene file? Does the temp-file-then-rename pattern work on Windows (where rename-over-existing is restricted)? On Android (sandboxed FS)? What about iCloud-synced directories on macOS?
   - **Files**: [src/Library/SceneEditor/SceneEditController.cpp](../src/Library/SceneEditor/SceneEditController.cpp), platform-specific file I/O helpers.

### 14.7 Phase 6 risks

- **Float formatting precision loss**: `%g` defaults to 6 significant digits; `%.17g` is round-trip-safe for `double`. Use `%.17g` for `Scalar` writes.
- **CRLF on Windows**: scene files committed by Windows users may have CRLF; we must preserve them.
- **Comments on parameter lines**: a line like `position 1 2 3 # original setup` — does our rewriter preserve the trailing comment? (Recommended: yes, by replacing only the numeric tokens.)
- **Save-As with new filename**: simple — we read from the original path, write to the new path. Save (in-place) needs atomic rename.

---

## 15. Future Phases (Out of Scope for This Initiative)

- **Phase B — descriptor-driven re-emit for cameras and lights.** Add `Serialize` companion to each `IAsciiChunkParser`'s descriptor. Lets us round-trip more than just transforms — camera FOV changes, light intensity edits, painter parameter tweaks. Requires per-interface getter audit; estimated 3-5 days additive interface work plus 2-3 days serializer.
- **Phase C — full re-serialize from in-memory state.** Topo-sort the manager dependency graph (painters → materials → objects → lights → rasterizer), call `Serialize` on every chunk, emit a fresh `.RISEscene`. Required for "Save As" fidelity and for scripting / plugin export.
- **Material / shader / painter editing in the viewport.** Currently text-editor-only.
- **Multi-select and group operations.**
- **Animation keyframe authoring.** Adding/removing keyframes from the viewport.
- **Vertex-level mesh editing.**
- **Replay / scripting.** The `EditHistory` is already serializable; a follow-on can persist edit logs as `.RISEsession` files for crash recovery and macro recording.

---

## 16. Adversarial Review Playbook (Process Reference)

Per [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md), every phase ends with a parallel multi-reviewer pass before merge. Recap of the procedure:

1. **Self-audit first.** Before launching reviewers, write down the 3-5 ways the change is most likely wrong, the contracts that must not regress, and the tests that would best expose a break. Use this to choose better axes.

2. **Pick orthogonal axes.** Two reviewers with overlapping remits produce correlated findings and shared blind spots. The standard axes for this initiative:
   - Math / algorithmic correctness
   - Thread safety (races, memory ordering, lifetime)
   - API / plumbing integrity (parser ↔ Job ↔ API ↔ constructor agreement)
   - ABI preservation (vtable layout, exported signatures, name hiding)
   - Memory / allocation (sizes, alignment, caps, leaks)
   - Blast radius / missing tests (affected callers, uncovered cases)

3. **Self-contained reviewer prompts.** Each reviewer starts from zero context. Every prompt must include:
   - One paragraph describing what changed in plain English.
   - The specific concern for this reviewer (the lens).
   - Absolute file paths to the critical files.
   - Pointed lettered questions ("A) Does X? B) Does Y?").
   - A reporting format: `severity` (critical / major / minor) + `confidence` (high / medium / low) + `file:line` + concrete failure scenario.
   - A requirement to call out one missing test by name.
   - A word cap (400 words typical).

4. **Triage findings.** Critical/major findings block merge until fixed. Minor findings file as follow-up tickets. Re-review after fixes for any critical finding.

5. **No "looks good" reviews.** A reviewer reporting zero findings should be queried — either the prompt was too vague, or they should explicitly state "I checked X, Y, Z and found nothing actionable."

Per-phase axis assignments are listed in §9.6, §10.6, §11.7, §12.8, §13.4, §14.6.

---

## 17. Risks and Out-of-Scope Items (cross-cutting)

### Known issues deferred from adversarial review

- **Edit-while-rendering scene-state race.** `OnPointerMove → SceneEditor::Apply` mutates camera/object fields on the UI thread without locking against the render thread, which is reading those same fields through the ray caster. The cancel signal arrives at the next tile boundary, so a small window exists where a tile reads a torn write (e.g. `Point3` partially updated, basis vectors mid-recompute → `NaN` after `Vector3Ops::Normalize`). Current symptom: rare firefly pixels on fast drags, transient (cancel-restart fires a clean render right after with the correct state). Proper fix is a scene-state RW lock or double-buffered camera; both are invasive. TSAN flags the race today.
- **Per-property `RegenerateData` inside composite undo.** Each restored property edit in `SceneEditor::Undo`'s composite branch calls `cam->RegenerateData()` mid-loop instead of deferring to the trailing call. Wasteful (multiple basis recomputes per composite) and a minor exposure to the race above. Acceptable today — composite property undo only fires for property-panel composites, which are rare.

### Other risks

- **Pre-existing animator race.** [docs/ARCHITECTURE.md](ARCHITECTURE.md) flags `IAnimator::EvaluateAtTime` as a known race during multi-threaded temporal sampling. Unrelated to interactive scrubbing correctness for the `pixelpel` case — the interactive rasterizer is single-thread per render anyway from the editor's standpoint, and timeline scrub never spawns a multi-threaded animation render. Adding `SetSceneTimeForPreview` does not introduce a new race.
- **Production rasterizer + photon regen pause.** Unavoidable for correctness in photon-mapped scenes. UI must show a clear "Preparing photons…" indicator. Document in user-facing docs.
- **macOS `RISESceneEditorBridge` naming clash**: existing bridge is for the **text editor**. New 3D bridge is `RISEViewportBridge`. Per-platform analogues: `ViewportEngine` (Windows), `ViewportJni` (Android).
- **Phase B/C round-trip gaps**: editors that touch anything beyond object transforms (cameras, lights, materials) won't round-trip in V1. Document in release notes.
- **Out of scope (V1)**: material edit, light edit, mesh edit, multi-select, keyframe authoring, shader-op editing, group operations, copy/paste of objects.

---

## 18. Read Next

- [docs/ARCHITECTURE.md](ARCHITECTURE.md) — scene immutability invariant, photon regen behavior, animation race.
- [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md) — review playbook.
- [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) — apply when extending `IScene` and `RISE_API.h`.
- [src/Library/README.md](../src/Library/README.md) — Library map.
- [CLAUDE.md](../CLAUDE.md) — five-build-file checklist, thread-priority policy, source-file add/remove discipline.

---

## 19. Implementation Progress Log

This section records what shipped (vs. the original plan) and any requirement changes that were made along the way. The original §9-§14 phase plans are unmodified; this log is the source of truth for *current* state and reasoning behind deviations.

### 19.1 Phase 1 — Library skeleton (shipped)

Delivered as planned. New files under [src/Library/SceneEditor/](../src/Library/SceneEditor/):

- `SceneEdit.h` — value-typed mutation record with prev-state fields for undo (`prevTransform`, `prevTime`, `prevCameraPos/LookAt/Up`, `prevPropertyValue`).
- `EditHistory.{h,cpp}` — bounded ring buffer with composite markers; `TrimToMax` pops whole composites atomically (round-3 review fix).
- `SceneEditor.{h,cpp}` — friend-class mutator. Apply/Undo/Redo branches for object/camera/time/property ops. Composite undo/redo loops with scope tracking (`sawObjectOp`, `sawCameraOp`, `sawTimeOp`, `sawPropertyOp`) so the dirty scope is the maximum-significance op in the composite (round-4 review fix).
- `CancellableProgressCallback.{h,cpp}` — wraps an `IProgressCallback` and ANDs its `Progress()` return with an atomic cancel flag. `mInner` is `std::atomic<IProgressCallback*>` so `SetInner` from the UI thread is safe against concurrent `Progress()` calls from worker threads (round-1 review fix).
- `CameraIntrospection.{h,cpp}` — descriptor-driven property snapshot for the active camera. `Inspect()` returns `vector<CameraProperty>`; `SetProperty()` parses string values back via the same `IAsciiChunkParser` formatters. Filters duplicate scalar params (pitch/roll/yaw) keeping vec3 versions; logs a warning on unknown / read-only properties.
- `InteractivePelRasterizer.{h,cpp}` (under `src/Library/Rendering/`) — subclass of `PixelBasedPelRasterizer` with a `Config` struct (`previewScale`, `idleMaxPasses`, `tileOrder`, `progressiveOnIdle=false`).

`tests/SceneEditorBasicsTest.cpp` exercises the full invariant chain via API and verifies undo round-trips transforms within 1e-9.

### 19.2 Phase 2 — Cancel-restart controller (shipped)

`SceneEditController.{h,cpp}` — render thread, condvar, atomics. Owns the `SceneEditor`, the interactive rasterizer, the toolbar state machine, the cancel-restart loop, and the descriptor-driven property snapshot.

Round-1 review fixed a condvar lost-wakeup: `KickRender` and `Stop` now hold `mMutex` around the store-then-notify so the render thread cannot park between its predicate check and `cv.wait`.

`tests/SceneEditorCancelRestartTest.cpp` validates that rapid edits cancel in-flight renders. **Test fix** (during multi-level scaling work): the test was creating `new Job()` without a camera, so `SceneEditor::Apply` for `OrbitCamera` returned false at the `if (!baseCam) return false;` check and the test's pointer-driven cancel path never fired. Added `AttachDefaultCamera` helper that constructs a 64×64 pinhole camera and calls `IScenePriv::SetCamera` before the controller starts. Test now reports `cancels=30 aborts=30 completed=1` (30 motion edits each cancel the prior in-flight render; final pass after `OnPointerUp` runs to completion).

### 19.3 Phase 3 — macOS viewport (shipped)

Bridge: `build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.{h,mm}`. SwiftUI: `App/ViewportView.swift`, `App/ViewportToolbar.swift`. Toolbar binding to the C-API `SetTool` enum.

Tooltips on each toolbar button (per user feedback during implementation). Selected toolbar button shows accent-color background + tinted icon.

Cursor binding: when a tool is selected, the viewport canvas's cursor switches to that tool's `NSCursor` (e.g., `closedHand` for orbit, `crosshair` for select). The cursor reverts to `.arrow` when interaction is disabled.

**Picking** uses `IObjectManager::IntersectRay` against a camera-generated primary ray. Implementation skipped the half-resolution coordinate conversion path because the platform UI never sees the lower-resolution preview surface — the preview sink upscales server-side via the camera dimension swap (see §19.7).

### 19.4 Properties panel (added during Phase 3, not in original plan)

Right-side panel reflecting the active camera's parameters via the `IAsciiChunkParser` descriptor. Built from `CameraIntrospection::Inspect`. Edits round-trip back through `SetCameraProperty` SceneEdit op so they participate in undo/redo. Surfaced over C-API as the `RISE_API_SceneEditController_Property*` family (Count / Name / Value / Description / Kind / Editable / RefreshProperties / SetProperty).

The panel re-snapshots every frame the bridge dispatches an image (so drag-driven edits show up in the panel) but suppresses the snapshot while the panel's text field is being actively edited (so the user's in-progress text is not overwritten).

The panel is mirrored on Windows (`ViewportProperties` Qt widget) and Android (`ViewportPropertiesPanel` Compose). Platform UIs disable the panel when `interactionEnabled=false` (see §19.5).

### 19.5 Always-on viewport — replaced "Interact mode" toggle

**Original plan** (pinned design): the viewport was an explicit mode the user toggled into via an "Interact" button. Production render and viewport were mutually exclusive.

**Decision change** (post-Phase-3): "Let's ditch having a separate interact mode. Always in that mode. Render button stops viewport, runs production, restarts viewport. While rendering all interaction disabled."

Resulting requirements:
- Viewport is the default editing surface as soon as a scene loads. No separate Interact button.
- Render flow: UI thread calls `bridge.stop()` → engine runs production rasterizer (synchronous) → on completion, UI thread calls `bridge.suppressNextFrame()` then `bridge.start()`.
- During `Loading` / `Rendering` / `Cancelling`, all viewport widgets (toolbar, canvas pointer events, timeline slider, properties panel) are visually disabled (greyed) so edits cannot race the production rasterizer.
- State-driven enablement is computed in each platform's view-model and threaded as `interactionEnabled: Bool` (Swift) / `setEnabled(bool)` (Qt) / `interactionEnabled: Boolean` (Compose) into all child widgets.

Files touched: macOS `ContentView.swift` / `RenderViewModel.swift` / `ViewportView.swift`; Windows `MainWindow.cpp` / `ControlsWidget.cpp` (removed `m_interactBtn`); Android `RenderScreen.kt` / `RenderViewModel.kt` (removed `_interactiveMode`/`setInteractiveMode`) / `ViewportPane.kt` (added `interactionEnabled` parameter and disabled inner widgets).

### 19.6 Sink dispatch policy (cancel-guard added then reversed; suppress-next-frame kept)

Two end-of-pass guards were originally added to each platform's `ViewportPreviewSink` after Phase 4:

1. **Cancel guard** *(added then removed)*. The rasterizer's `FlushToOutputs` fires unconditionally even when the dispatcher returned early for a cancel. The initial concern: post-production, the bridge restart would dispatch a half-rendered preview buffer that flashed over the just-completed production image. The first fix consulted `SceneEditController::IsCancelRequested()` (promoted from `protected` to `public`) and dropped the dispatch when true.

   **Reversal**: in practice, during a fast drag every pointer move trips the cancel flag, so almost every in-flight render is cancelled mid-pass. With the guard active the sink dropped all of them, and the user only ever saw the post-pause refinement passes (§19.7) — visually indistinguishable from a throttled viewport. The user feedback was direct: *"For interactive there should be no throttling updates to the screen."*

   Final policy: the sink dispatches **every** `OutputImage` call. Center-out tile order makes partial buffers visually usable (the centre of the image fills first, edges later). The post-production-flash concern that motivated the cancel guard is fully covered by the `SuppressNextFrame()` mechanism below — there is no remaining case where dropping cancelled frames was load-bearing.

   The promoted `SceneEditController::IsCancelRequested()` accessor stays public — it's a small surface and may be useful for instrumentation later. The `m_controller` / `mController` field on each sink is now unused dead code; left in place because removing it touches the bridge wiring on three platforms for no functional gain.

2. **Suppress-next** *(kept)*. Drops exactly one upcoming dispatch. Used right after a production render returns so the production image stays visible until the user actually starts dragging — otherwise the bridge's first preview render after restart would overwrite the production result. Each platform exposes the trigger:
   - macOS: `[RISEViewportBridge suppressNextFrame]`
   - Windows: `ViewportBridge::suppressNextFrame()`
   - Android: `RiseBridge::viewportSuppressNextFrame()` + JNI `nativeViewportSuppressNextFrame`

   The flag is atomic so the UI thread can flip it safely while the render thread fires `OutputImage` from a worker.

### 19.7 Multi-level adaptive scaling with idle refinement (added post-Phase-5)

**Original plan**: half-resolution (scale=2) preview during drag.

**Evolved requirement**: multi-level subsampling deep enough to keep the viewport responsive on heavy scenes, plus progressive upsampling as the user slows and stops. Final state delivers 6 scale levels (1, 2, 4, 8, 16, 32) and three feedback loops in `SceneEditController`:

1. **During-motion adaptation** in `DoOneRenderPass` — measures wall-clock per pass and steps the scale toward a 30Hz budget. ×2 step on `>kTargetMs` (33ms), **×4 jump on `>kSlowMs`** (100ms) so heavy scenes ramp from scale=4 to scale=16 in two slow frames instead of three, /2 step on `<kFastMs` (16ms).

2. **Resume-after-pause snap** in `OnPointerMove` — if the time since the last `KickRender` exceeds `kRefineIdleMs` (150ms) and the current tool is a motion tool, snap `mPreviewScale` back up to `kPreviewScaleMotionStart` (4). Without this, the first frame after a long pause would render at scale=1 (refined down by loop 3 below) and stall the viewport.

3. **Idle refinement loop** in `RenderLoop` — when `mPointerDown && mPreviewScale > 1`, the wait uses `cv.wait_for(kRefineWakeMs)` (100ms) instead of an indefinite wait. On timeout (no edit landed within the wake interval), if `sinceEdit > kRefineIdleMs`, the loop sets `mInRefinementPass=true`, halves the scale, and runs a refinement pass at the finer resolution. Refinement passes skip the during-motion adaptation (otherwise a heavy pass at scale=2 would yo-yo back to 4 only to be walked back down by the next refinement tick). Result: after the user stops moving, the image refines from coarse to full-resolution over ~500ms.

State added to `SceneEditController`:
- `std::atomic<bool> mPointerDown` (was plain bool — promoted because the render thread reads it)
- `std::atomic<long long> mLastEditTimeMs` — stamped by `KickRender` and `OnPointerDown`
- `bool mInRefinementPass` — set inside `RenderLoop` before each pass

Constants in `SceneEditController.h`:
```
kPreviewScaleMin = 1, kPreviewScaleMax = 32, kPreviewScaleMotionStart = 4
kTargetMs = 33, kSlowMs = 100, kFastMs = 16
kRefineIdleMs = 150, kRefineWakeMs = 100
```

The platform sinks see the scaled preview surface via `CameraCommon::SetDimensions(w, h)` swap before/after `RasterizeScene`. The platform UI nearest-neighbor-upscales for display. Production renders use the original camera dimensions untouched.

### 19.8 Persistent IRasterImage + skip-clear for interactive (added late in iteration)

**Trigger**: user reported colour flashes during fast manipulation: *"I am still seeing flashes of color, like the buffer is being cleared before every render, we don't want that for the interactive renders, we want the renderer to start as fast as it can on the existing buffer."*

**Root cause**: `PixelBasedRasterizerHelper::RasterizeScene` (1) allocated a fresh `RISERasterImage` on every call, then (2) cleared it to a random pastel colour as a debug visualization. Both behaviours are fine for production renders (always run to completion, fresh per call) but produce visible flashes during the interactive cancel-restart loop where partial buffers are dispatched mid-render.

**Solution**: factored the image lifecycle and per-pass clear into virtual hooks:

- `IRasterImage* AcquireRenderImage(unsigned int w, unsigned int h) const` — default impl returns a persistent buffer that survives across `RasterizeScene` calls. Reallocates only on dimension change (e.g., camera resize for preview-scale stepping). State (`mPersistentImage`, `mPersistentW/H`) lives on the base helper as `mutable` fields. Production rasterizers get the same persistence — desired side-effect: `GetLastRenderedImage()` lets the host UI grab the final pixel data for a "save image" feature without re-rendering.
- `void ReleaseRenderImage(IRasterImage*) const` — paired counterpart. Default decrements the borrowed reference; the persistent reference holds the image alive for the rasterizer's lifetime.
- `void PrepareImageForNewRender(IRasterImage&, const Rect*) const` — default impl runs the existing clear-to-random-pastel + `OutputIntermediateImage` notification. Interactive override is empty (no clear, no notify).

`InteractivePelRasterizer` overrides only `PrepareImageForNewRender` (skip clear); it inherits the persistent-buffer behaviour from the base, so the previous frame's pixels stay under the in-progress tiles during cancel-restart.

The destructor of `PixelBasedRasterizerHelper` was extended to `safe_release(mPersistentImage)` — was previously not needed since the buffer was released at end of every `RasterizeScene`.

### 19.9 Center-out tile order for interactive renders (added late in iteration)

**Trigger**: user reported subsampled renders appearing in the upper-left corner: *"when we do subsampled renders, does the result get upscaled to the entire buffer properly? I feel like its not and so the render happens at half, quarter or whatever resolution into the upper left of the screen."*

**Root cause**: the upscaling was correct — `ViewportNSView.draw` aspect-fit scales the small `NSImage` to fill the bounds. But `Config.tileOrder = TileOrder_CenterOut` (the documented default for interactive rendering) was never wired up — `RasterizeScene` line 520 unconditionally created a `MortonRasterizeSequence` (Z-curve, fills tiles starting upper-left). Combined with the persistent buffer (§19.8), partial buffers showed new content materializing from the upper-left while the rest of the frame held previous state.

**Solution**: added a virtual factory `CreateDefaultRasterSequence(unsigned int tileEdge) const` to `PixelBasedRasterizerHelper`. Default returns Morton (production behaviour unchanged). `InteractivePelRasterizer` overrides to honour `Config.tileOrder`, returning `BlockRasterizeSequence(tileEdge, tileEdge, type)` where `type=0` (centre-out) is the default. Partial buffers now fill from the centre outward — the most useful tiles for an interactive viewport.

### 19.10 Post-release 4-SPP polish pass with elevated max-recursion (added late in iteration)

**Trigger**: user request to add quality polish at rest: *"On mouse up, after we are done rendering the full image at 1spp, let's do one last one of the full image at 4spp"*, plus *"for that final 4 sample render bump the number of glossy, reflected and refracted bounces to 1."*

**Solution**: chained polish pass with elevated-recursion ray caster swap.

- `InteractivePelRasterizer::SetSampleCount(unsigned int n)` — `n=1` (default) clears the rasterizer's `pSampling`; `n>1` lazy-installs a MultiJittered 2D kernel sized to `n` samples and disables progressive mode so the next render is a single multi-sampled pass.
- `InteractivePelRasterizer::SetPolishRayCaster(IRayCaster*)` — installs an optional secondary caster. When `SetSampleCount(>1)` is called and a polish caster is installed, `pCaster` is swapped to it for the duration of the polish pass; `SetSampleCount(1)` swaps back. The bridge constructs both casters at scene-load (preview maxR=1, polish maxR=2 → one bounce of refl/refr/glossy) and passes the polish one via this setter.
- **Controller polish state machine**:

  | State | Meaning |
  |---|---|
  | `None` | No polish in flight. Default. |
  | `FinalRegularRunning` | `OnPointerUp` set this after kicking the regular 1-SPP scale=1 final pass. Post-pass logic transitions to `PolishQueued` and re-kicks. |
  | `PolishQueued` | Upcoming pass is the polish. `RenderLoop` calls `SetSampleCount(4)` before the pass, `SetSampleCount(1)` after. |

  Any user-driven `KickRender` (pointer move, scrub, undo, etc.) resets state to `None`, cancelling the chain. `OnPointerUp` works around this by setting `FinalRegularRunning` *after* its own `KickRender` call. CAS guards in the post-pass transitions prevent races where a concurrent edit invalidates the chain mid-transition.

- **Bridge wiring**: macOS `RISEViewportBridge.tryBuildLivePreviewForJob` constructs both casters; both are released in `releaseLivePreview`. The polish caster is best-effort — if construction fails, polish falls back to the preview caster (no bounces) and the polish pass still gives a 4-SPP anti-aliased image.

### 19.14 Toolbar pared to camera-only + cursor scoped to image area (added late in iteration)

**Trigger**: user requests, *"Let's drop the object move, rotate and translate controls. This is too much complexity for this state of the app. So remove those from the tool bar."* and *"The pointer icon should revert back to default system icon whenever the pointer moves outside of the manipulation area (i.e. the screen area) and should become the pointer for that type of manipulation when it enters the screen area."* and *"Remove the timeline scrubber, it doesn't make sense since on animated scenes we have a timeline bar that shows up in the bottom."*

**Toolbar**: each platform now filters the displayed tools through a `visibleInToolbar` list containing only `Select`, `OrbitCamera`, `PanCamera`, `ZoomCamera`. The C++ `SceneEditController::Tool` enum keeps all eight values so the controller is forward-compatible if the dropped tools come back later — the dropped values just aren't reachable from the UI.

- macOS: `ViewportTool.visibleInToolbar` (Swift) — toolbar `ForEach` iterates that list.
- Windows: `ViewportToolbar::ViewportToolbar` constructor adds only the four buttons.
- Android: the Kotlin `ViewportTool` enum was reduced to just the four values; controller-side rawValues (1/2/3/7) are unreachable from the Compose UI.

**Cursor**: tool cursor only takes effect over the aspect-fit rendered-image area; outside it (and outside the widget), the parent / window's system arrow takes over.

- macOS `ViewportNSView`: `resetCursorRects` adds a cursor rect for `currentImageDrawRect()` (the aspect-fit draw rect), not full bounds. Image setter and `resize` invalidate the cursor rects so they recompute as the image / view size changes.
- Windows `ViewportWidget`: `setActiveTool` stores the desired `Qt::CursorShape` but no longer calls `setCursor` directly. `mouseMoveEvent` calls `updateCursorForPosition`, which calls `setCursor` over `imageDrawRect()` and `unsetCursor` outside. `leaveEvent` calls `unsetCursor` so leaving the widget restores the system arrow.
- Android: no-op (touch UI).

**Timeline scrubber tool**: removed from the toolbar (per-platform filter above). The bottom timeline bar at the foot of the viewport keeps its existing behaviour (scrub → `OnTimeScrub` → preview re-renders at the new time). Production-render time-handling was verified: `RISEBridge.rasterize` doesn't reset scene time, so clicking Render after a scrub renders at the scrubbed time; `RISEBridge.rasterizeAnimation` calls `RasterizeAnimationUsingOptions` which uses the scene-declared start/end, so Render Animation always covers the full animation regardless of current scrub position.

### 19.15 Right-side panel becomes a four-section accordion (Phase 1)

**Trigger**: user request to replace the single-property panel with an accordion covering the scene's main entity types.

**Mechanism**: selection state is generalized from a single string (`mSelected`) to a `(Category, entityName)` tuple in `SceneEditController`.  `Category` ∈ `{None, Camera, Rasterizer, Object, Light}` (numeric values 0..4); `PanelMode` shares those values.  `CurrentPanelMode()` is derived purely from the selection tuple.

**Accordion contract**:
- One section per category, in order: Cameras, Rasterizer, Objects, Lights.
- Single selection across the whole panel — picking a row in any section auto-collapses the others, expands this one, and surfaces that entity's property rows directly under it.
- Camera and Rasterizer selections trigger scene mutations (`IScenePriv::SetActiveCamera`, `IJob::SetActiveRasterizer`); Object and Light selections are pure UI state.
- Click-on-image object picking routes through `SetSelection(Object, hitName)` so the Objects section auto-expands.
- Camera-tool gestures (Orbit / Pan / Zoom / Roll) auto-promote selection to `(Camera, activeName)` so the Cameras section opens during the drag.

**Per-category introspection**:
- `Camera`: existing `CameraIntrospection::Inspect` (descriptor-driven, editable).
- `Rasterizer`: `RasterizerIntrospection` — read-only Phase 1 (Type / Active / Registered count / "Editing: phase 2"); rasterizer parameter editing is Phase 3.
- `Object`: `ObjectIntrospection` — Phase 1 read-only Name + Position + Material flag + Bounds; Phase 2 makes position / orientation / scale / stretch editable.
- `Light`: `LightIntrospection` — Phase 1 read-only Name + Position + Direction + Cone half-angle + Exitance + Photons; Phase 2 makes position / energy / color editable.

**Rasterizer registry**: `Job` owns a `std::map<std::string, IRasterizer*>` keyed by chunk-name (`"bdpt_pel_rasterizer"`, `"vcm_pel_rasterizer"`, etc.).  Each `Set*Rasterizer` registers under its type name and activates.  `pRasterizer` is a non-owning borrow into the map.  Phase 1 lists only registered (parser-declared) types; Phase 2 lazy-instantiates any of the 8 standard types (PT/BDPT/VCM/MLT × Pel/Spectral) on first selection with sensible defaults.  Switching among instantiated rasterizers serializes through the same cancel-and-park machinery as `SetActiveCamera`.

**Scene epoch**: `mSceneEpoch` is a process-globally-unique counter (initialized from a static atomic that increments per controller construction).  Platform UIs cache `(epoch, category) → entity-name list` and re-pull when the epoch advances — covers scene reload (controller destroy/recreate) without per-platform plumbing.

**C-API additions** in [RISE_API.h](../src/Library/RISE_API.h):
- `int RISE_API_SceneEditController_PanelMode(p)` — returns 0..4 (or -1 on null).
- `unsigned int RISE_API_SceneEditController_CategoryEntityCount(p, cat)` and `_CategoryEntityName(p, cat, idx, buf, bufLen)`.
- `int RISE_API_SceneEditController_GetSelectionCategory(p)` and `_GetSelectionName(p, buf, bufLen)`.
- `bool RISE_API_SceneEditController_SetSelection(p, cat, name)`.
- `unsigned int RISE_API_SceneEditController_SceneEpoch(p)`.

**IJob extensions** (default-implemented to preserve ABI for out-of-tree implementers): `SetActiveRasterizer`, `GetActiveRasterizerName`, `GetRasterizerTypeCount`, `GetRasterizerTypeName`.

### 19.16 Pointer-event coordinate normalization (added late in iteration)

**Trigger**: user observation, *"hmmm something is still off, are you sure the click co-ordinates are normalized to within in the rendered image area?"* — picking was hitting wrong objects because click coordinates were in the platform widget's backing-pixel space, not the camera's image-pixel space. With aspect-fit drawing and HiDPI, those two coordinate systems can differ by both an offset (letterbox / pillarbox margins) and a scale (HiDPI multiplier × image-vs-widget aspect).

**Fix**: each platform's pointer-event handler now maps view-local click coordinates to image-pixel space before handing them to the controller. Two adjustments per platform:

1. **Aspect-fit offset** — subtract the draw-rect origin so widget-relative coords become image-relative.
2. **Pixel-density rescale** — divide by the draw-rect's size in widget points and multiply by the image's size in rasterizer pixels. The ratio handles HiDPI implicitly because the draw rect is in points and the image dims are in pixels.

Coordinates may fall outside `[0, image.size]` if the user drags past the image edge — the controller treats that as a miss for picking and as a normal delta for orbit / pan / zoom (deltas remain pixel-accurate even past the edge).

Per-platform implementation:

- macOS — [ViewportView.swift](../build/XCode/rise/RISE-GUI/App/ViewportView.swift): `surfacePoint(from:)` returns `Optional<CGPoint>` derived from `currentImageDrawRect()` and `image.size`. Mouse-down/dragged/up handlers guard on `nil`.
- Windows — [ViewportWidget.cpp](../build/VS2022/RISE-GUI/ViewportWidget.cpp): `surfacePoint` does the same transform via `imageDrawRect()` and `m_image.width()/height()`.
- Android — [ViewportPane.kt](../android/app/src/main/java/com/risegfx/android/ui/ViewportPane.kt): new `mapToImagePixel(p, boxSize, frame)` helper called from each `detectDragGestures` callback. Tracks the Box size via `onSizeChanged`.

**Camera-projection Y-flip in picker** *(separate but related)*: user pointed out *"The camera projection matrix for most cameras results in inverted Y, this is a well known thing"*. `PickAt` flips `px.y` to `cam->GetHeight() - px.y` before calling `GenerateRay`. The rasterizer's image-pixel y-axis (top-down, row 0 at the visual top) is inverted relative to the camera's projection-space y-axis (down corresponds to +world-up after the projection). Without the flip, clicking visually low picked objects rendered visually high.

### 19.17 Tool re-sync + scene-scale-relative camera rates (added late in iteration)

**Trigger**: user reported two issues — *"If I am editing a scene and I have one of the camera controllers selected and then load a new scene, the camera toggle stays selected but under the hood its been reset to the picker tool"* and *"The camera controls need to change the rate at which they apply the mouse movements to the overall scale of the scene. So that small scenes result in smaller absolute value changes vs. larger ones."*

**Tool re-sync**: when a new scene loads, the host platform tears down the old `SceneEditController` and constructs a new one. The new controller's `mTool` defaults to `Tool::Select`; the platform UI's toolbar `@State` / `m_current` / `rememberSaveable` persists. The disconnect produced a UI that highlighted (e.g.) the Orbit button while pointer events went through the Select-tool path. Each platform now re-pushes the toolbar selection to the controller after viewport restart:

- macOS: `.task(id: ObjectIdentifier(bridge))` on the `ViewportView`'s root — fires on appear and on bridge-identity change.
- Windows: in `MainWindow::onStateChanged`'s `SceneLoaded` branch, after the new `m_viewportBridge->start()`, also call `m_viewportBridge->setTool(m_viewportToolbar->currentTool())`.
- Android: new `viewportEpoch: StateFlow<Int>` on `RenderViewModel` that increments after each `nativeViewportStart()`. `ViewportPane` observes the epoch and re-applies `selectedTool` via `LaunchedEffect(viewportEpoch)`.

**Scene-scale-relative camera rates**: `SceneEditor` now caches `SceneScale()` — the diagonal of the axis-aligned union of every object's bounding box (computed lazily on first call by enumerating `IObjectManager::EnumerateObjects` and unioning each `IObject::getBoundingBox()`, floored at 1.0). The pan and zoom rate formulas in `ApplyCameraOpForward` switched from `dist * factor` (where `dist` was the camera-to-look-at distance) to `sceneScale * factor`:

- Pan: `speed = sceneScale * 0.0015` (was `dist * 0.0015`).
- Zoom: `speed = sceneScale * 0.005` (was `dist * 0.005`).

The user benefit: a small scene gets small absolute changes per pixel; a large scene gets large ones; the rate no longer depends on where the camera is positioned, only on the scene's intrinsic size. Orbit stays angular — rotation angles are a function of pixel deltas, not scene extent, so there's no sensible "scene scale" to scale by. The `dist` local was removed (no longer needed; `Vector3Ops::Normalize` handles the zero-vector edge case for the `forward / right / trueUp` basis derivation).

### 19.18 Angle-driven camera ops + Roll tool (added late in iteration)

**Trigger**: user request — *"For the camera I want to add the following functionality: [roll, orbit (phi/theta), zoom, pan] … Make sure that all cameras also support keyframing for these parameters for animations."*

**Key insight that shaped the design**: every camera that derives from `CameraCommon` already has the parameters the user wants. They're declared in the parser descriptor, applied by `Recompute` via `CameraTransforms::AdjustCameraForThetaPhi` and `CameraTransforms::AdjustCameraForOrientation`, and keyframable through the existing `TARGET_ORIENTATION_ID` / `ORIENTATION_ID` animator IDs:

| User-facing concept | RISE parameter | What it drives |
|---|---|---|
| Roll | `orientation.z` | rotation around the forward axis (camera→look-at) |
| Orbit phi (azimuth) | `target_orientation.y` | rotation around world up |
| Orbit theta (elevation) | `target_orientation.x` | rotation around camera-right; clamped to ±π/2 by `AdjustCameraForThetaPhi` |
| Zoom | `vPosition` along forward | unchanged from before |
| Pan | `vPosition` + `vLookAt` translation | unchanged from before |

**The shift** is from "bake orbit into `vPosition`" (the previous `OrbitCamera` op rotated the camera's world position around `vLookAt`) to "increment `target_orientation`" (the new `OrbitCamera` op deltas the angle parameters; `Recompute` derives the post-orbit position). Geometrically the rendered camera ends up in the same place — rotation preserves radius — but the parameter representation is now keyframe-clean and round-trippable.

**Concrete changes**:

- [SceneEdit.h](../src/Library/SceneEditor/SceneEdit.h) — added `RollCamera` op (uses `s` for the pixel delta), added `prevCameraTargetOrient` (Vector2) and `prevCameraOrient` (Vector3) to the prev-state fields. `IsCameraOp(RollCamera)` returns true; the existing `OrbitCamera` op classifier carries through.
- [SceneEditor.cpp](../src/Library/SceneEditor/SceneEditor.cpp) — `ApplyCameraOpForward`: `OrbitCamera` now reads `cam.GetTargetOrientation()`, applies `±0.0087 rad/px` deltas, calls `cam.SetTargetOrientation()`. `RollCamera` reads `GetEulerOrientation()`, deltas `.z` by `s × 0.0087`, calls `SetEulerOrientation()`. The Apply path captures both prev fields in addition to the existing pos/lookAt/up. `RestoreCameraTransform` on Undo restores all five (no-op for fields the forward op didn't touch — keeps Undo monomorphic).
- [SceneEditController](../src/Library/SceneEditor/SceneEditController.h) — `Tool::RollCamera = 8` enum value; `OnPointerDown` / `OnPointerMove` / `OnPointerUp` handle it like the other camera-motion tools (`BeginComposite("Camera")`, motion-scale bump, `EndComposite` on release). `OnPointerMove` packs the X delta into `edit.s` (single degree of freedom — Y ignored).
- C-API: `SceneEditTool_RollCamera = 8` added to [RISE_API.h](../src/Library/RISE_API.h).
- Per-platform toolbar UIs gain a Roll button — macOS `ViewportTool.rollCamera` (SF symbol `arrow.clockwise.circle`, cursor `.resizeLeftRight`), Windows `ViewportTool::RollCamera` (Qt icon `object-rotate-right`, cursor `Qt::SizeHorCursor`), Android `ViewportTool.RollCamera`.
- Properties-panel exposure — already there. `CameraIntrospection::Inspect` filters out the redundant scalar shadows (pitch / yaw / roll / theta / phi) and keeps the canonical Vec3 / Vec2 forms (`orientation` / `target_orientation`); descriptions updated to mention the Roll and Orbit tools and that they delta these parameters in radians.
- Keyframing — already wired. `CameraCommon::KeyframeFromParameters` accepts both `orientation` and `target_orientation` names; `SetIntermediateValue` interpolates them on every per-frame animation tick. No new infrastructure.

**Test coverage**: new [tests/SceneEditorCameraAnglesTest.cpp](../tests/SceneEditorCameraAnglesTest.cpp) covers four cases (28 assertions): RollCamera classification, OrbitCamera mutates target_orientation only, RollCamera mutates `orientation.z` only, Pan/Zoom leave both angle parameters alone. Includes a roundtrip check that zeroing `target_orientation` after an orbit returns `GetLocation()` to its pre-orbit value — confirming the orbit is genuinely stored in angles, not baked into `vPosition`.

### 19.11 Adversarial review summary

Four rounds of parallel adversarial reviews ran across Phase 1-3 work, finding 13 P1 bugs total. All fixed before Phase 4 began. Notable findings:

- **Round 1**: condvar lost-wakeup in `KickRender`/`Stop`; `mInner` raw pointer in `CancellableProgressCallback`; Windows queued-lambda UAF (fixed with `QPointer`); caster leak on partial construction; zoom sign inverted; `SetSceneTime` undo no-ops; `SetProperty` bypassed undo.
- **Round 2**: composite-undo branch missing `SetSceneTime` and `SetCameraProperty` cases.
- **Round 3**: `EditHistory::TrimToMax` could split composites; `EndComposite` at depth 0 pushed an orphan marker.
- **Round 4**: composite undo scope was hardcoded; replaced with max-significant-op tracking via `sawObjectOp` / `sawCameraOp` / `sawTimeOp` / `sawPropertyOp` flags.

### 19.12 Open work — Phase 6 (round-trip save)

Not started. Plan unchanged: in-place rewrite of `position` / `orientation` / `scale` / `stretch` lines on dirty objects, preserving comments and surrounding whitespace, via a `Span` index of `object { … }` chunks built during scene load.

### 19.13 Deferred / known issues

- **Edit-while-rendering scene-state race** (§17). Unchanged from original plan. TSAN flags it; symptom is rare transient firefly pixels on fast drags; cancel-restart cleans up on the next render. Proper fix (RW lock or double-buffered camera) deferred.
- **Per-property `RegenerateData` inside composite undo** (§17). Unchanged. Wasteful but rare.
- **Time-scrub idle refinement** — the refinement loop currently keys on `mPointerDown`, which is not set during `OnTimeScrub`. Pausing on a frame mid-scrub does not currently trigger refinement. Fix would be either a `mInteracting` rename or a separate `mScrubbing` atomic; deferred (low priority).
