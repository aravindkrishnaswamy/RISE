//////////////////////////////////////////////////////////////////////
//
//  RenderEngine.cpp - Core engine wrapper implementation.
//
//  Bridges RISE C++ callbacks to Qt signals for thread-safe UI updates.
//  Ported from the Mac app's RISEBridge.mm + RenderViewModel.swift.
//
//////////////////////////////////////////////////////////////////////

#include "RenderEngine.h"

#include "VideoEncoder.h"
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
#include "SceneEditor/SceneEditController.h"
#include "SceneEditor/CancellableProgressCallback.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QMetaObject>
#include <QCoreApplication>
#include <QImage>
#include <QPointer>

#include <atomic>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>

using namespace RISE;
using namespace RISE::FrameStoreOutput;

// ============================================================
// C++ callback adapter: IProgressCallback
// ============================================================
class ProgressCallbackAdapter : public IProgressCallback {
public:
    RenderEngine* engine;
    std::string currentTitle;

    // Production pause (UI refinement item 4).  Mirrors macOS
    // RISEBridge.mm's BlockProgressCallback pause gate: when `paused`
    // is set, the render worker that calls Progress()/IsCancelled()
    // parks HERE at its next call -- PixelBasedRasterizerHelper calls
    // Progress() at high frequency (per tile/pixel-batch; see its
    // callers in PixelBasedRasterizerHelper.cpp), so parking inside
    // Progress() is enough to make a pause visibly quiet the CPU
    // without needing every call site to also poll IsCancelled().
    // `pauseMutex` serializes the polling loop so N parked workers
    // don't all spin; the fast not-paused path is a single relaxed
    // atomic load and never touches the mutex.
    //
    // KNOWN GAP vs macOS (documented, not silently papered over): when
    // a SceneEditController is attached, the Job's installed progress
    // object is the controller's CancellableProgressCallback
    // (`coordProgress`, see RunProductionRenderThroughController
    // above), which composes THIS adapter as its `inner` and forwards
    // every Progress() call to it (CancellableProgressCallback::Progress)
    // -- so the parking loop below still engages and pause AND
    // cancel-while-paused both work correctly through that path.
    // However CancellableProgressCallback::IsCancelled() does NOT
    // forward to `inner`'s IsCancelled() (it only reports its own
    // cancel flag) -- src/Library/SceneEditor/CancellableProgressCallback.cpp
    // is out of scope for this GUI-only change -- so
    // PixelBasedRasterizerHelper's periodic query-only 100ms mid-block
    // IsCancelled() flush (a check that does NOT itself call Progress())
    // will not immediately park a worker that happens to hit ONLY that
    // check during a pause window; that worker parks on its next
    // Progress() call instead, which workers call far more often in
    // practice.  Not expected to be user-visible, but it's an honest
    // architectural difference from macOS's flat BlockProgressCallback,
    // which IS the object installed directly on the Job in every case.
    std::atomic<bool>  paused{false};
    mutable std::mutex pauseMutex;

    ProgressCallbackAdapter(RenderEngine* e) : engine(e) {}

