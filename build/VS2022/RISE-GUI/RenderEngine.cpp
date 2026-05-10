//////////////////////////////////////////////////////////////////////
//
//  RenderEngine.cpp - Core engine wrapper implementation.
//
//  Bridges RISE C++ callbacks to Qt signals for thread-safe UI updates.
//  Ported from the Mac app's RISEBridge.mm + RenderViewModel.swift.
//
//////////////////////////////////////////////////////////////////////

#include "RenderEngine.h"

#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IProgressCallback.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IScene.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/ILogPrinter.h"
#include "Utilities/RTime.h"
#include "Utilities/MediaPathLocator.h"
#include "Utilities/Reference.h"

// L4c — ViewportFrameStore-driven viewport pipeline replaces the
// legacy IJobRasterizerOutput adapter (ImageOutputAdapter).  The
// rasterizer feeds pixels into the canonical HDR FrameStore wrapped
// by ViewportFrameStore; on each tile/frame observer callback the
// engine RenderToBuffer(RGBA8_sRGB)s directly into m_pixelBuffer
// (saving the RGBA16-then->>8 hop the legacy ImageOutputAdapter
// path performed) and emits the existing imageUpdated() Qt signal.
// Adds live exposure scrubbing (setViewExposureEV) and multi-format
// SaveAs without re-render.  See docs/FRAMESTORE_DESIGN.md §11 L4c.
#include "Rendering/ViewportFrameStore.h"
#include "Rendering/FrameStore.h"
#include "Rendering/FrameEncoders.h"
#include "Rendering/TargetFormat.h"
#include "Rendering/ViewTransform.h"
#include "Interfaces/IFrameEncoder.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QCoreApplication>
#include <QImage>
#include <QPointer>

#include <atomic>
#include <cstdlib>
#include <algorithm>

using namespace RISE;
using namespace RISE::FrameStoreOutput;

// ============================================================
// C++ callback adapter: IProgressCallback
// ============================================================
class ProgressCallbackAdapter : public IProgressCallback {
public:
    RenderEngine* engine;
    std::string currentTitle;

    ProgressCallbackAdapter(RenderEngine* e) : engine(e) {}

    bool Progress(const double progress, const double total) override {
        if (engine) {
            engine->onProgress(progress, total, currentTitle);
        }
        return !engine->m_cancelFlag.load();
    }

    void SetTitle(const char* title) override {
        currentTitle = title ? title : "";
    }
};

// (Legacy ImageOutputAdapter removed by L4c — see header comment.
// VFS observer callbacks fan out to RenderEngine::onTileComplete /
// onFrameComplete; the engine renders directly into m_pixelBuffer
// at RGBA8_sRGB.)

// ============================================================
// C++ callback adapter: ILogPrinter
// ============================================================
class LogPrinterAdapter : public ILogPrinter, public Implementation::Reference {
public:
    RenderEngine* engine;

    LogPrinterAdapter(RenderEngine* e) : engine(e) {}
    virtual ~LogPrinterAdapter() {}

    void Print(const LogEvent& event) override {
        if (engine && (event.eType & (eLog_Warning | eLog_Error | eLog_Fatal | eLog_Event))) {
            engine->onLogMessage(event.eType, event.szMessage);
        }
    }

    void Flush() override {}
};

// ============================================================
// RenderEngine implementation
// ============================================================

RenderEngine::RenderEngine(QObject* parent)
    : QObject(parent)
{
    srand(GetMilliseconds());

    // Set log file to user's home directory
    QString logPath = QDir::homePath() + "/RISE_GUI_Log.txt";
    SetGlobalLogFileName(logPath.toUtf8().constData());

    // Check existing RISE_MEDIA_PATH
    const char* mediaPath = std::getenv("RISE_MEDIA_PATH");
    if (mediaPath) {
        GlobalMediaPathLocator().AddPath(mediaPath);
    }

    // Create the RISE job
    RISE_CreateJobPriv(&m_job);

    // L5d — interactive GUI: drop file_rasterizeroutput chunks at
    // parse time so loading a scene doesn't auto-write PNG/EXR/etc.
    // on every Render click.  Users save via File > Save Rendered
    // Image (writes to a user-picked path through the L2
    // IFrameEncoder pipeline).
    if (m_job) {
        m_job->SetSuppressFileRasterizerOutputs(true);
    }

    // Install log printer
    auto* logPrinter = new LogPrinterAdapter(this);
    logPrinter->addref();
    GlobalLogPriv()->AddPrinter(logPrinter);
    // LogPrinter is now owned by the global log; we don't track it separately

    // Elapsed time timer. Drives both the elapsed counter and the ETA
    // read-out; the ETA estimator is updated from the progress callback
    // (worker thread) and sampled here (UI thread) behind m_etaMutex.
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(500);
    connect(m_elapsedTimer, &QTimer::timeout, this, [this]() {
        emit elapsedTimeUpdated(m_renderClock.elapsed() / 1000.0);

        double remaining = 0.0;
        bool hasEstimate = false;
        {
            std::lock_guard<std::mutex> lock(m_etaMutex);
            hasEstimate = m_eta.RemainingSeconds(remaining);
        }
        emit remainingTimeUpdated(remaining, hasEstimate);
    });
}

