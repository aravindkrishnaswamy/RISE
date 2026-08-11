// RiseBridge.h — single owner of an RISE::IJobPriv* on the native side.
//
// Mirrors the Objective-C++ RISEBridge.mm on macOS. The bridge is a thin C++
// class that:
//   - owns the job,
//   - owns the framebuffer (RGBA8, resized with the active display output),
//   - owns the progress, VFS/fallback-output, and logger adapters,
//   - forwards cancellation requests to an atomic flag read by the progress
//     callback from worker threads.
//
// The JNI glue in rise_jni.cpp never touches RISE::* types — it only talks to
// this bridge. That keeps the JNI layer dead simple and means adding more
// features is just "expose a method on RiseBridge and add a JNIEXPORT".

#ifndef RISE_BRIDGE_H_
#define RISE_BRIDGE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <jni.h>

#include "Utilities/RenderETAEstimator.h"

// Forward-declare RISE types to keep this header small and the JNI bridge
// free of library internals.
namespace RISE {
    class IJobPriv;
    class IProgressCallback;
    class ILogPrinter;
    class SceneEditController;
    class IRayCaster;
    class IRasterizer;
    class IRasterizerOutput;
    struct Rect;
    namespace Implementation { class ViewportFrameStore; }
}

namespace rise_jni {

class RiseBridge {
public:
    RiseBridge();
    ~RiseBridge();

    RiseBridge(const RiseBridge&)            = delete;
    RiseBridge& operator=(const RiseBridge&) = delete;

    // One-time initialization. Writes a global.options file containing
    // force_number_of_threads, points RISE_OPTIONS_FILE at it, adds
    // projectRoot to GlobalMediaPathLocator, redirects the logger to
    // logFile, and attaches the log printer adapter.
    //
    // Must be called before any loadScene/rasterize call, ideally from
    // Application.onCreate via JNI_OnLoad or an explicit nativeInit.
    void initialize(const std::string& projectRoot,
                    const std::string& logFile,
                    int                threadCount);

    // Install the Kotlin-side RiseCallback that will receive onProgress /
    // onRegionInvalidated / onSceneReady / onLog. Held as a JNI global ref.
    // Replacing an owner waits for any blocking scene operation and stops its
    // viewport before returning the new nonzero ownership token.
    uint64_t setCallback(JNIEnv* env, jobject kotlinCallback);
    // Release the callback and viewport only when ownerToken still names the
    // installed owner. A delayed teardown from an old ViewModel is a no-op.
    void clearCallback(JNIEnv* env, uint64_t ownerToken);

    // Tear down any previous job and parse the scene. Returns false on
    // parse error.
    bool loadScene(const std::string& absPath);

    // Blocking render. MUST be called from a non-UI thread. The library's
    // own pthread worker pool dispatches tiles underneath this call. The
    // production VFS publishes coherent display snapshots and invalidation
    // callbacks to Kotlin.
    bool rasterize();

    // The active rasterizer's resolved concrete integrator ("pt"/"bdpt"/"vcm")
    // when it is the auto_rasterizer dispatcher; empty otherwise.  Valid after a
    // render (the dispatcher resolves lazily at render time).  Queried via
    // GetRasterizer()->IsAutoDispatcher() etc. -- the shared cross-UI surface.
    std::string autoResolvedIntegrator() const;
    std::string autoResolveReason() const;

    // Advance the in-memory scene to time `t` AND regenerate every
    // populated photon map.  Called by RenderViewModel before
    // nativeRasterize so post-scrub renders pick up caustics
    // consistent with the scrubbed scene state.  The interactive
    // viewport's scrub path uses SetSceneTimeForPreview (animator-only,
    // no photon regen) for responsiveness; that's why we need a
    // distinct full-fidelity entry point at production-render time.
    // Photon-heavy scenes may pause many seconds inside this call;
    // the caller should already be in a "rendering" UI state.
    void setSceneTime(double t);

    // True if the loaded scene declares any keyframed objects (so
    // the Compose UI should surface the timeline scrubber).  Mirrors
    // the macOS bridge's hasAnimatedObjects.  Queried right after
    // loadScene; doesn't require the viewport controller to be
    // running, so it works for the first-render-then-restart-viewport
    // ordering on Android.
    bool hasAnimatedObjects() const;

    // Cooperative cancel. The next IProgressCallback::Progress tick will
    // return false, the library will wind down its workers on tile
    // boundaries, and rasterize() will return false (not true).
    void requestCancel();

