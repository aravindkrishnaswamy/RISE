//////////////////////////////////////////////////////////////////////
//
//  RenderEngine.h - Core engine wrapper that owns the IJobPriv*,
//  manages the worker thread, and bridges RISE callbacks to Qt signals.
//
//  This is the combined equivalent of the Mac app's RISEBridge.mm
//  and RenderViewModel.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef RENDERENGINE_H
#define RENDERENGINE_H

#include <QObject>
#include <QByteArray>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <mutex>
#include <vector>
#include <string>

#include "Utilities/RenderETAEstimator.h"

namespace RISE {
    class IJobPriv;
    struct Rect;
    namespace Implementation { class ViewportFrameStore; }
}

class RenderEngine : public QObject
{
    Q_OBJECT

public:
    enum State {
        Idle,
        Loading,
        SceneLoaded,
        Rendering,
        Cancelling,
        Completed,
        Cancelled,
        Error
    };
    Q_ENUM(State)

    explicit RenderEngine(QObject* parent = nullptr);
    ~RenderEngine();

    State state() const { return m_state; }
    QString loadedFilePath() const { return m_loadedFilePath; }
    /// Phase 6.5: re-anchor the loaded-path after a successful
    /// Save-As.  No I/O — the file at `path` is assumed to already
    /// have been written by the save engine.  ViewportBridge calls
    /// this from saveSceneTo() when the engine reports Saved with
    /// a target distinct from the current loaded path.
    void setLoadedFilePath(const QString& path) { m_loadedFilePath = path; }
    bool hasAnimation() const { return m_hasAnimation; }
    QString versionString() const;

    /// The active rasterizer's resolved concrete integrator ("pt"/"bdpt"/"vcm")
    /// when it is the auto_rasterizer dispatcher; empty otherwise (or before the
    /// first render -- the dispatcher resolves lazily at render time).  Drives the
    /// "Auto -> X" status-bar surfacing.  Safe on the main thread after a render
    /// completes (m_job + rasterizer stay alive until the dtor).
    QString autoResolvedIntegrator() const;
    /// The one-line reason the auto-dispatcher chose its integrator (empty if the
    /// active rasterizer is not the dispatcher).
    QString autoResolveReason() const;

    /// Human-readable summary of the video file(s) the last animation render
    /// actually wrote (filename + codec per file), e.g.
    /// "Wrote scene.mov (ProRes 4444) + scene.mp4 (HEVC HDR10)".  Empty for a
    /// still-image render or before any animation render.  Captured on the
    /// worker thread before the encoders are freed; read on the main thread
    /// in the Completed-state handler (set before setState(Completed) fires).
    QString lastAnimationOutputsSummary() const { return m_lastAnimationSummary; }

    /// Opaque handle to the underlying IJobPriv* — consumed by
    /// ViewportBridge so the interactive editor and the loader bridge
    /// share the same in-memory scene.  The handle is owned by this
    /// engine; callers must not retain or release.
    void* opaqueJobHandle() const;

signals:
    void stateChanged(RenderEngine::State newState);
    void progressUpdated(double fraction, const QString& title);
    void imageUpdated(const QImage& image);
    void sceneSizeDetected(int width, int height);
    void logMessage(int level, const QString& message);
    void elapsedTimeUpdated(double seconds);
    // seconds is valid only when hasEstimate is true; during warmup the UI
    // should render a placeholder (e.g. "estimating...") instead.
    void remainingTimeUpdated(double seconds, bool hasEstimate);
    void errorOccurred(const QString& message);
    void hasAnimationChanged(bool hasAnimation);

    // L5b — HDR display path (Windows scRGB via DXGI swap chain).
    // Fired in place of `imageUpdated` when HDR mode is on.  The
    // payload is binary16 RGBA half-float in extended-linear-sRGB
    // primaries (the same `RGBA16F_ExtendedLinearSRGB` TargetFormat
    // the macOS EDR path uses).  Values can exceed 1.0 for
    // highlights past SDR clip.  Consumed by `HDRRenderWidget`'s
    // DXGI swap chain (set up with `DXGI_FORMAT_R16G16B16A16_FLOAT`
    // + `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`).
    //
    // `halfFloats` is row-major, 8 bytes per pixel (4 binary16 RGBA),
    // length == W * H * 8.  Emitted via QueuedConnection; the
    // QByteArray detaches its data so a worker-thread emit doesn't
    // race the slot's read against the next worker-thread overwrite
    // of `m_hdrPixelBuffer`.
    void hdrImageUpdated(const QByteArray& halfFloats, int width, int height);

public slots:
    void loadScene(const QString& filePath);
    void startRender();
    void startAnimationRender(const QString& videoOutputPath);
    void cancelRender();
    void clearScene();

