// RiseBridge.h — single owner of an RISE::IJobPriv* on the native side.
//
// Mirrors the Objective-C++ RISEBridge.mm on macOS. The bridge is a thin C++
// class that:
//   - owns the job,
//   - owns the framebuffer (RGBA8, allocated once per scene),
//   - owns the progress / rasterizer-output / logger adapters,
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
    class IJobRasterizerOutput;
    class ILogPrinter;
    class SceneEditController;
    class IRayCaster;
    class IRasterizer;
    class IRasterizerOutput;
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

    // Set the Kotlin-side RiseCallback that will receive onProgress /
    // onRegionInvalidated / onSceneReady / onLog. Held as a JNI global ref.
    // Passing nullptr detaches and releases the current callback.
    void setCallback(JNIEnv* env, jobject kotlinCallback);

    // Tear down any previous job and parse the scene. Returns false on
    // parse error.
    bool loadScene(const std::string& absPath);

    // Blocking render. MUST be called from a non-UI thread. The library's
    // own pthread worker pool dispatches tiles underneath this call. The
    // RasterizerOutputImpl callback fires from those workers and writes
    // into m_framebuffer while calling back into Kotlin.
    bool rasterize();

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

    // Expose the internal RGBA8 framebuffer as a direct ByteBuffer. Called
    // by Kotlin after onSceneReady fires so it can wrap it in an
    // AndroidBitmap and display it.
    jobject getFramebufferByteBuffer(JNIEnv* env) const;

    // Internal: called by RasterizerOutputImpl on the first tile callback
    // when scene dimensions become known. Allocates m_framebuffer if needed
    // and notifies the Kotlin callback via onSceneReady. Thread-safe.
    void ensureFramebuffer(unsigned w, unsigned h);

    // Internal: called by RasterizerOutputImpl on every tile callback to
    // copy the dirty region from RGBA16 source into the RGBA8 framebuffer
    // and notify Kotlin via onRegionInvalidated.
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
    bool isViewportRunning() const { return m_viewportRunning; }
    bool hasLivePreview() const    { return m_viewportRasterizer != nullptr; }

    // Drop exactly one upcoming preview frame.  Race-prone if called
    // *after* startViewport's render thread has already fired —
    // prefer the suppressFirstFrame argument on startViewport for
    // the post-production-render restart path.  Still useful for
    // late-arriving suppress intents (e.g. inside an unrelated
    // event after the viewport's been running for a while).
    void viewportSuppressNextFrame();

    void viewportSetTool(int tool);
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

    /// Scene's animation options for sizing the timeline scrubber.
    /// Returns false on null controller; the Compose UI treats that
    /// as "no animation" and hides the slider.
    bool viewportGetAnimationOptions(double& outTimeStart, double& outTimeEnd,
                                     unsigned int& outNumFrames) const;
    void viewportScrubBegin();
    void viewportScrub(double t);
    void viewportScrubEnd();

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
    void teardownJob();
    void writeGlobalOptionsFile(const std::string& path, int threadCount);

    // Callback adapters (forward-declared here; defined in RiseCallbacks.cpp)
    std::unique_ptr<RISE::IProgressCallback>    m_progress;
    std::unique_ptr<RISE::IJobRasterizerOutput> m_output;
    RISE::ILogPrinter*                          m_logPrinter; // reference-counted inside the library; released via safe_release in teardown

    // Job & state
    RISE::IJobPriv*    m_job = nullptr;
    std::atomic<bool>  m_cancel{false};

    // ETA estimator, read from the UI thread and written from progress
    // callbacks on worker threads.
    mutable std::mutex            m_etaMutex;
    RISE::RenderETAEstimator      m_eta;

    // Framebuffer: RGBA8, allocated once per scene, reused across tiles and
    // across renders of the same scene dimensions.
    mutable std::mutex m_fbMutex;
    uint8_t*           m_framebuffer = nullptr;
    unsigned           m_fbWidth  = 0;
    unsigned           m_fbHeight = 0;

    // JNI global ref to the Kotlin RiseCallback. Held by the bridge;
    // released in setCallback(nullptr) or ~RiseBridge.
    jobject m_kotlinCallback = nullptr;

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
    bool                       m_viewportRunning = false;

    void buildViewportLivePreview();
    void releaseViewportLivePreview();
};

// Process-wide singleton accessor. The bridge is created lazily on first
// call and destroyed in nativeShutdown (if ever called) or at process exit.
RiseBridge& getBridge();

} // namespace rise_jni

#endif // RISE_BRIDGE_H_
