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
    void onImageOutput(const unsigned short* pImageData,
                       unsigned int width, unsigned int height,
                       unsigned int rc_top, unsigned int rc_left,
                       unsigned int rc_bottom, unsigned int rc_right);
    void onLogMessage(int level, const std::string& message);

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

    // Worker thread
    QThread* m_workerThread = nullptr;

    // Forward declarations for callback adapters (defined in .cpp)
    friend class ProgressCallbackAdapter;
    friend class ImageOutputAdapter;
    friend class LogPrinterAdapter;

    // Persistent image-output adapter, lazily attached to the job's
    // rasterizer on the first render and reused for subsequent renders
    // (new render → existing dispatch in the rasterizer's output list
    // is still valid).  Without persistence, every startRender() would
    // construct a fresh adapter, register a new dispatch, and then
    // delete the adapter at render-completion time — leaving the
    // dispatch in the rasterizer with a now-dangling reference.
    // The next render iterates that stale dispatch and crashes inside
    // OutputIntermediateImage when it calls back into the freed adapter.
    // Owned by RenderEngine; freed in the destructor.  Reset to null
    // on scene reload so loadScene() can re-attach (LoadAsciiScene
    // re-creates the rasterizer along with its output list).
    ImageOutputAdapter* m_imageOutput = nullptr;
};

#endif // RENDERENGINE_H