RenderEngine::~RenderEngine()
{
    // L4 round-4 P1-A + round-5 P1-B — synchronously wait for any
    // in-flight background thread (load, render, or animation)
    // BEFORE touching m_job or m_productionVFS.  Without this,
    // a dtor running while LoadAsciiScene OR Rasterize is on a
    // QThread stack would race m_job access against m_job->release()
    // and (for renders) race VFS callback lambdas (which capture
    // `this` raw) against engine destruction.  cancelRender flips
    // m_cancelFlag, which the progress callback returns to the
    // rasterizer for renders; LoadAsciiScene doesn't observe the
    // flag but is typically short enough that the dtor will block
    // briefly until it returns naturally.
    m_cancelFlag = true;
    // waitForWorkerToFinish() also drains queued events posted by
    // the prior worker (round-6 P1 + round-7 P2): without that, a
    // stale `setState(Completed)` etc. from a worker the dtor just
    // joined could fire later against a freed receiver / VFS / Job.
    // Two-layer defense: (a) every queued lambda captures a
    // QPointer<RenderEngine> guard and early-returns if it cleared,
    // and (b) `removePostedEvents(this)` inside
    // `waitForWorkerToFinish` drops all pending queued events so
    // Qt's delivery path doesn't even attempt to dispatch them.
    waitForWorkerToFinish();

    // Tear-down ordering: drop the Job (which destroys the rasterizer,
    // joining its worker pool en route) FIRST.  By the time
    // m_job->release() returns no callbacks can be in flight against
    // m_productionVFS.  Then null the VFS callbacks so any
    // late-fire (defensive — should be impossible after Job release)
    // bypasses the captured `this` lambda.  Finally release the
    // engine's VFS reference (the rasterizer's reference is already
    // gone via the rasterizer's dtor).  This ordering matters because
    // the VFS callback lambdas capture `this` raw — running them
    // after RenderEngine destruction begins would UAF.  See L4
    // round-4 P2-B adversarial review.
    if (m_job) {
        m_job->SetProgress(nullptr);
        if (auto* rasterizer = m_job->GetRasterizer()) {
            rasterizer->FreeRasterizerOutputs();
        }
        m_job->release();
        m_job = nullptr;
    }

    if (m_productionVFS) {
        // Defensive: nil the callbacks before release so any path
        // that re-enters them post-destruction (e.g. due to a
        // future bug) gracefully no-ops instead of dereferencing
        // `this`.  Per ViewportFrameStore.h these setters are NOT
        // thread-safe in general, but at this point we've drained
        // observers via Job->release()'s rasterizer dtor → outputs
        // dtor → VFS dtor → RemoveObserver cv-wait.
        m_productionVFS->SetTileCompleteCallback(nullptr);
        m_productionVFS->SetFrameCompleteCallback(nullptr);
        m_productionVFS->SetPreDenoiseCompleteCallback(nullptr);
        m_productionVFS->SetDenoiseCompleteCallback(nullptr);
        m_productionVFS->release();
        m_productionVFS = nullptr;
    }
}

void RenderEngine::waitForWorkerToFinish()
{
    // Synchronously join any in-flight background thread (load,
    // render, or animation).  Called from the dtor + scene-reload
    // paths so the new operation doesn't race the previous worker's
    // tile callbacks / completion lambda against freshly-created
    // VFS / Job state.  See L4 round-4 P1-A + round-5 P1-B reviews.
    QThread* thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_workerThreadMutex);
        thread = m_workerThread;
    }
    if (thread) {
        thread->wait();   // blocks until the lambda returns
    }

    // L4 round-7 P2 — drain any UI-thread queued events the prior
    // worker posted via Qt::QueuedConnection BEFORE returning.
    // Without this, a stale `setState(Completed)` / `setState(Error)`
    // / `m_elapsedTimer->stop()` / progressUpdated / imageUpdated /
    // logMessage from the previous render would fire DURING or
    // AFTER loadScene / clearScene / startRender, overwriting the
    // newly-set state.  Round-6's `removePostedEvents` was only in
    // the dtor; centralising the drain here means scene-transition
    // call sites get it too.  This nukes ALL pending events for
    // `this` (not just QMetaCallEvents), which is acceptable
    // because RenderEngine only receives events it posted to
    // itself (no incoming user events from other QObjects).
    QCoreApplication::removePostedEvents(this);
}

