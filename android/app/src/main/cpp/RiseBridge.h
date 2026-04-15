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

// Forward-declare RISE types to keep this header small and the JNI bridge
// free of library internals.
namespace RISE {
    class IJobPriv;
    class IProgressCallback;
    class IJobRasterizerOutput;
    class ILogPrinter;
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
};

// Process-wide singleton accessor. The bridge is created lazily on first
// call and destroyed in nativeShutdown (if ever called) or at process exit.
RiseBridge& getBridge();

} // namespace rise_jni

#endif // RISE_BRIDGE_H_
