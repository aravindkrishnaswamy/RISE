# RISE Interactive Scene Editor Plan

**Status**: Draft for review. No code changes made yet.
**Owner**: Aravind Krishnaswamy
**Scope**: Add a true interactive 3D viewport on top of the existing Mac, Windows, and Android RISE apps. Toolbar-driven object/camera/timeline mutation, live preview rendering as the user drags, undo/redo, and round-trip save back to `.RISEscene` (transform-only in this initiative; full re-serialize deferred).

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