void RenderEngine::trackWorkerThread(QThread* thread)
{
    {
        std::lock_guard<std::mutex> lock(m_workerThreadMutex);
        m_workerThread = thread;
    }
    // Auto-clear m_workerThread on thread finish so the dtor's
    // wait() observes a real in-flight thread and a stale pointer
    // doesn't survive past QThread's deleteLater.  DirectConnection
    // so the slot runs on the worker thread itself (the lambda
    // body has just returned; no Qt event loop dispatching needed).
    connect(thread, &QThread::finished, this, [this, thread]() {
        std::lock_guard<std::mutex> lock(m_workerThreadMutex);
        if (m_workerThread == thread) m_workerThread = nullptr;
    }, Qt::DirectConnection);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
}

QString RenderEngine::versionString() const
{
    int major = 0, minor = 0, revision = 0, build = 0;
    bool debug = false;
    RISE_API_GetVersion(&major, &minor, &revision, &build, &debug);
    return QString("%1.%2.%3 build %4%5")
        .arg(major).arg(minor).arg(revision).arg(build)
        .arg(debug ? " (DEBUG)" : "");
}

void* RenderEngine::opaqueJobHandle() const
{
    // The ViewportBridge takes IJobPriv* via a void* to avoid leaking
    // C++ types through the Qt-level header.  Lifetime is tied to
    // this RenderEngine — the ViewportBridge must not outlive us.
    return static_cast<void*>(m_job);
}

void RenderEngine::setState(State newState)
{
    m_state = newState;
    emit stateChanged(newState);
}

void RenderEngine::setupMediaPaths(const QString& sceneFilePath)
{
    QFileInfo fi(sceneFilePath);
    QString sceneDir = fi.absolutePath();

    // Add scene directory as media path
    GlobalMediaPathLocator().AddPath(sceneDir.toUtf8().constData());

    // Walk up to find project root (directory containing global.options)
    QDir dir(sceneDir);
    while (true) {
        if (QFileInfo::exists(dir.filePath("global.options"))) {
            QString rootPath = dir.absolutePath();
            if (!rootPath.endsWith('/')) {
                rootPath += '/';
            }
            GlobalMediaPathLocator().AddPath(rootPath.toUtf8().constData());

            // Set RISE_MEDIA_PATH environment variable
#ifdef _WIN32
            _putenv_s("RISE_MEDIA_PATH", rootPath.toUtf8().constData());
#else
            setenv("RISE_MEDIA_PATH", rootPath.toUtf8().constData(), 1);
#endif
            break;
        }
        if (!dir.cdUp()) break;
    }
}

void RenderEngine::loadScene(const QString& filePath)
{
    if (!m_job) return;

    // L4 round-4 P1-A + round-5 P1-B — wait for any in-flight worker
    // (load OR render) to finish before swapping scenes.  Otherwise
    // the previous worker's m_job deref or post-completion lambda +
    // VFS tile callbacks could race the new scene's rasterizer
    // construction.
    m_cancelFlag = true;
    waitForWorkerToFinish();
    m_cancelFlag = false;

    setState(Loading);
    m_loadedFilePath = filePath;

    setupMediaPaths(filePath);

    // The previous scene's rasterizer state goes away inside
    // LoadAsciiScene.  The VFS persists across scene loads (the
    // engine's reference keeps it alive); the rasterizer's reference
    // is dropped via FreeRasterizerOutputs.  The dim-change machinery
    // inside ViewportFrameStore::EnsureChain handles the FrameStore's
    // resolution-swap on the new scene's first OutputImage.
    if (auto* rasterizer = m_job->GetRasterizer()) {
        rasterizer->FreeRasterizerOutputs();
        m_productionVFSAttachedToRasterizer = false;
    }

    // Run LoadAsciiScene on a worker thread.  L4 round-5 P1-B:
    // tracked via trackWorkerThread so engine destruction during
    // a load synchronously waits for the load to finish (otherwise
    // the lambda's m_job deref + QMetaObject::invokeMethod(this,...)
    // race the dtor's release(this) and m_job->release()).
    // L4 round-6 P1 — capture a QPointer guard so the queued
    // completion lambda becomes a no-op if the engine was destroyed
    // before Qt got around to delivering it.  See ~RenderEngine().
    QPointer<RenderEngine> guard(this);
    QThread* thread = QThread::create([this, filePath, guard]() {
        bool ok = m_job->LoadAsciiScene(filePath.toUtf8().constData());

        QMetaObject::invokeMethod(this, [guard, ok]() {
            if (!guard) return;
            if (ok) {
                guard->m_hasAnimation = guard->m_job->AreThereAnyKeyframedObjects();
                emit guard->hasAnimationChanged(guard->m_hasAnimation);
                guard->setState(SceneLoaded);
            } else {
                guard->setState(Error);
                emit guard->errorOccurred("Failed to load scene file.");
            }
        }, Qt::QueuedConnection);
    });

    trackWorkerThread(thread);
    thread->start();
}

