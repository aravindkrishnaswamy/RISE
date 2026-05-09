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
    bool hasAnimation() const { return m_hasAnimation; }
    QString versionString() const;

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
    void onVFSTileComplete(const RISE::Rect& halfOpenRoi);
    void onVFSFrameComplete();
    // halfOpenRoi == nullptr → full image; non-null → render only
    // the [y0, y1) × [x0, x1) region into m_pixelBuffer's matching
    // image-space slice.
    void renderViewportToBufferAndEmit_locked(unsigned int W, unsigned int H,
                                              const RISE::Rect* halfOpenRoi);
    void ensureViewportFrameStoreAttached();

    RISE::IJobPriv* m_job = nullptr;
    std::atomic<bool> m_cancelFlag{false};
    State m_state = Idle;
    QString m_loadedFilePath;
    bool m_hasAnimation = false;

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
    RISE::Implementation::ViewportFrameStore* m_viewportFrameStore = nullptr;
    bool                                      m_vfsAttachedToRasterizer = false;

    // Live exposure-EV (atomic for cross-thread reads from the VFS
    // callback path on rasterizer workers vs setter calls from the
    // UI thread).  Applied at RenderToBuffer time as a ForLDRDisplay
    // ViewTransform — does NOT re-trigger the rasterizer.
    std::atomic<double> m_viewExposureEV{0.0};
};

#endif // RENDERENGINE_H