    // L4c — live exposure scrubbing & multi-format Save-As over the
    // canonical HDR FrameStore.  setViewExposureEV adjusts the
    // ViewTransform applied at display read-back without re-rendering;
    // saveAs encodes the cached FrameStore via the L2 IFrameEncoder
    // registry to any of "PNG", "EXR", "TIFF", "HDR", "RGBEA", "TGA",
    // "PPM" (case-insensitive).  Both no-op until the first render
    // has produced output.  See docs/FRAMESTORE_DESIGN.md §11 L4c.
    void setViewExposureEV(double ev);
    bool saveAs(const QString& path, const QString& formatName, double ev);

    // L5b — toggle HDR display path on/off.  When ON, every emit
    // (tile-complete, frame-complete, exposure scrub) renders the
    // FrameStore at `RGBA16F_ExtendedLinearSRGB` + `ForHDRDisplay(ev)`
    // and fires `hdrImageUpdated` instead of `imageUpdated`.  When
    // OFF, the legacy SDR `RGBA8_sRGB` path runs and `imageUpdated`
    // fires.  The toggle re-emits the current FrameStore at the new
    // mode immediately (matching the macOS `setHDREnabled` semantics
    // — see RISEBridge.mm round-7 fix), so the display switches
    // without waiting for the next render.  Calling with the current
    // state is a no-op.  Safe to call before any render has produced
    // output (the re-emit just no-ops in that case).
    void setHDREnabled(bool enabled);
    bool hdrEnabled() const { return m_hdrEnabled.load(); }

    // L5e — LDR preview controls (parity with macOS).  Both apply
    // at display read-back time only — no rasterizer re-run; an
    // immediate Repaint refreshes the on-screen image.  `curve` is
    // a value of the `DISPLAY_TRANSFORM` enum cast to int:
    //   0 None / 1 Reinhard / 2 ACES (default) / 3 AgX / 4 Hable.
    // Tone curve is ignored when HDR is on (the HDR display path
    // is by-construction tone-curve-free; the OS compositor
    // handles the display map).
    void setViewToneCurve(int curve);
    int  viewToneCurve() const { return m_viewToneCurve.load(); }

    /// Advance the in-memory scene to time `t` AND regenerate every
    /// populated photon map.  Called by MainWindow before kicking the
    /// production rasterizer so post-scrub renders pick up caustics
    /// consistent with the scrubbed scene state.  The interactive
    /// viewport's scrub path uses SetSceneTimeForPreview (animator-only,
    /// no photon regen) for responsiveness; that's why we need a
    /// distinct full-fidelity entry point at production-render time.
    /// Photon-heavy scenes may pause many seconds inside this call;
    /// the caller should already be in a "rendering" UI state.
    void setSceneTime(double t);

private:
    void setState(State newState);
    void setupMediaPaths(const QString& sceneFilePath);
    QImage buildImageFromBuffer();

    // Called from worker thread via callback adapters
    void onProgress(double progress, double total, const std::string& title);
    void onLogMessage(int level, const std::string& message);

    // L4c — VFS observer entry points.  Called from rasterizer
    // worker threads (multiple may concurrently land tile events);
    // m_bufferMutex serialises buffer access + the QImage emission.
    // L4 round-7 P1: tile callback now receives the half-open roi
    // so we can RenderToBuffer just the changed region (was: full
    // image every tile fire — ~4× regression vs legacy).  Frame-
    // complete uses full-image (once per frame, not hot).
    void onProductionVFSTileComplete(const RISE::Rect& halfOpenRoi);
    void onProductionVFSFrameComplete();
    // L8 round 9 — lockless progressive-update poll.  Runs on the
    // Qt main thread via `m_progressivePollTimer`; reads the
    // production VFS's atomic generation counter and no-ops if no
    // workers have produced new pixels since the last call.
    // Otherwise renders the full image into the staging buffer and
    // emits a QImage to the UI.  See `RenderEngine.cpp` impl for
    // the deadlock-avoidance rationale (replaces the per-tile
    // `onProductionVFSTileComplete` callback which acquired
    // `m_bufferMutex` from every worker thread).
    void pollProductionVFS();
    // halfOpenRoi == nullptr → full image; non-null → render only
    // the [y0, y1) × [x0, x1) region into m_pixelBuffer's matching
    // image-space slice.
    //
    // L8 round 14 — `nonBlocking` opt-in for the polling path so a
    // slow worker block doesn't beachball the Qt GUI thread.  When
    // true, RenderToBuffer uses `try_lock_shared` per tile and
    // skips contended tiles; the staging buffer retains its prior
    // contents for those tiles.  See `FrameStore::Render` doc for
    // the architecture rationale.
    void renderViewportToBufferAndEmit_locked(unsigned int W, unsigned int H,
                                              const RISE::Rect* halfOpenRoi,
                                              bool nonBlocking = false);
    void ensureProductionVFSAttachedToRasterizer();

    RISE::IJobPriv* m_job = nullptr;
    std::atomic<bool> m_cancelFlag{false};
    State m_state = Idle;
    QString m_loadedFilePath;
    bool m_hasAnimation = false;