void RenderEngine::startRender()
{
    if (!m_job) return;

    setState(Rendering);
    m_cancelFlag = false;
    m_sizeDetected = false;

    // Install progress callback
    auto* progressCb = new ProgressCallbackAdapter(this);
    m_job->SetProgress(progressCb);

    // Install ViewportFrameStore as the rasterizer's IRasterizerOutput.
    // The engine owns the initial reference; Attach() addrefs the
    // rasterizer's side.  FreeRasterizerOutputs at next startRender
    // (or on scene load) drops the rasterizer's side, leaving the
    // engine as the sole owner — so the VFS + its FrameStore + its
    // observer chain persist across renders, the same observer
    // callbacks fire, no rebinding required.  See L4 design §7.5.
    ensureProductionVFSAttachedToRasterizer();

    // Start elapsed timer
    m_renderClock.start();
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Begin();
    }
    m_elapsedTimer->start();

    // L4 round-6 P1 — QPointer guard for the queued completion
    // lambda; see ~RenderEngine() and loadScene's matching guard.
    // If we early-return because the engine is gone, the
    // ProgressCallbackAdapter still leaks (allocated above with
    // `new`) — that's acceptable: the rasterizer is on its way down
    // and the leak is bounded to one Render() invocation.
    QPointer<RenderEngine> guard(this);
    QThread* thread = QThread::create([this, progressCb, guard]() {
        bool ok = m_job->Rasterize();

        QMetaObject::invokeMethod(this, [guard, ok, progressCb]() {
            if (!guard) { delete progressCb; return; }
            guard->m_elapsedTimer->stop();
            guard->m_job->SetProgress(nullptr);
            delete progressCb;

            if (guard->m_cancelFlag) {
                guard->setState(Cancelled);
            } else if (ok) {
                guard->setState(Completed);
            } else {
                guard->setState(Error);
                emit guard->errorOccurred("Rasterization failed.");
            }
        }, Qt::QueuedConnection);
    });

    trackWorkerThread(thread);
    thread->start();
}

void RenderEngine::startAnimationRender(const QString& videoOutputPath)
{
    if (!m_job) return;

    IRasterizer* rasterizer = m_job->GetRasterizer();
    if (!rasterizer) return;

    setState(Rendering);
    m_cancelFlag = false;
    m_sizeDetected = false;

    // Clear outputs from previous renders so the rasterizer's outs
    // list doesn't accumulate stale references across animation
    // start / stop cycles.  FreeRasterizerOutputs drops the
    // rasterizer's reference to our VFS; the engine still holds a
    // reference, so the VFS survives Free → re-Attach.
    rasterizer->FreeRasterizerOutputs();
    m_productionVFSAttachedToRasterizer = false;

    // Install progress callback
    auto* progressCb = new ProgressCallbackAdapter(this);
    m_job->SetProgress(progressCb);

    // (Re-)attach VFS to the rasterizer for this render pass.
    ensureProductionVFSAttachedToRasterizer();

    // TODO: Create and attach VideoEncoder for H.264 output
    // VideoEncoder* videoEncoder = new VideoEncoder(videoOutputPath.toStdString());
    // rasterizer->AddRasterizerOutput(videoEncoder);
    // videoEncoder->release(); // rasterizer now owns it

    // Start elapsed timer
    m_renderClock.start();
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Begin();
    }
    m_elapsedTimer->start();

    // L4 round-6 P1 — QPointer guard for the queued completion lambda.
    QPointer<RenderEngine> guard(this);
    QThread* thread = QThread::create([this, progressCb, rasterizer, guard]() {
        bool ok = m_job->RasterizeAnimationUsingOptions();

        // TODO: Finalize video encoder here
        // if (videoEncoder) videoEncoder->finalize();

        // Free rasterizer outputs at end-of-animation.  This drops
        // the rasterizer's reference to the engine's VFS; the engine
        // keeps its own reference, so the VFS (and its FrameStore
        // contents) persist for post-render Save-As / exposure scrub.
        rasterizer->FreeRasterizerOutputs();

        QMetaObject::invokeMethod(this, [guard, ok, progressCb]() {
            if (!guard) { delete progressCb; return; }
            guard->m_elapsedTimer->stop();
            guard->m_job->SetProgress(nullptr);
            delete progressCb;
            guard->m_productionVFSAttachedToRasterizer = false;

            if (guard->m_cancelFlag) {
                guard->setState(Cancelled);
            } else if (ok) {
                guard->setState(Completed);
            } else {
                guard->setState(Error);
                emit guard->errorOccurred("Animation rasterization failed.");
            }
        }, Qt::QueuedConnection);
    });

    trackWorkerThread(thread);
    thread->start();
}

void RenderEngine::cancelRender()
{
    if (m_state == Rendering) {
        setState(Cancelling);
        m_cancelFlag = true;
    }
}