    // Copy the current RGBA8 framebuffer into caller-owned direct storage.
    // Dimensions, generation, and copy status are captured under one lock.
    jobject copyFramebufferSnapshot(JNIEnv* env, jobject destination) const;

    // Internal: called by the production and interactive display paths when
    // their output dimensions become known or change. Reallocates the
    // framebuffer as needed and notifies Kotlin via onSceneReady. Thread-safe.
    void ensureFramebuffer(unsigned w, unsigned h);

    // Internal: legacy interactive-fallback blit. Copies one dirty region
    // from RGBA16 into the RGBA8 framebuffer and notifies Kotlin.
    void writeDirtyRegion(const unsigned short* src16,
                          unsigned w, unsigned h,
                          unsigned top, unsigned left,
                          unsigned bottom, unsigned right);

    // Internal: called by ProgressCallbackImpl to forward progress to
    // Kotlin and to read the cancel flag.
    bool onProgressTick(double progress, double total);

    // Internal: called by LogPrinterImpl to forward a formatted log line
    // to Kotlin (which relays it to Logcat and any UI log pane).
    void onLogLine(int level, const char* message);

    // Render-time ETA. The estimator is fed from the progress callback on
    // worker threads and read from the UI thread during polling, guarded
    // by m_etaMutex.
    void etaBegin();
    int64_t etaElapsedMs() const;
    // Returns >=0 ms when an estimate is available; returns -1 while the
    // estimator is still warming up.
    int64_t etaRemainingMs() const;

    // -------------------------------------------------------------
    // Interactive viewport (descriptor-driven 3D editor).
    //
    // The viewport reuses the bridge's framebuffer and onRegionInvalidated
    // callback path: the live-preview sink writes RGBA8 into m_framebuffer
    // and fires onRegionInvalidated for the full image, identical to the
    // production path.  Compose displays whichever frame arrived most
    // recently — production or viewport-preview.
    // -------------------------------------------------------------

    // Build the live-preview rasterizer + sink, create the controller,
    // and start its render thread.  When `suppressFirstFrame` is true
    // (typical post-production-render path), the suppression flag is
    // latched on the sink BEFORE the render thread starts, closing
    // the race where a fast preview pass could blit through to the
    // sink between the controller's Start and a follow-up
    // SuppressNextFrame call from the UI layer.  On Android the sink
    // is reconstructed by every stop/start (unlike macOS / Windows
    // where it's persistent), so the suppress intent has to be
    // threaded into the start call itself.
    bool startViewport(bool suppressFirstFrame);
    void stopViewport();
    bool isViewportRunning() const { return m_viewportRunning.load(); }
    bool hasLivePreview() const    { return m_viewportRasterizer != nullptr; }

    // Shrink the loaded scene's Film so the interactive preview
    // renders at a screen-appropriate resolution rather than blindly
    // inheriting whatever the .RISEscene file declared.  Caller passes
    // the display surface dims in pixels; the long edge is also
    // capped at maxLongEdge.  Wraps IJobPriv::SetViewportFit, which
    // caches the fit params and applies the fit immediately; the cache
    // lets a subsequent D2 full re-derive (variant switch / CST edit)
    // re-apply the SAME fit so the preview stays screen-sized instead
    // of jumping to authored full-res.  Never upscales, preserves
    // aspect + pixelAR.  Call AFTER loadScene and BEFORE startViewport
    // so the override is in place when the render thread spawns.
    // Returns false on null job or invalid arguments.
    bool scaleFilmToFit(unsigned int maxSurfaceW,
                        unsigned int maxSurfaceH,
                        unsigned int maxLongEdge);

    // Drop exactly one upcoming preview frame.  Race-prone if called
    // *after* startViewport's render thread has already fired —
    // prefer the suppressFirstFrame argument on startViewport for
    // the post-production-render restart path.  Still useful for
    // late-arriving suppress intents (e.g. inside an unrelated
    // event after the viewport's been running for a while).
    void viewportSuppressNextFrame();

    void viewportSetTool(int tool);
    int  viewportCurrentTool() const;
    int  viewportCategoryForTool(int tool) const;
    int  viewportDefaultSubToolForCategory(int category) const;
    int  viewportGetLastSubToolForCategory(int category) const;

