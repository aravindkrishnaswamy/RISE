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
#include "Interfaces/IJobRasterizerOutput.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IScene.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/ILogPrinter.h"
#include "Utilities/RTime.h"
#include "Utilities/MediaPathLocator.h"
#include "Utilities/Reference.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QCoreApplication>

#include <cstdlib>
#include <algorithm>

using namespace RISE;

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

// ============================================================
// C++ callback adapter: IJobRasterizerOutput
// ============================================================
class ImageOutputAdapter : public IJobRasterizerOutput {
public:
    RenderEngine* engine;

    ImageOutputAdapter(RenderEngine* e) : engine(e) {}

    bool PremultipliedAlpha() override { return false; }
    int GetColorSpace() override { return 1; } // sRGB

    void OutputImageRGBA16(
        const unsigned short* pImageData,
        const unsigned int width,
        const unsigned int height,
        const unsigned int rc_top,
        const unsigned int rc_left,
        const unsigned int rc_bottom,
        const unsigned int rc_right) override
    {
        if (engine) {
            engine->onImageOutput(pImageData, width, height,
                                  rc_top, rc_left, rc_bottom, rc_right);
        }
    }
};

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
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
    }

    if (m_job) {
        m_job->SetProgress(nullptr);
        // Drop rasterizer outputs first so the dispatch wrapping
        // m_imageOutput is destroyed before the adapter it references.
        if (auto* rasterizer = m_job->GetRasterizer()) {
            rasterizer->FreeRasterizerOutputs();
        }
        m_job->release();
        m_job = nullptr;
    }

    delete m_imageOutput;
    m_imageOutput = nullptr;
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

    setState(Loading);
    m_loadedFilePath = filePath;

    setupMediaPaths(filePath);

    // The previous scene's rasterizer state goes away inside
    // LoadAsciiScene; the dispatch wrapping m_imageOutput goes with
    // it, so the adapter must die before that happens (and a fresh
    // one will be lazy-allocated on the next startRender).
    if (auto* rasterizer = m_job->GetRasterizer()) {
        rasterizer->FreeRasterizerOutputs();
    }
    delete m_imageOutput;
    m_imageOutput = nullptr;

    // Run LoadAsciiScene on a worker thread
    QThread* thread = QThread::create([this, filePath]() {
        bool ok = m_job->LoadAsciiScene(filePath.toUtf8().constData());

        QMetaObject::invokeMethod(this, [this, ok]() {
            if (ok) {
                m_hasAnimation = m_job->AreThereAnyKeyframedObjects();
                emit hasAnimationChanged(m_hasAnimation);
                setState(SceneLoaded);
            } else {
                setState(Error);
                emit errorOccurred("Failed to load scene file.");
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
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

    // Install image output callback once per scene.  The dispatch the
    // rasterizer wraps around this adapter holds it by reference, so
    // we MUST keep the adapter alive for as long as the dispatch is
    // in the rasterizer's output list (i.e. until the next scene load
    // or until RenderEngine is destroyed).  Re-adding on every render
    // would compound: the prior dispatch stays in the list with a
    // dangling reference once we delete the adapter.
    if (!m_imageOutput) {
        m_imageOutput = new ImageOutputAdapter(this);
        m_job->AddCallbackRasterizerOutput(m_imageOutput);
    }

    // Start elapsed timer
    m_renderClock.start();
    {
        std::lock_guard<std::mutex> lock(m_etaMutex);
        m_eta.Begin();
    }
    m_elapsedTimer->start();

    QThread* thread = QThread::create([this, progressCb]() {
        bool ok = m_job->Rasterize();

        QMetaObject::invokeMethod(this, [this, ok, progressCb]() {
            m_elapsedTimer->stop();
            m_job->SetProgress(nullptr);
            delete progressCb;

            if (m_cancelFlag) {
                setState(Cancelled);
            } else if (ok) {
                setState(Completed);
            } else {
                setState(Error);
                emit errorOccurred("Rasterization failed.");
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
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

    // Clear outputs from previous renders.  The dispatch wrapping any
    // existing m_imageOutput is in this list, so it gets destroyed
    // here — drop the bare pointer immediately so we don't reuse a
    // dangling adapter (it'd be wrapped by no live dispatch).
    rasterizer->FreeRasterizerOutputs();
    delete m_imageOutput;
    m_imageOutput = nullptr;

    // Install progress callback
    auto* progressCb = new ProgressCallbackAdapter(this);
    m_job->SetProgress(progressCb);

    // Install image output callback (recreated since we just freed it).
    m_imageOutput = new ImageOutputAdapter(this);
    m_job->AddCallbackRasterizerOutput(m_imageOutput);

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

    QThread* thread = QThread::create([this, progressCb, rasterizer]() {
        bool ok = m_job->RasterizeAnimationUsingOptions();

        // TODO: Finalize video encoder here
        // if (videoEncoder) videoEncoder->finalize();

        // Free rasterizer outputs (this destroys the dispatch wrapping
        // m_imageOutput, so the adapter must die in lockstep on the UI
        // thread before any subsequent render tries to reuse it).
        rasterizer->FreeRasterizerOutputs();

        QMetaObject::invokeMethod(this, [this, ok, progressCb]() {
            m_elapsedTimer->stop();
            m_job->SetProgress(nullptr);
            delete progressCb;
            delete m_imageOutput;
            m_imageOutput = nullptr;

            if (m_cancelFlag) {
                setState(Cancelled);
            } else if (ok) {
                setState(Completed);
            } else {
                setState(Error);
                emit errorOccurred("Animation rasterization failed.");
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
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
    if (m_job) {
        // Same lifecycle ordering as loadScene: drop the dispatch
        // before the adapter, then ClearAll wipes the rasterizer.
        if (auto* rasterizer = m_job->GetRasterizer()) {
            rasterizer->FreeRasterizerOutputs();
        }
        delete m_imageOutput;
        m_imageOutput = nullptr;
        m_job->ClearAll();
    }

    m_loadedFilePath.clear();
    m_hasAnimation = false;
    m_pixelBuffer.clear();
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_sizeDetected = false;

    emit hasAnimationChanged(false);
    emit imageUpdated(QImage());
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

    QMetaObject::invokeMethod(this, [this, fraction, qtTitle]() {
        emit progressUpdated(fraction, qtTitle);
    }, Qt::QueuedConnection);
}

void RenderEngine::onImageOutput(const unsigned short* pImageData,
                                  unsigned int width, unsigned int height,
                                  unsigned int rc_top, unsigned int rc_left,
                                  unsigned int rc_bottom, unsigned int rc_right)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    // Initialize buffer on first call
    if (m_imageWidth != (int)width || m_imageHeight != (int)height) {
        m_imageWidth = width;
        m_imageHeight = height;
        m_pixelBuffer.resize(width * height * 4, 0);
    }

    // Convert RGBA16 region to RGBA8 in the pixel buffer
    // The pImageData contains the full image in RGBA16 format
    for (unsigned int y = rc_top; y <= rc_bottom && y < height; ++y) {
        for (unsigned int x = rc_left; x <= rc_right && x < width; ++x) {
            size_t srcIdx = (y * width + x) * 4;
            size_t dstIdx = (y * width + x) * 4;

            // Convert 16-bit [0..65535] to 8-bit [0..255]
            m_pixelBuffer[dstIdx + 0] = static_cast<uint8_t>(pImageData[srcIdx + 0] >> 8);
            m_pixelBuffer[dstIdx + 1] = static_cast<uint8_t>(pImageData[srcIdx + 1] >> 8);
            m_pixelBuffer[dstIdx + 2] = static_cast<uint8_t>(pImageData[srcIdx + 2] >> 8);
            m_pixelBuffer[dstIdx + 3] = static_cast<uint8_t>(pImageData[srcIdx + 3] >> 8);
        }
    }

    QImage image = buildImageFromBuffer();
    bool firstTime = !m_sizeDetected;

    QMetaObject::invokeMethod(this, [this, image, firstTime, width, height]() {
        emit imageUpdated(image);
        if (firstTime) {
            m_sizeDetected = true;
            emit sceneSizeDetected(width, height);
        }
    }, Qt::QueuedConnection);
}

void RenderEngine::onLogMessage(int level, const std::string& message)
{
    QString qtMsg = QString::fromUtf8(message.c_str());

    QMetaObject::invokeMethod(this, [this, level, qtMsg]() {
        emit logMessage(level, qtMsg);
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