void RenderEngine::setSceneTime(double t)
{
    if (!m_job) return;
    IScenePriv* scene = m_job->GetScene();
    if (!scene) return;
    // Full SetSceneTime: advances the animator AND regenerates every
    // populated photon map at time `t`.  The interactive viewport's
    // scrub path calls SetSceneTimeForPreview (animator-only, no
    // photon regen) for responsiveness; this method runs the
    // expensive photon regen so the next production render gets
    // caustics consistent with the scrubbed scene state.
    scene->SetSceneTime(static_cast<Scalar>(t));
}

void RenderEngine::clearScene()
{
    // L4 round-4 P1-A + round-5 P1-B — wait for any in-flight
    // worker (load OR render) before wiping scene state.
    m_cancelFlag = true;
    waitForWorkerToFinish();
    m_cancelFlag = false;

    if (m_job) {
        // ClearAll wipes the rasterizer; we drop the rasterizer's
        // VFS reference first so the rasterizer's dtor doesn't
        // touch the engine's VFS after we've moved on.  The engine
        // keeps its own VFS reference — same instance is reused on
        // the next loadScene + startRender (the lazy chain alloc
        // inside VFS handles the dim change if any).
        if (auto* rasterizer = m_job->GetRasterizer()) {
            rasterizer->FreeRasterizerOutputs();
            m_productionVFSAttachedToRasterizer = false;
        }
        m_job->ClearAll();
    }

    m_loadedFilePath.clear();
    m_hasAnimation = false;
    m_pixelBuffer.clear();
    m_hdrPixelBuffer.clear();  // L5b — drop the binary16 cache too
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_sizeDetected = false;

    emit hasAnimationChanged(false);
    emit imageUpdated(QImage());
    // Empty HDR signal so HDRRenderWidget can clear its swap chain.
    emit hdrImageUpdated(QByteArray(), 0, 0);
    setState(Idle);
}

// ============================================================
// Callback handlers (called from worker thread)
// ============================================================

void RenderEngine::onProgress(double progress, double total, const std::string& title)
{
    double fraction = (total > 0) ? progress / total : 0.0;
    QString qtTitle = QString::fromUtf8(title.c_str());

    // Feed the ETA estimator on the worker thread; the UI thread reads it
    // on the next elapsed-timer tick.
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Update(progress, total);
    }

    // L4 round-6 P1 — QPointer guard.  This site fires from the
    // rasterizer worker pool and posts to the UI thread; if the
    // engine is destroyed between the post and the delivery, the
    // guard auto-clears and the lambda no-ops.
    QPointer<RenderEngine> guard(this);
    QMetaObject::invokeMethod(this, [guard, fraction, qtTitle]() {
        if (!guard) return;
        emit guard->progressUpdated(fraction, qtTitle);
    }, Qt::QueuedConnection);
}