    // Set on animation completion (worker thread, before the queued
    // Completed-state transition) to a summary of the written video files;
    // read on the main thread by the status-bar handler.  Cleared at the
    // start of every render so a still render never shows stale animation
    // info.  See lastAnimationOutputsSummary().
    QString m_lastAnimationSummary;

    // Progressive image buffer
    std::vector<uint8_t> m_pixelBuffer;  // RGBA8
    std::mutex m_bufferMutex;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    bool m_sizeDetected = false;

    // Elapsed time tracking
    QTimer* m_elapsedTimer = nullptr;
    QElapsedTimer m_renderClock;

    // ETA estimator: fed from the worker thread via the progress callback,
    // sampled on the UI thread by the elapsed-timer tick. Guarded by a
    // mutex since those two threads touch it.
    RISE::RenderETAEstimator m_eta;
    std::mutex m_etaMutex;

    // L4 round-4 P1-A + round-5 P1-B — track in-flight background
    // threads (loadScene's load thread, startRender's render thread,
    // startAnimationRender's animation thread) so the dtor + scene
    // transitions can synchronously join the active one.  The
    // original `m_workerThread` ivar was declared and never assigned;
    // every background path created a local `QThread* thread =
    // QThread::create`, so `~RenderEngine()` previously couldn't
    // actually wait — engine destruction during scene load OR
    // render races `m_job` access on the worker thread against
    // `m_job->release()` in the dtor, AND races VFS callback
    // lambdas (rendering paths) against engine destruction.  Load
    // and render don't overlap in practice (the UI gates Render on
    // state==SceneLoaded), so a single slot is sufficient.  Guarded
    // by m_workerThreadMutex.
    QThread*    m_workerThread = nullptr;
    std::mutex  m_workerThreadMutex;
    void waitForWorkerToFinish();
    // Helper used by every QThread::create site to publish the
    // newly-spawned thread into m_workerThread + register the
    // auto-clear connection on QThread::finished.  Centralises the
    // four-line publish-and-track pattern so a future thread site
    // can't accidentally bypass it.
    void trackWorkerThread(QThread* thread);

    // Forward declarations for callback adapters (defined in .cpp)
    friend class ProgressCallbackAdapter;
    friend class LogPrinterAdapter;

    // L4c — ViewportFrameStore replaces the legacy ImageOutputAdapter.
    // The engine owns one persistent VFS reference for the engine's
    // lifetime; the rasterizer's reference is bumped on Attach() and
    // dropped via FreeRasterizerOutputs() between renders.  The VFS
    // (and its FrameStore + observer chain) survives scene reloads
    // and rasterizer swaps — exactly the L4 design intent (§7.5).
    RISE::Implementation::ViewportFrameStore* m_productionVFS = nullptr;
    bool                                      m_productionVFSAttachedToRasterizer = false;

    // L8 round 9 — sentinel for the lockless polling path.  Read +
    // written ONLY on the Qt main thread (the `m_progressivePollTimer`
    // tick handler).  Compares `vfs->Generation()` so a poll that
    // catches no new pixels returns immediately.  See
    // `pollProductionVFS` impl.
    uint64_t m_lastSeenGeneration = 0;
    // 30 Hz timer driving the progressive-update poll.  Started in
    // `startRender` / `startAnimationRender`, stopped in the finish
    // path of each.
    QTimer*  m_progressivePollTimer = nullptr;

    // Live exposure-EV (atomic for cross-thread reads from the VFS
    // callback path on rasterizer workers vs setter calls from the
    // UI thread).  Applied at RenderToBuffer time as a ForLDRDisplay
    // ViewTransform — does NOT re-trigger the rasterizer.
    std::atomic<double> m_viewExposureEV{0.0};

    // L5b — HDR display state.  `m_hdrEnabled` is atomic so the VFS
    // callback path (rasterizer worker thread) can read it without
    // locking; setHDREnabled (UI thread) writes it.  The HDR pixel
    // buffer is binary16 RGBA half-float (8 bytes per pixel), kept
    // alongside `m_pixelBuffer` (the SDR uint8 RGBA) so a HDR-toggle
    // round-trip doesn't need to reallocate either buffer.
    // `renderViewportToBufferAndEmit_locked` selects which buffer +
    // TargetFormat + ViewTransform to use based on a snapshot of
    // `m_hdrEnabled` taken once per emit (matching the round-2 P2-A
    // pattern from the macOS bridge — encode + dispatch must agree
    // even if a UI toggle lands between RenderToBuffer and emit).
    std::atomic<bool>     m_hdrEnabled{false};
    std::vector<uint16_t> m_hdrPixelBuffer;  // binary16 RGBA, 4 per pixel

    // L5e — LDR view tone curve.  Default 2 = ACES; matches
    // the modern preview-standard convergent in macOS / Karma /
    // Maya Arnold.  Cast to DISPLAY_TRANSFORM at consumption.
    std::atomic<int>      m_viewToneCurve{2 /* eDisplayTransform_ACES */};
};

#endif // RENDERENGINE_H