    /// Gizmo handle math — recompute the per-tool screen-space layout
    /// for the current Object selection + camera, then expose the
    /// array for the Compose overlay.  See SceneEditController for
    /// the underlying contract.
    void viewportRefreshGizmoHandles();
    unsigned int viewportGizmoHandleCount() const;

    /// Fill `out[5]` with `{kind, axis, screenX, screenY, screenRadius}`
    /// for handle `idx`.  Returns true on success.  `kind` and `axis`
    /// are stored as doubles to keep the JNI handoff a single
    /// jdoubleArray copy.
    bool viewportGizmoHandle(unsigned int idx, double out[5]) const;

    int  viewportGizmoHandleAt(double x, double y) const;
    bool viewportIsGizmoDragActive() const;
    int  viewportActiveGizmoKind() const;
    int  viewportActiveGizmoAxis() const;

    void viewportPointerDown(double x, double y);
    void viewportPointerMove(double x, double y);
    void viewportPointerUp(double x, double y);

    /// Stable full-resolution camera dimensions for pointer-event
    /// coord conversion in the Compose viewport pane.  The rendered
    /// framebuffer's size shrinks during a fast drag (preview-scale
    /// subsampling); using framebuffer dims as the conversion target
    /// makes mLastPx (captured at one scale level) and the next
    /// pointer event (in another) live in mismatched coord spaces,
    /// producing 4×–32× pan/orbit jumps when the scale state machine
    /// steps.  Returns (0, 0) when no camera is attached.
    void viewportGetCameraDimensions(unsigned int& outW, unsigned int& outH) const;
    bool viewportSetSurfaceDimensions(unsigned int width, unsigned int height);

    /// Scene's animation options for sizing the timeline scrubber.
    /// Returns false on null controller; the Compose UI treats that
    /// as "no animation" and hides the slider.
    bool viewportGetAnimationOptions(double& outTimeStart, double& outTimeEnd,
                                     unsigned int& outNumFrames) const;
    /// Fallible because a coordinated/direct render can acquire admission
    /// after Compose sampled its enabled state.  FALSE means no scrub
    /// state/time mutation occurred.
    bool viewportScrubBegin();
    bool viewportScrub(double t);
    bool viewportScrubEnd();

    /// Bracket a property-panel chevron scrub.  See
    /// SceneEditController::BeginPropertyScrub for the rationale.
    void viewportBeginPropertyScrub();
    void viewportEndPropertyScrub();
    void viewportUndo();
    void viewportRedo();

    /// Canonical scene time owned by the underlying SceneEditController.
    /// Updated by every time-scrub AND by Undo / Redo of a SetSceneTime
    /// edit; that's why RenderViewModel queries this just before
    /// nativeRasterize / nativeSetSceneTime instead of trusting its
    /// own _sceneTime StateFlow, which goes stale when undo/redo
    /// changes scene time without going through the slider.  Returns
    /// 0 when no controller is attached.
    double viewportLastSceneTime() const;

    bool viewportProductionRender();

    // L4d — live exposure scrubbing & multi-format Save-As over the
    // canonical HDR FrameStore.  setViewExposureEV adjusts the
    // ViewTransform applied at framebuffer read-back without re-rendering;
    // saveAs encodes the cached FrameStore via the L2 IFrameEncoder
    // registry (formats: "PNG", "EXR", "TIFF", "HDR", "RGBEA", "TGA",
    // "PPM", case-insensitive).  Both no-op until the first render
    // has produced output.  See docs/FRAMESTORE_DESIGN.md §11 L4d.
    void setViewExposureEV(double ev);

    // L5e — Set the LDR view tone curve.  `curve` is the
    // RISE::DISPLAY_TRANSFORM enum cast to int (0 None, 1
    // Reinhard, 2 ACES default, 3 AgX, 4 Hable).  Same lifecycle
    // as setViewExposureEV: applies at read-back time only, no
    // rasterizer re-run, immediate framebuffer refresh.  Ignored
    // when HDR display is on (HDR path is by-construction tone-
    // curve-free).
    void setViewToneCurve(int curve);

    bool saveAs(const std::string& path,
                const std::string& formatName,
                double             ev);