    //! Returns false when the render was cancelled while paused.
    bool PauseGateContinue() const {
        if (!paused.load(std::memory_order_acquire)) return true;   // fast path
        std::lock_guard<std::mutex> lk(pauseMutex);   // one poller; others park here
        while (paused.load(std::memory_order_acquire)) {
            if (engine->m_cancelFlag.load()) return false;   // cancelled while paused
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return !engine->m_cancelFlag.load();
    }

    bool Progress(const double progress, const double total) override {
        if (!PauseGateContinue()) return false;
        if (engine) {
            engine->onProgress(progress, total, currentTitle);
        }
        return !engine->m_cancelFlag.load();
    }

    void SetTitle(const char* title) override {
        currentTitle = title ? title : "";
    }

    //! Base default is false (cancellation normally flows via Progress's
    //! return value); this override adds ONLY the pause gate -- when
    //! not paused the behavior is byte-identical to the base.
    bool IsCancelled() const override {
        return !PauseGateContinue();
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
// Model-B F2 slice S4: production-render coordinator routing
// ============================================================
//
// Mirrors macOS RISEBridge.mm's `RunProductionRenderThroughController` --
// see that function's comment for the full rationale.  Runs `doRasterize`
// (the "actually call Job::Rasterize()/RasterizeAnimationUsingOptions()"
// closure) through `controller`'s single-slot coordinator when one is
// attached, so a production render can never overlap the interactive
// loop or an agent render inside Rasterize().  NULL-safe: falls back to
// calling `doRasterize` directly when no controller is attached (today's
// pre-S4 behaviour).
//
// Fix-round S4-1 (throw-path UAF) + Fix-round 2 (Windows cancel
// regression): the actual progress/cancel composition lives ONCE in
// SceneEditController::RunProductionRenderComposed -- see that method's
// header doc for the full rationale, the RAII throw-path fix, and the
// prior-vs-inner contract.
//
// `progressCb` (this Windows render's own ProgressCallbackAdapter,
// forwarding to RenderEngine::onProgress and returning !m_cancelFlag) is
// now passed straight through as the EXPLICIT `guiProgress` parameter --
// the thing RunProductionRenderComposed composes in as mCancelProgress's
// `inner`.  The PRIOR fix-round-2 shape of this wrapper discarded
// `progressCb` and instead re-derived "prior" by reading
// job->GetProgress() -- which matched macOS (whose BlockProgressCallback
// is installed PERSISTENTLY, so GetProgress() always returns it) but was
// wrong here: startRender/startAnimationRender deliberately skip
// installing `progressCb` on the Job when a controller is attached (see
// startRender's own comment), specifically so RunProductionRenderComposed
// owns the Job's progress slot for the render's duration. That meant
// GetProgress() read nullptr, `inner` was never `progressCb`,
// ProgressCallbackAdapter::Progress was never invoked, and the Windows
// Cancel button + progress/ETA UI went dead for every coordinator-routed
// production render -- the regression this fix-round closes.
// RunProductionRenderComposed itself still separately captures the Job's
// progress slot (via ExchangeProgress, inside its coordinator turn) as
// the value it RESTORES on exit (typically nullptr here, matching this
// file's own ClearProgressIfCurrent detach at each render's completion)
// -- see that
// method's header doc for why "restore" and "inner" are deliberately two
// different values.
static bool RunProductionRenderThroughController(
    IJobPriv* job,
    SceneEditController* controller,
    const std::string& clientLabel,
    IProgressCallback* progressCb,
    const std::function<bool()>& doRasterize)
{
    try {
        if (!job) {
            return doRasterize();
        }
        return SceneEditController::RunProductionRenderComposed(
            *job, controller, RISE::String(clientLabel.c_str()), progressCb, doRasterize);
    } catch (const std::exception& e) {
        GlobalLog()->PrintEx(eLog_Error,
            "RenderEngine: production render threw an exception: %s", e.what());
    } catch (...) {
        GlobalLog()->PrintEx(eLog_Error,
            "RenderEngine: production render threw an unknown exception.");
    }
    return false;
}

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
        // Item 4: elapsed = wall time minus time spent paused, so the
        // readout reflects actual render work and freezes visibly
        // while paused.  Mirrors macOS RenderViewModel's displayTimer
        // tick (productionPausedAccum + productionPauseBegan).
        const bool isPaused = isProductionRenderPaused();
        qint64 pausedNowMs = 0;
        if (isPaused && m_productionPauseBeganMs >= 0) {
            pausedNowMs = m_renderClock.elapsed() - m_productionPauseBeganMs;
        }
        const qint64 elapsedMs = m_renderClock.elapsed() - m_productionPausedAccumMs - pausedNowMs;
        emit elapsedTimeUpdated(std::max<qint64>(0, elapsedMs) / 1000.0);

        // Item 4: don't surface an ETA while paused -- wall time is
        // frozen from the user's perspective and the estimate isn't
        // being fed (see onProgress), so a stale number would mislead.
        // Re-appears once the estimator re-converges after resume.
        double remaining = 0.0;
        bool hasEstimate = false;
        if (!isPaused) {
            std::lock_guard<std::mutex> lock(m_etaMutex);
            hasEstimate = m_eta.RemainingSeconds(remaining);
        }
        emit remainingTimeUpdated(remaining, hasEstimate);
    });