// L4c — VFS observer fan-in.
//
// Replaces the legacy onImageOutput (which converted RGBA16 → RGBA8
// pixel-by-pixel via `>> 8`).  The VFS's per-tile shared_mutex (L1)
// makes RenderToBuffer thread-safe alongside concurrent rasterizer
// writes; we render directly into m_pixelBuffer at RGBA8_sRGB
// (saving the RGBA16 hop entirely).  Multiple worker threads can
// land tile callbacks concurrently — m_bufferMutex serialises them
// so the QImage emission isn't torn.
// Render `roi` (full image when roi == nullptr) into m_pixelBuffer +
// emit imageUpdated.  Caller must hold m_bufferMutex.
//
// L4 round-7 P1 perf fix: per-tile invocations now write only the
// changed region into m_pixelBuffer (was: full-image RenderToBuffer
// every tile fire, ~4× regression vs legacy `>> 8`-of-tile path).
// The QImage emission is still full-image because Qt's Compose
// painter wants a complete pixmap, but the per-tile rasterizer→
// buffer cost scales with tile area instead of frame area.  Frame-
// complete fires once per frame (not hot) and uses the full-image
// path to guarantee post-denoise / post-resolve coherence.
//
// RGBA8_sRGB direct: gamma + sRGB primaries + uint8 quantize, no
// intermediate uint16 buffer.  Slight ±1-LSB difference in some
// pixels vs the legacy RGBA16-then->>8 path because VFS's `Q8`
// (FrameStore.cpp:629-634) rounds-to-nearest while legacy
// `Integerize<sRGBPel,unsigned short>` (Color_Template.h:118-122)
// truncates.  Visible impact: sub-quantum on most displays.
void RenderEngine::renderViewportToBufferAndEmit_locked(unsigned int W, unsigned int H,
                                                        const RISE::Rect* halfOpenRoi)
{
    if (W == 0 || H == 0 || !m_productionVFS) return;

    // L5b — snapshot the HDR-mode flag ONCE so the encode (the
    // RenderToBuffer call below) and the dispatch (the QueuedConnection
    // emit at the end) agree even if a UI-thread setHDREnabled toggle
    // lands between them.  Same round-2 P2-A pattern as the macOS
    // bridge; without snapshot-once a mode flip mid-emit could deliver
    // half-floats through `imageUpdated` (interpreted as RGBA8) or
    // uint8 sRGB through `hdrImageUpdated` (interpreted as binary16).
    const bool useHDR = m_hdrEnabled.load();
    const float ev    = static_cast<float>(m_viewExposureEV.load());

    // Buffer + size bookkeeping.  Both buffers track image dims; the
    // SDR buffer is 4 bytes/pixel (RGBA8), the HDR buffer is 8
    // bytes/pixel (4 binary16 RGBA).  Resizing on dim change is
    // independent — keeping the inactive buffer's last allocation
    // around so a HDR-toggle round-trip doesn't reallocate.
    const size_t needSDR = static_cast<size_t>(W) * H * 4;
    const size_t needHDR = static_cast<size_t>(W) * H * 4;  // uint16 count, NOT bytes
    if (m_imageWidth != static_cast<int>(W)
        || m_imageHeight != static_cast<int>(H)) {
        m_pixelBuffer.assign(needSDR, 0);
        m_hdrPixelBuffer.assign(needHDR, 0);
        m_imageWidth  = static_cast<int>(W);
        m_imageHeight = static_cast<int>(H);
    } else {
        // Dim unchanged but a mode flip may have left the
        // currently-active buffer un-allocated (toggle happened
        // before the first render at this dim in this mode).
        if (useHDR && m_hdrPixelBuffer.size() != needHDR) {
            m_hdrPixelBuffer.assign(needHDR, 0);
        } else if (!useHDR && m_pixelBuffer.size() != needSDR) {
            m_pixelBuffer.assign(needSDR, 0);
        }
    }

    if (useHDR) {
        // HDR path: extended-linear-sRGB binary16 (no tone curve, no
        // gamma; values may exceed 1.0).  Same TargetFormat the macOS
        // EDR path uses — the platform-specific code is just the
        // DXGI swap chain set up in HDRRenderWidget.
        const ViewTransform xf = ViewTransform::ForHDRDisplay(ev);
        if (halfOpenRoi) {
            const unsigned int y0 = halfOpenRoi->top;
            const unsigned int x0 = halfOpenRoi->left;
            const unsigned int y1 = std::min<unsigned int>(halfOpenRoi->bottom, H);
            const unsigned int x1 = std::min<unsigned int>(halfOpenRoi->right,  W);
            if (y1 <= y0 || x1 <= x0) return;
            // Pixel stride is 4 uint16 = 8 bytes; row stride is W*8.
            uint16_t* base = m_hdrPixelBuffer.data()
                             + (static_cast<size_t>(y0) * W + x0) * 4;
            m_productionVFS->RenderToBuffer(
                base, static_cast<size_t>(W) * 4 * sizeof(uint16_t),
                *halfOpenRoi,
                TargetFormat::RGBA16F_ExtendedLinearSRGB, xf);
        } else {
            m_productionVFS->RenderToBuffer(
                m_hdrPixelBuffer.data(),
                static_cast<size_t>(W) * 4 * sizeof(uint16_t),
                RISE::Rect(0, 0, H, W),
                TargetFormat::RGBA16F_ExtendedLinearSRGB, xf);
        }

        // Snapshot pixels into a QByteArray for QueuedConnection
        // delivery.  The detached array is independent of
        // m_hdrPixelBuffer, so a subsequent worker-thread tile
        // callback can keep writing while the UI thread consumes
        // this snapshot.
        const int byteCount = static_cast<int>(needHDR * sizeof(uint16_t));
        QByteArray halfFloats(reinterpret_cast<const char*>(m_hdrPixelBuffer.data()),
                              byteCount);
        bool firstTime = !m_sizeDetected;
        QPointer<RenderEngine> guard(this);
        QMetaObject::invokeMethod(this, [guard, halfFloats, firstTime, W, H]() {
            if (!guard) return;
            emit guard->hdrImageUpdated(halfFloats,
                                        static_cast<int>(W),
                                        static_cast<int>(H));
            if (firstTime) {
                guard->m_sizeDetected = true;
                emit guard->sceneSizeDetected(static_cast<int>(W),
                                              static_cast<int>(H));
            }
        }, Qt::QueuedConnection);
        return;
    }

    // SDR path.  L5e — bake in the user-selected tone curve from
    // `m_viewToneCurve` (default ACES) instead of the previous
    // hardcoded `eDisplayTransform_None`.  Ignored on the HDR path
    // above (ForHDRDisplay is by-construction tone-curve-free).
    const DISPLAY_TRANSFORM tc =
        static_cast<DISPLAY_TRANSFORM>(m_viewToneCurve.load());
    const ViewTransform xf = ViewTransform::ForLDRDisplay(ev, tc);

    if (halfOpenRoi) {
        // Region-bounded path.  RenderToBuffer writes pixels at
        // dst[(y - y0) * dstStride + (x - x0) * bpp]
        // (FrameStore.cpp:748-750), so to land them at their actual
        // (y, x) image coordinates we point dst at the (y0, x0) pixel
        // of the full-image buffer and pass the FULL row stride.
        const unsigned int y0 = halfOpenRoi->top;
        const unsigned int x0 = halfOpenRoi->left;
        const unsigned int y1 = std::min<unsigned int>(halfOpenRoi->bottom, H);
        const unsigned int x1 = std::min<unsigned int>(halfOpenRoi->right,  W);
        if (y1 <= y0 || x1 <= x0) return;
        uint8_t* base = m_pixelBuffer.data()
                        + (static_cast<size_t>(y0) * W + x0) * 4;
        m_productionVFS->RenderToBuffer(
            base, static_cast<size_t>(W) * 4,
            *halfOpenRoi, TargetFormat::RGBA8_sRGB, xf);
    } else {
        m_productionVFS->RenderToBuffer(
            m_pixelBuffer.data(), static_cast<size_t>(W) * 4,
            RISE::Rect(0, 0, H, W), TargetFormat::RGBA8_sRGB, xf);
    }

    QImage image = buildImageFromBuffer();
    bool firstTime = !m_sizeDetected;

    // L4 round-6 P1 — QPointer guard.  This is the VFS-tile-callback
    // fan-out (rasterizer worker → UI thread); if the engine is
    // destroyed mid-render, the guard makes the queued lambda a no-op
    // instead of dereferencing a freed `this`.
    QPointer<RenderEngine> guard(this);
    QMetaObject::invokeMethod(this, [guard, image, firstTime, W, H]() {
        if (!guard) return;
        emit guard->imageUpdated(image);
        if (firstTime) {
            guard->m_sizeDetected = true;
            emit guard->sceneSizeDetected(static_cast<int>(W), static_cast<int>(H));
        }
    }, Qt::QueuedConnection);
}