    // Properties panel accessors — descriptor-driven snapshot.
    void         viewportRefreshProperties();
    int          viewportPanelMode() const;       // 0=None,1=Camera,2=Rasterizer,3=Object,4=Light
    std::string  viewportPanelHeader() const;     // "Camera: …" / "Object: …" / etc.
    unsigned int viewportPropertyCount() const;
    std::string  viewportPropertyName(unsigned int idx) const;
    std::string  viewportPropertyValue(unsigned int idx) const;
    std::string  viewportPropertyDescription(unsigned int idx) const;
    int          viewportPropertyKind(unsigned int idx) const;
    bool         viewportPropertyEditable(unsigned int idx) const;
    // Quick-pick presets surfaced to the UI as a dropdown.  Returns
    // empty / 0 for parameters whose descriptor declared no presets,
    // in which case the panel falls back to a plain text edit.  The
    // multi-camera "active_camera" row leans on this so Android can
    // show a real dropdown of camera names instead of forcing the
    // user to type.
    unsigned int viewportPropertyPresetCount(unsigned int idx) const;
    std::string  viewportPropertyPresetLabel(unsigned int idx, unsigned int presetIdx) const;
    std::string  viewportPropertyPresetValue(unsigned int idx, unsigned int presetIdx) const;
    bool         viewportSetProperty(const std::string& name, const std::string& value);

    // Accordion list entries — see SceneEditController::Category for
    // the int → category mapping.
    unsigned int viewportCategoryEntityCount(int category) const;
    std::string  viewportCategoryEntityName(int category, unsigned int idx) const;
    std::string  viewportCategoryActiveName(int category) const;
    int          viewportSelectionCategory() const;
    std::string  viewportSelectionName() const;
    bool         viewportSetSelection(int category, const std::string& name);
    unsigned int viewportSceneEpoch() const;

    // Internal: invoked by the viewport preview sink after blitting
    // the final-frame pixels into m_framebuffer.  Fires onRegionInvalidated
    // covering the whole image so Compose redraws.
    void onViewportFramePainted();

private:
    enum class DisplaySource : uint8_t {
        None,
        Production,
        Interactive,
    };

    void teardownJob();
    void writeGlobalOptionsFile(const std::string& path, int threadCount);

    // Callback adapters (forward-declared here; defined in RiseCallbacks.cpp)
    std::unique_ptr<RISE::IProgressCallback>    m_progress;
    RISE::ILogPrinter*                          m_logPrinter; // reference-counted inside the library; released via safe_release in teardown

    // L4d — ViewportFrameStore replaces the legacy
    // IJobRasterizerOutput RasterizerOutputAdapter.  The bridge owns
    // one persistent VFS reference; the rasterizer's reference is
    // bumped on Attach() and dropped via FreeRasterizerOutputs()
    // between renders.  The interactive viewport uses its own
    // ViewportPreviewSink and VFS below; that sink forwards final images
    // into the interactive VFS while production remains independently
    // bound to the rasterizer's canonical FrameStore.  See
    // docs/FRAMESTORE_DESIGN.md §11 L4d.
    // L5a round-5 — TWO independent ViewportFrameStores, mirroring
    // the macOS architecture (see
    // build/XCode/rise/RISE-GUI/Bridge/RISEBridge.mm for the full
    // rationale).  Production VFS receives frame-complete callbacks and is
    // generation-polled at display cadence for progressive updates.
    // Interactive VFS receives ONLY frame-complete fires from the
    // SceneEditController-driven live-preview rasterizer (no
    // per-tile observer wiring → no DrawToggles flash, no
    // preview-scale resolution thrash interfering with production).
    // Both currently render into the same `m_framebuffer` for
    // Compose display; a future landing can split the display
    // surface to give interactive its own overlay buffer.
    RISE::Implementation::ViewportFrameStore* m_productionVFS = nullptr;
    RISE::Implementation::ViewportFrameStore* m_interactiveVFS = nullptr;
    bool                                      m_productionVFSAttachedToRasterizer = false;
    // L8 round 9 — sentinel shared by the RenderViewModel 30 Hz poll and
    // the render-thread frame-complete callback.
    std::atomic<uint64_t>                     m_lastSeenGeneration{0};
    std::atomic<double>                       m_viewExposureEV{0.0};