    // L8 round 9 — progressive-update poll timer.  Drives the
    // lockless `pollProductionVFS` at 30 Hz during an active
    // render.  Started in `startRender` / `startAnimationRender`,
    // stopped at the finish path of each.  See `pollProductionVFS`
    // for the architecture rationale.
    m_progressivePollTimer = new QTimer(this);
    m_progressivePollTimer->setInterval(33);  // ~30 Hz
    connect(m_progressivePollTimer, &QTimer::timeout, this, [this]() {
        pollProductionVFS();
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

    // If the drain above just discarded an in-flight render's queued
    // completion lambda, that lambda's cleanup never runs -- recover
    // the RENDER-SIDE portion here (adapter delete + published-pointer
    // clear, Job progress-slot detach, timer stops, pause-bookkeeping
    // reset) so every waitForWorkerToFinish caller (the dtor, plus
    // loadScene/clearScene and MainWindow's scene transitions via
    // cancelAndJoinInFlightWork) gets it structurally.  NOT recovered here,
    // deliberately: m_productionVFSAttachedToRasterizer -- see the
    // reclaim body for why it CANNOT be reset uniformly; a future
    // caller of this method that doesn't Free-and-clear the VFS
    // attachment the way loadScene/clearScene do must handle that
    // flag itself.
    reclaimDiscardedRenderCompletion();
}

void RenderEngine::reclaimDiscardedRenderCompletion()
{
    // See the header doc for the full contract.  m_activeProgressCallback
    // is non-null exactly when a render's completion lambda has NOT yet
    // run (startRender/startAnimationRender publish it before spawning
    // the worker; only the completion lambda -- or this method -- clears
    // it).  Our caller has already joined the worker thread and drained
    // the posted-event queue, so a non-null pointer here means the
    // completion lambda was discarded unrun and can never fire: this
    // method is the only remaining owner of the cleanup, and there is no
    // double-delete window.
    if (ProgressCallbackAdapter* orphan = m_activeProgressCallback) {
        // Mirror the completion lambda's engine-state cleanup, minus
        // three deliberate skips:
        //   * setState -- every caller immediately establishes its own
        //     state (Loading, Idle, or destruction), so no transient
        //     Cancelled/Completed transition belongs here.
        //   * the final pollProductionVFS() flush -- the render is
        //     being abandoned for a scene swap / teardown; flushing its
        //     last tiles would just post fresh queued events AFTER the
        //     drain our caller performed (worst on the dtor path).
        //   * m_productionVFSAttachedToRasterizer -- it CANNOT be reset
        //     uniformly: after a discarded STILL render the rasterizer
        //     genuinely still holds the VFS reference (flag true is the
        //     truth; clearing it would double-Attach on the next
        //     ensureProductionVFSAttachedToRasterizer -> duplicate
        //     observer fires), while after a discarded ANIMATION render
        //     the worker already ran FreeRasterizerOutputs.  The flag
        //     stays owned by the Free-and-clear blocks in loadScene /
        //     clearScene / startAnimationRender, which every current
        //     caller reaches right after this method.
        // Timers first: without the lambda they'd tick forever after
        // the render's worker is gone.
        m_elapsedTimer->stop();
        m_progressivePollTimer->stop();
        // Detach the Job's progress slot BEFORE deleting the adapter.
        // In the no-controller case startRender installed `orphan`
        // directly, so the slot still points at it; leaving it would
        // hand the next coordinator-routed render a dangling "prior" to
        // capture-and-restore (RunProductionRenderComposed exchange-
        // captures the slot inside its coordinator turn; a stale orphan
        // is a steady-state occupant, so it would still be captured), and
        // after the delete below it would
        // be a use-after-free waiting for the next Rasterize().
        // Conditional (CAS) clear, not SetProgress(nullptr), for the
        // same reason as the completion lambdas (see startRender's):
        // an agent render claiming the coordinator right after our
        // cancelled render released it may already have installed its
        // own callback from the worker thread -- the CAS refuses
        // instead of stomping it.  In the controller-routed case the
        // slot never held `orphan` (RunProductionRenderComposed
        // restored its prior on the worker thread), so the CAS is a
        // clean refusal there too.
        if (m_job) {
            m_job->ClearProgressIfCurrent(orphan);
        }
        // Clear the published pointer before the delete so a
        // setProductionRenderPaused/isProductionRenderPaused call can't
        // observe a freed adapter (same ordering rule as the completion
        // lambda; all on the UI thread, so no race -- just discipline).
        m_activeProgressCallback = nullptr;
        delete orphan;
    }
    // Always reset the pause bookkeeping, even when no adapter was
    // orphaned: the ms accumulators are tracked independent of the
    // adapter (see setProductionRenderPaused), so a scene transition
    // must not leak a prior render's paused-time into the next one.
    // This subsumes the explicit resetProductionPauseState() calls
    // loadScene/clearScene used to make right after
    // waitForWorkerToFinish().
    resetProductionPauseState();
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

bool RenderEngine::updateHasAnimation(bool hasAnimation)
{
    if (m_hasAnimation == hasAnimation) return false;
    m_hasAnimation = hasAnimation;
    emit hasAnimationChanged(m_hasAnimation);
    return true;
}

QString RenderEngine::autoResolvedIntegrator() const
{
    if (!m_job) return QString();
    auto* r = m_job->GetRasterizer();
    if (!r || !r->IsAutoDispatcher()) return QString();
    const char* name = r->ResolvedIntegratorName();
    return (name && name[0]) ? QString::fromUtf8(name) : QString();
}

QString RenderEngine::autoResolveReason() const
{
    if (!m_job) return QString();
    auto* r = m_job->GetRasterizer();
    if (!r || !r->IsAutoDispatcher()) return QString();
    const char* reason = r->ResolveReason();
    return (reason && reason[0]) ? QString::fromUtf8(reason) : QString();
}

void* RenderEngine::opaqueJobHandle() const
{
    // The ViewportBridge takes IJobPriv* via a void* to avoid leaking
    // C++ types through the Qt-level header.  Lifetime is tied to
    // this RenderEngine — the ViewportBridge must not outlive us.
    return static_cast<void*>(m_job);
}

void RenderEngine::attachSceneEditController(void* opaqueController)
{
    // See the header doc for the full lifetime contract (ViewportBridge
    // is the sole caller; registers after creating its controller, clears
    // to nullptr before destroying it).
    // Clearing is also the lifetime barrier: cancel and join the tracked
    // production worker before publishing nullptr, so no worker can still
    // hold the old borrowed controller when ViewportBridge destroys it.
    if (!opaqueController && m_viewportController) {
        cancelAndJoinInFlightWork();
    }
    m_viewportController = static_cast<SceneEditController*>(opaqueController);
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

void RenderEngine::cancelAndJoinInFlightWork()
{
    // L4 round-4 P1-A + round-5 P1-B — wait for any in-flight worker
    // (load OR render) to finish before the caller proceeds to swap or
    // wipe scene state.  Otherwise the previous worker's m_job deref or
    // post-completion lambda + VFS tile callbacks could race whatever
    // the caller does next (rasterizer construction, ClearAll, viewport
    // bridge teardown).
    m_cancelFlag = true;
    // waitForWorkerToFinish()'s removePostedEvents() discards any QUEUED
    // completion lambda from an in-flight render this call just
    // cancelled ("Open Scene" while a render, possibly paused,
    // was still going); its trailing reclaimDiscardedRenderCompletion()
    // recovers the render-side cleanup that lambda would have done
    // (adapter delete, Job progress-slot detach, timer stops,
    // pause-bookkeeping reset) -- see that method's doc.  The one piece
    // it deliberately does NOT recover, the VFS-attachment flag, is
    // handled by loadScene/clearScene's Free-and-clear blocks.
    waitForWorkerToFinish();
    m_cancelFlag = false;
}

void RenderEngine::loadScene(const QString& filePath, bool untitled)
{
    m_regionProductionRender = false;
    if (!m_job) return;

    cancelAndJoinInFlightWork();

    setState(Loading);
    // Start-screen create path (spec §5.2): an untitled scene's
    // loadedFilePath() must read as empty so Save routes to Save-As, the
    // scene never enters recents, and the window title shows "Untitled" --
    // see this method's header doc.  The real `filePath` is still used
    // below to actually read the file.
    m_loadedFilePath = untitled ? QString() : filePath;

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
        // P5 (Model-B, Slice 6c-3a): scene load is now CST-ONLY.  LoadAsciiSceneAuto routes a native-v7 scene
        // to the canonical CST path (retains the CST Document so scene_variant switching + CST edit/save work)
        // and HARD-FAILS an un-migrated scene (returns false, with a migrator pointer) -- the legacy streaming
        // parser was retired.  A derive error on the native-v7 branch is a real, visible failure (returns
        // false), NOT masked by a legacy retry.  There is no env escape hatch anymore.  A false return here
        // surfaces as the Error state below.
        bool ok = m_job->LoadAsciiSceneAuto(filePath.toUtf8().constData());

        QMetaObject::invokeMethod(this, [guard, ok]() {
            if (!guard) return;
            if (ok) {
                guard->updateHasAnimation(guard->m_job->AreThereAnyKeyframedObjects());
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

void RenderEngine::startRender(double sceneTime)
{
    startStillRender(sceneTime, false, 0, 0, 0, 0);
}

void RenderEngine::startRegionRender(double sceneTime, unsigned int left, unsigned int top,
                                     unsigned int right, unsigned int bottom)
{
    if (!productionRasterizerHonorsRegion() || right < left || bottom < top) return;
    startStillRender(sceneTime, true, left, top, right, bottom);
}

bool RenderEngine::productionRasterizerHonorsRegion() const
{
    if (!m_job) return false;
    IRasterizer* rasterizer = m_job->GetRasterizer();
    return rasterizer && rasterizer->HonorsRegion();
}

void RenderEngine::startStillRender(double sceneTime, bool regionOnly,
                                    unsigned int left, unsigned int top,
                                    unsigned int right, unsigned int bottom)
{
    if (!m_job) return;

    m_regionProductionRender = regionOnly;
    setState(Rendering);
    m_cancelFlag = false;
    m_sizeDetected = false;
    m_lastAnimationSummary.clear();  // still render: no video outputs to report

    // Install progress callback.  Model-B F2 slice S4: when a
    // SceneEditController is attached, do NOT install directly here --
    // RunProductionRenderThroughController installs its OWN composed
    // callback (forwarding to `progressCb`) for the render's exact
    // duration, on the worker thread, immediately before calling
    // Rasterize().  Installing `progressCb` directly here as well would
    // just be clobbered by that swap an instant later; skipping it
    // keeps the "one progress hook, owned by whoever is actually
    // rendering" invariant the coordinator relies on.  With no
    // controller attached (today's pre-S4 shape), install directly as
    // before.
    auto* progressCb = new ProgressCallbackAdapter(this);
    if (!m_viewportController) {
        m_job->SetProgress(progressCb);
    }
    // Item 4: publish the fresh (unpaused) adapter so
    // setProductionRenderPaused/isProductionRenderPaused can reach it,
    // and reset this render's pause-time bookkeeping.
    m_activeProgressCallback = progressCb;
    resetProductionPauseState();

    // Initialize the persistent ViewportFrameStore on the UI thread before
    // polling starts. Its rasterizer attachment happens only inside the
    // coordinated render closure below: output-list mutation must not race
    // an agent render already iterating that list.
    // The engine owns the initial reference; Attach() addrefs the
    // rasterizer's side.  FreeRasterizerOutputs at next startRender
    // (or on scene load) drops the rasterizer's side, leaving the
    // engine as the sole owner — so the VFS + its FrameStore + its
    // observer chain persist across renders, the same observer
    // callbacks fire, no rebinding required.  See L4 design §7.5.
    ensureProductionVFSInitialized();

    // Start elapsed timer
    m_renderClock.start();
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Begin();
    }
    m_elapsedTimer->start();
    // L8 round 9 — start the lockless progressive-update poll.
    m_progressivePollTimer->start();

    // L4 round-6 P1 — QPointer guard for the queued completion
    // lambda; see ~RenderEngine() and loadScene's matching guard.
    // Adapter ownership on the abnormal exits: if the queued lambda
    // is DISCARDED by waitForWorkerToFinish's removePostedEvents
    // (scene transition / destruction while this render is in
    // flight), reclaimDiscardedRenderCompletion deletes the adapter;
    // if the lambda runs after the guard cleared (can't happen today
    // -- the dtor drains the queue -- but kept as defense in depth),
    // the `delete progressCb` in its !guard branch does.
    QPointer<RenderEngine> guard(this);
    SceneEditController* const viewportController = m_viewportController;
    QThread* thread = QThread::create([this, progressCb, guard, sceneTime, viewportController,
                                       regionOnly, left, top, right, bottom]() {
        IJobPriv* job = m_job;
        bool ok = RunProductionRenderThroughController(
            job, viewportController, "gui_render", progressCb,
            [this, job, sceneTime, regionOnly, left, top, right, bottom]() -> bool {
                ensureProductionVFSAttachedToRasterizer();
                // Full SetSceneTime may regenerate photon maps for a long
                // time. Execute it on this worker, inside the coordinator,
                // so the GUI remains responsive and no agent render can
                // traverse the scene concurrently.
                if (IScenePriv* scene = job->GetScene()) {
                    scene->SetSceneTime(static_cast<Scalar>(sceneTime));
                }
                return regionOnly
                    ? job->RasterizeRegion(left, top, right, bottom)
                    : job->Rasterize();
            });

        QMetaObject::invokeMethod(this, [guard, ok, progressCb]() {
            if (!guard) { delete progressCb; return; }
            guard->m_elapsedTimer->stop();
            // L8 round 9 — stop the progressive-update poll + run
            // one final poll to flush any generation bumps between
            // the last 30 Hz tick and the OnFrameComplete fire.
            guard->m_progressivePollTimer->stop();
            guard->pollProductionVFS();
            // Conditional (CAS) clear, NOT SetProgress(nullptr): an agent
            // render queued behind this one on the coordinator may have
            // just installed ITS callback from the coordinator worker
            // thread; an unconditional null here would stomp it (silently
            // degrading that render's progress/cancel wiring).  The CAS
            // clears only if the slot still holds OUR adapter -- true in
            // the no-controller case (startRender installed it directly),
            // a clean refusal otherwise (controller-routed renders never
            // put progressCb in the slot; RunProductionRenderComposed
            // already restored its prior on the worker thread).
            guard->m_job->ClearProgressIfCurrent(progressCb);
            // Item 4: the adapter this pointer refers to is about to be
            // deleted -- clear it FIRST so a setProductionRenderPaused/
            // isProductionRenderPaused call landing right now (still on
            // this same Qt main thread, so no race) can't dereference a
            // freed adapter.
            guard->m_activeProgressCallback = nullptr;
            delete progressCb;
            // Review P2 (mirrors macOS RenderViewModel's completion-
            // handler fix): clear pause bookkeeping on EVERY exit path
            // (normal completion, cancel, error) -- not just at the next
            // startRender/startAnimationRender.  Readers already
            // re-derive "is paused" off m_activeProgressCallback==nullptr
            // first today (see isProductionRenderPaused), which happens
            // to make m_productionPausedAccumMs/m_productionPauseBeganMs
            // inert until the next render starts -- but that's a
            // duplicated invariant, not a guarantee, so reset explicitly
            // here too.
            guard->resetProductionPauseState();

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

    m_regionProductionRender = false;
    setState(Rendering);
    m_cancelFlag = false;
    m_sizeDetected = false;
    m_lastAnimationSummary.clear();  // repopulated on completion below

    // Install progress callback.  Model-B F2 slice S4: same "skip the
    // direct install when a controller is attached" rule as startRender
    // -- see that method's comment.
    auto* progressCb = new ProgressCallbackAdapter(this);
    if (!m_viewportController) {
        m_job->SetProgress(progressCb);
    }
    // Item 4: same publish + reset as startRender -- see that method's
    // comment.
    m_activeProgressCallback = progressCb;
    resetProductionPauseState();

    // Create the persistent VFS before the progressive poll starts. All
    // rasterizer output-list changes, including encoder setup/finalize/free,
    // happen inside the coordinated closure below.
    ensureProductionVFSInitialized();

    const std::string videoBasePath = videoOutputPath.toStdString();

    // Start elapsed timer
    m_renderClock.start();
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Begin();
    }
    m_elapsedTimer->start();
    // L8 round 9 — start the lockless progressive-update poll.
    m_progressivePollTimer->start();

    // L4 round-6 P1 — QPointer guard for the queued completion lambda.
    QPointer<RenderEngine> guard(this);
    SceneEditController* const viewportController = m_viewportController;
    QThread* thread = QThread::create([this, progressCb, videoBasePath, guard, viewportController]() {
        IJobPriv* job = m_job;
        QString animationSummary;
        bool ok = RunProductionRenderThroughController(
            job, viewportController, "gui_render_animation", progressCb,
            [this, job, &animationSummary, &videoBasePath]() -> bool {
                IRasterizer* rasterizer = job->GetRasterizer();
                if (!rasterizer) return false;

                rasterizer->FreeRasterizerOutputs();
                m_productionVFSAttachedToRasterizer = false;
                ensureProductionVFSAttachedToRasterizer();

                VideoEncoder* proResEncoder =
                    new VideoEncoder(videoBasePath, VideoEncoder::Codec::ProRes4444);
                VideoEncoder* hevcEncoder =
                    new VideoEncoder(videoBasePath, VideoEncoder::Codec::HevcHdr10);
                rasterizer->AddRasterizerOutput(proResEncoder);
                rasterizer->AddRasterizerOutput(hevcEncoder);
                proResEncoder->release();
                hevcEncoder->release();

                bool result = false;
                try {
                    result = job->RasterizeAnimationUsingOptions();
                    proResEncoder->finalize();
                    hevcEncoder->finalize();
                } catch (...) {
                    // The platform worker catches at its outer boundary, but
                    // the rasterizer owns these encoder references. Finalize
                    // and release them before propagating to that boundary.
                    try { proResEncoder->finalize(); } catch (...) {}
                    try { hevcEncoder->finalize(); } catch (...) {}
                    rasterizer->FreeRasterizerOutputs();
                    m_productionVFSAttachedToRasterizer = false;
                    throw;
                }

                QStringList writtenParts;
                if (proResEncoder->wroteOutput()) {
                    writtenParts << QStringLiteral("%1 (ProRes 4444)").arg(
                        QFileInfo(QString::fromStdString(proResEncoder->outputPath())).fileName());
                }
                if (hevcEncoder->wroteOutput()) {
                    writtenParts << QStringLiteral("%1 (HEVC HDR10)").arg(
                        QFileInfo(QString::fromStdString(hevcEncoder->outputPath())).fileName());
                }
                if (!writtenParts.isEmpty()) {
                    animationSummary =
                        QStringLiteral("Wrote ") + writtenParts.join(QStringLiteral(" + "));
                    if (!hevcEncoder->wroteOutput()) {
                        animationSummary +=
                            QStringLiteral(" (HEVC .mp4 not written - see log)");
                    }
                }

                rasterizer->FreeRasterizerOutputs();
                m_productionVFSAttachedToRasterizer = false;
                return result;
            }
        );

        QMetaObject::invokeMethod(this, [guard, ok, progressCb, animationSummary]() {
            if (!guard) { delete progressCb; return; }
            guard->m_elapsedTimer->stop();
            // L8 round 9 — stop the progressive-update poll + run
            // one final poll to flush any generation bumps between
            // the last 30 Hz tick and the OnFrameComplete fire.
            guard->m_progressivePollTimer->stop();
            guard->pollProductionVFS();
            // CAS clear -- see startRender's completion lambda for why
            // this must not be an unconditional SetProgress(nullptr).
            guard->m_job->ClearProgressIfCurrent(progressCb);
            // Item 4: see startRender's completion lambda for why this
            // clear happens BEFORE the delete.
            guard->m_activeProgressCallback = nullptr;
            delete progressCb;
            guard->m_productionVFSAttachedToRasterizer = false;
            // Set before setState(Completed) — onStateChanged reads it
            // synchronously when the Completed transition fires.
            guard->m_lastAnimationSummary = animationSummary;
            // Review P2: see startRender's completion lambda for why this
            // reset belongs here too, not just at the next start/cancel.
            guard->resetProductionPauseState();

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
        m_cancelFlag = true;
        // Item 4: cancel-while-paused -- unpark the workers so the
        // cancel lands promptly (the pause gate also self-detects
        // cancellation via m_cancelFlag on its next 100ms poll tick,
        // but resuming here is snappier and keeps the pause UI state
        // honest during the cancelling window).  Mirrors macOS
        // RenderViewModel.cancelRender.
        if (isProductionRenderPaused()) {
            setProductionRenderPaused(false);
        }
        setState(Cancelling);
    }
}

// ---- Production render pause (UI refinement item 4) -----------------

void RenderEngine::setProductionRenderPaused(bool paused)
{
    if (m_activeProgressCallback) {
        m_activeProgressCallback->paused.store(paused, std::memory_order_release);
    }
    // Elapsed-time pause accounting -- see the header doc for
    // m_productionPausedAccumMs/m_productionPauseBeganMs.  Tracked
    // independent of whether an adapter happens to be attached so the
    // readout stays honest even in edge cases.
    if (paused) {
        m_productionPauseBeganMs = m_renderClock.isValid() ? m_renderClock.elapsed() : 0;
    } else if (m_productionPauseBeganMs >= 0) {
        const qint64 now = m_renderClock.isValid() ? m_renderClock.elapsed() : m_productionPauseBeganMs;
        m_productionPausedAccumMs += (now - m_productionPauseBeganMs);
        m_productionPauseBeganMs = -1;
    }
}

bool RenderEngine::isProductionRenderPaused() const
{
    return m_activeProgressCallback
        && m_activeProgressCallback->paused.load(std::memory_order_acquire);
}

void RenderEngine::resetProductionPauseState()
{
    m_productionPausedAccumMs = 0;
    m_productionPauseBeganMs = -1;
    if (m_activeProgressCallback) {
        m_activeProgressCallback->paused.store(false, std::memory_order_release);
    }
}

void RenderEngine::clearScene()
{
    m_regionProductionRender = false;
    // Cancel + join any in-flight worker (load OR render) before wiping
    // scene state -- see cancelAndJoinInFlightWork's doc for the
    // discarded-completion reclaim this also performs.
    cancelAndJoinInFlightWork();

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
    updateHasAnimation(false);
    m_pixelBuffer.clear();
    m_hdrPixelBuffer.clear();  // L5b — drop the binary16 cache too
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_sizeDetected = false;

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

    // Item 4: the ETA estimator is NOT fed while paused -- structurally
    // guaranteed rather than an extra flag check here: this method is
    // only ever called from ProgressCallbackAdapter::Progress() AFTER
    // its PauseGateContinue() pause loop has returned (i.e. not
    // currently paused at that instant), so onProgress simply never
    // runs during a paused span.  (macOS's RenderViewModel adds a
    // second, redundant `!pausedRef.value` guard around its own
    // etaUpdateProgress call for extra safety against a race between
    // its Swift-side pause flag and the C++-side one; Windows has only
    // one pause flag -- the adapter's own `paused` -- so there is no
    // analogous second flag to race against.)
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
// (saving the RGBA16 hop entirely).  Frame-complete callbacks and
// main-thread polling share m_bufferMutex so staging and emission
// cannot tear.
// Render the full frame into m_pixelBuffer and emit imageUpdated.
// Caller must hold m_bufferMutex.  Progressive updates are coalesced
// by the Qt-main-thread poller; do not emit a full-resolution QImage
// from every rasterizer tile callback.
//
// RGBA8_sRGB direct: gamma + sRGB primaries + uint8 quantize, no
// intermediate uint16 buffer.  Slight ±1-LSB difference in some
// pixels vs the legacy RGBA16-then->>8 path because VFS's `Q8`
// (FrameStore.cpp:629-634) rounds-to-nearest while legacy
// `Integerize<sRGBPel,unsigned short>` (Color_Template.h:118-122)
// truncates.  Visible impact: sub-quantum on most displays.
void RenderEngine::renderViewportToBufferAndEmit_locked(unsigned int W, unsigned int H,
                                                        bool nonBlocking)
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
        m_productionVFS->RenderToBuffer(
            m_hdrPixelBuffer.data(),
            static_cast<size_t>(W) * 4 * sizeof(uint16_t),
            RISE::Rect(0, 0, H, W),
            TargetFormat::RGBA16F_ExtendedLinearSRGB, xf, nonBlocking);

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

    m_productionVFS->RenderToBuffer(
        m_pixelBuffer.data(), static_cast<size_t>(W) * 4,
        RISE::Rect(0, 0, H, W), TargetFormat::RGBA8_sRGB, xf, nonBlocking);

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

void RenderEngine::onProductionVFSFrameComplete()
{
    if (!m_productionVFS) return;
    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H);  // full image
    // L8 round 9 — sync the poll sentinel so a subsequent
    // pollProductionVFS doesn't redo the same work.
    m_lastSeenGeneration = m_productionVFS->Generation();
}

void RenderEngine::pollProductionVFS()
{
    // L8 round 9 — UI-thread polling entry point.  Driven by
    // `m_progressivePollTimer` at 30 Hz during an active render.
    // The previous design ran a synchronous `OnTileComplete`
    // callback from every rasterizer worker thread, acquiring
    // `m_bufferMutex` for ~85 µs per tile + calling
    // `renderViewportToBufferAndEmit_locked` (which takes per-tile
    // FrameStore shared_locks).  Two workers landing on the same
    // FrameStore tile produced a `m_bufferMutex ↔ tile-mutex`
    // inversion that hung the render.
    //
    // Lockless replacement: workers no longer fire the tile
    // callback at all (see `ensureProductionVFSAttachedToRasterizer`
    // wiring above — `SetTileCompleteCallback` deliberately
    // omitted).  They just bump the atomic `globalGeneration_`
    // counter in `FrameStore` on every `EndTile`.  This method
    // reads that counter on the Qt main thread and emits one full
    // image when it advances.  Workers never block on the UI side.
    if (!m_productionVFS) return;
    const uint64_t gen = m_productionVFS->Generation();
    if (gen == m_lastSeenGeneration) return;  // no new pixels

    unsigned int W = 0, H = 0;
    m_productionVFS->GetDimensions(W, H);
    if (W == 0 || H == 0) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    // L8 round 14 — `nonBlocking=true`.  See FrameStore::Render doc;
    // prevents the Qt GUI thread from beachballing when workers
    // hold a slow per-pixel block's tile exclusive.
    renderViewportToBufferAndEmit_locked(W, H, /*nonBlocking=*/true);
    m_lastSeenGeneration = gen;
}

void RenderEngine::ensureProductionVFSInitialized()
{
    if (!m_productionVFS) {
        m_productionVFS = new Implementation::ViewportFrameStore();

        // Lambda captures `this` raw; the engine outlives the VFS
        // (engine releases its VFS reference in ~RenderEngine, AFTER
        // joining the worker pool via m_job->release()), so by the
        // time any in-flight observer callback completes, `this` is
        // still valid.  See -RenderEngine() ordering rationale.
        //
        // Do not install a per-tile callback.  Such callbacks run on
        // rasterizer workers and each would deep-copy a complete QImage
        // into the queued Qt event stream.  At high resolution that can
        // retain many full frames before the UI consumes them.  The
        // 30 Hz main-thread poll below coalesces tile generations into
        // one update, while frame-complete still guarantees a final
        // coherent image.
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
}

void RenderEngine::ensureProductionVFSAttachedToRasterizer()
{
    ensureProductionVFSInitialized();
    if (!m_job || !m_productionVFS) return;
    IRasterizer* rasterizer = m_job->GetRasterizer();
    if (!rasterizer) return;

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
    renderViewportToBufferAndEmit_locked(W, H);
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
    renderViewportToBufferAndEmit_locked(W, H);
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
    // m_bufferMutex serialises against frame-complete callbacks.
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    renderViewportToBufferAndEmit_locked(W, H);
}

bool RenderEngine::saveAs(const QString& path,
                          const QString& formatName,
                          double         ev)
{
    if (!m_productionVFS) return false;
	IFrameEncoder* enc =
		Implementation::FrameEncoderRegistry::Get().AcquireByFormatName(
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
	const bool saved = m_productionVFS->SaveAs(
		std::string(path.toUtf8().constData()), enc, opts);
	enc->release();
	return saved;
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
    if (image.isNull()) {
        GlobalLog()->PrintEx(eLog_Error,
            "RenderEngine::buildImageFromBuffer: failed to allocate %dx%d RGBA image",
            m_imageWidth, m_imageHeight);
        return QImage();
    }
    for (int y = 0; y < m_imageHeight; ++y) {
        const uint8_t* srcRow = m_pixelBuffer.data() + y * m_imageWidth * 4;
        memcpy(image.scanLine(y), srcRow, m_imageWidth * 4);
    }
    return image;
}