void RenderEngine::onProductionVFSTileComplete(const RISE::Rect& halfOpenRoi)
{
    if (!m_productionVFS) return;
    // GetDimensions takes chainMutex_ shared internally — safe against
    // a concurrent resolution-change reallocation in the rasterizer
    // thread (see L4 round-4 P2-D adversarial review).  The earlier
    // raw GetFrameStore()->Width()/Height() pattern was racy.
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;

    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H, &halfOpenRoi);
}

void RenderEngine::onProductionVFSFrameComplete()
{
    if (!m_productionVFS) return;
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H, nullptr);  // full image
}

void RenderEngine::ensureProductionVFSAttachedToRasterizer()
{
    if (!m_job) return;
    IRasterizer* rasterizer = m_job->GetRasterizer();
    if (!rasterizer) return;

    if (!m_productionVFS) {
        m_productionVFS = new Implementation::ViewportFrameStore();

        // Lambda captures `this` raw; the engine outlives the VFS
        // (engine releases its VFS reference in ~RenderEngine, AFTER
        // joining the worker pool via m_job->release()), so by the
        // time any in-flight observer callback completes, `this` is
        // still valid.  See -RenderEngine() ordering rationale.
        m_productionVFS->SetTileCompleteCallback(
            [this](const RISE::Rect& roi, uint64_t /*gen*/) {
                this->onProductionVFSTileComplete(roi);
            });
        m_productionVFS->SetFrameCompleteCallback(
            [this](unsigned int /*frame*/, uint64_t /*gen*/) {
                this->onProductionVFSFrameComplete();
            });
        m_productionVFS->SetPreDenoiseCompleteCallback(
            [this](unsigned int /*frame*/, uint64_t /*gen*/) {
                this->onProductionVFSFrameComplete();
            });
        m_productionVFS->SetDenoiseCompleteCallback(
            [this](unsigned int /*frame*/, uint64_t /*gen*/) {
                this->onProductionVFSFrameComplete();
            });
    }

    // L6e-2b/c — `Attach` auto-binds the VFS to the rasterizer's
    // canonical FrameStore via `BindFrameStore(rasterizer->GetFrameStore())`.
    // Subsequent `Rasterizer::SetFrameStore` swaps (camera-dim change,
    // active-camera switch) re-bind via the new
    // `OnRasterizerFrameStoreChanged` notification, dispatched on
    // every attached output.  Net effect: for PT / BDPT / VCM the VFS
    // now observes the rasterizer's mFrameStore directly — no
    // VFS-internal FrameStore allocation, no FrameSink cross-store
    // copy.  MLT rasterizers opt out of the FrameStore push (per
    // L6e-1.1's `AcceptsFrameStorePush()` virtual) and stay on the
    // legacy FrameSink path until L6d-2.
    if (!m_productionVFSAttachedToRasterizer) {
        m_productionVFS->Attach(rasterizer);
        m_productionVFSAttachedToRasterizer = true;
    }
}

