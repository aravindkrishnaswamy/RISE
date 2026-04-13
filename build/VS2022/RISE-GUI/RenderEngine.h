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

signals:
    void stateChanged(RenderEngine::State newState);
    void progressUpdated(double fraction, const QString& title);
    void imageUpdated(const QImage& image);
    void sceneSizeDetected(int width, int height);
    void logMessage(int level, const QString& message);
    void elapsedTimeUpdated(double seconds);
    void errorOccurred(const QString& message);
    void hasAnimationChanged(bool hasAnimation);

public slots:
    void loadScene(const QString& filePath);
    void startRender();
    void startAnimationRender(const QString& videoOutputPath);
    void cancelRender();
    void clearScene();

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

    // Worker thread
    QThread* m_workerThread = nullptr;

    // Forward declarations for callback adapters (defined in .cpp)
    friend class ProgressCallbackAdapter;
    friend class ImageOutputAdapter;
    friend class LogPrinterAdapter;
};

#endif // RENDERENGINE_H
