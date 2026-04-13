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

    // Elapsed time timer
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(500);
    connect(m_elapsedTimer, &QTimer::timeout, this, [this]() {
        emit elapsedTimeUpdated(m_renderClock.elapsed() / 1000.0);
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
        m_job->release();
        m_job = nullptr;
    }
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

    // Install image output callback
    auto* imageOutput = new ImageOutputAdapter(this);
    m_job->AddCallbackRasterizerOutput(imageOutput);

    // Start elapsed timer
    m_renderClock.start();
    m_elapsedTimer->start();

    QThread* thread = QThread::create([this, progressCb, imageOutput]() {
        bool ok = m_job->Rasterize();

        QMetaObject::invokeMethod(this, [this, ok, progressCb, imageOutput]() {
            m_elapsedTimer->stop();
            m_job->SetProgress(nullptr);
            delete progressCb;
            delete imageOutput;

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

    // Clear outputs from previous renders
    rasterizer->FreeRasterizerOutputs();

    // Install progress callback
    auto* progressCb = new ProgressCallbackAdapter(this);
    m_job->SetProgress(progressCb);

    // Install image output callback
    auto* imageOutput = new ImageOutputAdapter(this);
    m_job->AddCallbackRasterizerOutput(imageOutput);

    // TODO: Create and attach VideoEncoder for H.264 output
    // VideoEncoder* videoEncoder = new VideoEncoder(videoOutputPath.toStdString());
    // rasterizer->AddRasterizerOutput(videoEncoder);
    // videoEncoder->release(); // rasterizer now owns it

    // Start elapsed timer
    m_renderClock.start();
    m_elapsedTimer->start();

    QThread* thread = QThread::create([this, progressCb, imageOutput, rasterizer]() {
        bool ok = m_job->RasterizeAnimationUsingOptions();

        // TODO: Finalize video encoder here
        // if (videoEncoder) videoEncoder->finalize();

        // Free rasterizer outputs
        rasterizer->FreeRasterizerOutputs();

        QMetaObject::invokeMethod(this, [this, ok, progressCb, imageOutput]() {
            m_elapsedTimer->stop();
            m_job->SetProgress(nullptr);
            delete progressCb;
            delete imageOutput;

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

void RenderEngine::clearScene()
{
    if (m_job) {
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