// Live exposure scrubbing — applies a new EV stop to the cached
// FrameStore on the next emit, with no rasterizer re-run.  Calling
// it triggers an immediate repaint via the same render-to-buffer-and-
// emit path tile callbacks use, so the slider feels live.  No-op
// until the FrameStore has been allocated (i.e. a render has produced
// at least one OutputImage).
void RenderEngine::setViewExposureEV(double ev)
{
    m_viewExposureEV.store(ev);
    if (!m_productionVFS) return;
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    // Slider scrub: re-render the full image at the new EV.
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H, nullptr);
}

// L5e — Tone-curve scrub.  Same lifecycle as setViewExposureEV:
// no rasterizer re-run, no-op until the FrameStore is allocated,
// immediate Repaint to refresh the active display widget.  Tone
// curve is irrelevant on the HDR display path (ForHDRDisplay
// always uses _None internally) but Repaint is still safe — it
// just re-emits the same HDR bytes.
void RenderEngine::setViewToneCurve(int curve)
{
    m_viewToneCurve.store(curve);
    if (!m_productionVFS) return;
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H, nullptr);
}

// L5b — flip HDR display mode.  Triggers an immediate re-emit so the
// active widget (HDRRenderWidget when on, RenderWidget when off) gets
// fresh content at the new TargetFormat / ViewTransform without
// waiting for the next render.
//
// Mac round-7 fix history (RISEBridge.mm `setHDREnabled:`): toggling
// EDR OFF must NOT clobber the SDR-side cached pixmap that BlitWhole-
// AndDispatch was keeping current — the macOS bridge fixed this by
// only Repaint-ing on EDR-ON transitions.  Qt is simpler because
// only ONE display widget is active at a time (QStackedWidget):
// toggling either direction Repaints the new active path.  The
// inactive path's cached buffer (m_pixelBuffer / m_hdrPixelBuffer)
// is preserved, so a subsequent toggle back doesn't lose state.
void RenderEngine::setHDREnabled(bool enabled)
{
    const bool prev = m_hdrEnabled.exchange(enabled);
    if (prev == enabled) return;
    if (!m_productionVFS) return;
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    // Repaint at the new mode.  Caller is on the UI thread; locking
    // m_bufferMutex serialises against any in-flight rasterizer-
    // worker tile-complete callback that's mid-emit.
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H, nullptr);
}

bool RenderEngine::saveAs(const QString& path,
                          const QString& formatName,
                          double         ev)
{
    if (!m_productionVFS) return false;
    IFrameEncoder* enc =
        Implementation::FrameEncoderRegistry::Get().ByFormatName(
            formatName.toUtf8().constData());
    if (!enc) return false;
    // L5d — format-aware EncodeOpts.  HDR archival formats (EXR /
    // .hdr / RGBEA) preserve scene-referred linear values > 1.0;
    // their encoders explicitly ignore `viewTransform` and `bpp`,
    // but pass an identity transform anyway so a future encoder
    // change can't accidentally clip a save.  LDR formats (PNG /
    // TIFF-8 / TGA / PPM) get the user's current display EV baked
    // in via `ForLDRDisplay(ev)`, matching the on-screen viewport
    // result the user is likely trying to preserve.
    EncodeOpts opts;
    opts.colorSpace = eColorSpace_sRGB;
    if (enc->SupportsHDR()) {
        opts.bpp           = 0;
        opts.viewTransform = ViewTransform::Identity();
    } else {
        // L5e — bake the user's currently-active tone curve into
        // the LDR save so the file matches the on-screen viewport.
        const DISPLAY_TRANSFORM tc =
            static_cast<DISPLAY_TRANSFORM>(m_viewToneCurve.load());
        opts.bpp           = 8;
        opts.viewTransform = ViewTransform::ForLDRDisplay(
            static_cast<float>(ev), tc);
    }
    return m_productionVFS->SaveAs(
        std::string(path.toUtf8().constData()), enc, opts);
}

void RenderEngine::onLogMessage(int level, const std::string& message)
{
    QString qtMsg = QString::fromUtf8(message.c_str());

    // L4 round-6 P1 — log lines fire from any thread (including
    // post-render shutdown); QPointer guard prevents UAF if the
    // engine is destroyed before the queued lambda is delivered.
    QPointer<RenderEngine> guard(this);
    QMetaObject::invokeMethod(this, [guard, level, qtMsg]() {
        if (!guard) return;
        emit guard->logMessage(level, qtMsg);
    }, Qt::QueuedConnection);
}

QImage RenderEngine::buildImageFromBuffer()
{
    // Buffer must be locked by caller
    if (m_pixelBuffer.empty() || m_imageWidth <= 0 || m_imageHeight <= 0) {
        return QImage();
    }

    // Create a deep copy of the pixel data for the QImage
    QImage image(m_imageWidth, m_imageHeight, QImage::Format_RGBA8888);
    for (int y = 0; y < m_imageHeight; ++y) {
        const uint8_t* srcRow = m_pixelBuffer.data() + y * m_imageWidth * 4;
        memcpy(image.scanLine(y), srcRow, m_imageWidth * 4);
    }
    return image;
}