    // L5e — LDR view tone curve.  Default 2 = ACES; matches the
    // modern preview-standard convergent across other platforms.
    std::atomic<int>                          m_viewToneCurve{2 /* eDisplayTransform_ACES */};
    void ensureProductionVFSCreated();
    void ensureProductionVFSAttachedToRasterizer();
    void ensureInteractiveVFSCreated();
    // Android deliberately has no production tile callback. Render the
    // current full VFS image only from frame completion, display-cadence
    // generation polling, or an explicit view-transform refresh.
    void renderProductionVFS(bool nonBlocking = false);
    void onProductionVFSFrameComplete();
    // L8 round 9 — generation-gated progressive-update poll. Called from
    // RenderViewModel's 30 Hz coroutine during an active render. See
    // `pollProductionVFS` impl in
    // RiseBridge.cpp + `ViewportFrameStoreCallbacks::PollAndEmitIfDirty`
    // doc in RISEBridge.mm (the architecture spec).
public:
    void pollProductionVFS();
private:
    // L5a round-5 — interactive VFS fires frame-complete only
    // (tile callback intentionally not bound), matching the
    // SceneEditController preview cadence.
    void onInteractiveVFSFrameComplete();
    // Returns the interactive VFS for fan-out from
    // ViewportPreviewSink (the SceneEditController-driven sink).
    // Lazy-creates if not yet allocated.  Borrowed pointer.
    RISE::Implementation::ViewportFrameStore* getOrCreateInteractiveVFS();

    // Job & state
    // Blocking JNI load/render calls hold this process-wide Job lifecycle
    // lock. Callback replacement waits for the same lock, so a replacement
    // ViewModel cannot tear down or receive callbacks from the prior call.
    mutable std::mutex m_sceneLifecycleMutex;
    RISE::IJobPriv*    m_job = nullptr;
    std::atomic<bool>  m_cancel{false};
    std::atomic<DisplaySource> m_displaySource{DisplaySource::None};

    // ETA estimator, read from the UI thread and written from progress
    // callbacks on worker threads.
    mutable std::mutex            m_etaMutex;
    RISE::RenderETAEstimator      m_eta;

    // Framebuffer: RGBA8, reallocated whenever either display producer changes
    // dimensions and reused while those dimensions remain stable.
    mutable std::mutex m_fbMutex;
    uint8_t*           m_framebuffer = nullptr;
    unsigned           m_fbWidth  = 0;
    unsigned           m_fbHeight = 0;
    uint64_t           m_fbGeneration = 0;

    // JNI global ref to the Kotlin RiseCallback. Held by the bridge;
    // conditionally released by clearCallback(owner) or ~RiseBridge. Guarded by
    // m_kotlinCallbackMutex (L4 round-5 P1-A): worker threads from
    // the rasterizer pool fire callbacks (onProgressTick / onLogLine
    // / renderProductionVFS / writeDirtyRegion / ensureFramebuffer)
    // that JNI-CallVoidMethod against this jobject from arbitrary
    // threads. Callback replacement first waits on m_sceneLifecycleMutex,
    // then takes this mutex so it cannot retarget an in-flight native call.
    // Without the callback mutex,
    // DeleteGlobalRef would race CallVoidMethod and UAF.  Holding
    // the mutex across CallVoidMethod is safe because the Kotlin
    // callbacks (onProgress/onSceneReady/etc.) don't re-enter the
    // bridge; they post Compose state updates and return.
    mutable std::mutex m_kotlinCallbackMutex;
    jobject m_kotlinCallback = nullptr;
    uint64_t m_kotlinCallbackOwner = 0u;
    uint64_t m_nextKotlinCallbackOwner = 1u;

    // Snapshots of init config so global.options regeneration is possible
    // on subsequent calls (not currently wired to UI).
    std::string m_projectRoot;
    std::string m_logFile;
    std::string m_optionsFile;
    int         m_threadCount = 4;
    bool        m_initialized = false;

    // Interactive viewport state.  Created lazily by startViewport(),
    // torn down by stopViewport() and on scene reload.
    RISE::SceneEditController* m_viewportController = nullptr;
    RISE::IRayCaster*          m_viewportCaster = nullptr;        // preview caster, max-recursion 1
    RISE::IRayCaster*          m_viewportPolishCaster = nullptr;  // polish caster, max-recursion 2 (one bounce of glossy / refl / refr)
    RISE::IRasterizer*         m_viewportRasterizer = nullptr;
    RISE::IRasterizerOutput*   m_viewportSink = nullptr;
    std::atomic<bool>          m_viewportRunning{false};

    void buildViewportLivePreview();
    void releaseViewportLivePreview();
};

// Process-wide singleton accessor. The bridge is created lazily on first
// call and destroyed in nativeShutdown (if ever called) or at process exit.
RiseBridge& getBridge();

} // namespace rise_jni

#endif // RISE_BRIDGE_H_
