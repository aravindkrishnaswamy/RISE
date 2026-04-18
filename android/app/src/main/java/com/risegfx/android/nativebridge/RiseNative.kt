package com.risegfx.android.nativebridge

import java.nio.ByteBuffer

/**
 * Kotlin-side facade for librise_jni.so. All methods are thin JNI entry
 * points; the real work happens in C++ inside rise_jni.cpp and the RiseBridge
 * class.
 *
 * Threading rules (mirror the plan file):
 *   - [nativeInit] and [nativeSetCallback] are cheap and safe from the main
 *     thread.
 *   - [nativeLoadScene] parses a scene (blocking I/O) — call from
 *     Dispatchers.IO.
 *   - [nativeRasterize] is BLOCKING for the entire duration of a render
 *     (potentially seconds to minutes). MUST NOT be called from the main
 *     thread. The library spawns its own pthread worker pool underneath
 *     this call.
 *   - [nativeCancel] is fire-and-forget; it sets an atomic flag that the
 *     library polls on tile boundaries. A render can take several hundred
 *     ms to actually wind down after cancel.
 *
 * The shared library loads once at classload and stays resident for the
 * life of the process.
 */
object RiseNative {

    init {
        System.loadLibrary("rise_jni")
    }

    /**
     * One-time initialization. Safe to call multiple times — only the first
     * call actually takes effect because the library caches [GlobalOptions]
     * on first access.
     *
     * @param projectRoot absolute path of the directory containing the
     *   extracted RISE asset tree (filesDir/rise). Added to the media path
     *   locator so scenes can resolve relative paths against it.
     * @param logFile absolute path for the RISE_Log.txt file. Redirected
     *   before any library call so nothing ever logs to CWD.
     * @param threadCount value written to global.options as
     *   force_number_of_threads. Also sets support_hyperthreading=false so
     *   the library doesn't double-count cores on Android devices.
     */
    external fun nativeInit(projectRoot: String, logFile: String, threadCount: Int)

    /**
     * Register (or clear with null) the [RiseCallback] that will receive
     * progress, tile-dirty and log notifications. Held as a JNI global ref
     * on the native side.
     */
    external fun nativeSetCallback(callback: RiseCallback?)

    /**
     * Parse an ASCII RISE scene file. Blocking. Returns false on parse
     * error.
     */
    external fun nativeLoadScene(absPath: String): Boolean

    /**
     * Render the currently loaded scene. BLOCKING. Returns false if the
     * render was cancelled or failed.
     */
    external fun nativeRasterize(): Boolean

    /** Request cooperative cancellation of an in-flight render. */
    external fun nativeCancel()

    /**
     * Expose the native-owned RGBA8 framebuffer as a direct [ByteBuffer].
     * The returned buffer wraps C++ memory and is valid until the next
     * [nativeLoadScene] with different dimensions or process shutdown.
     *
     * Returns null before [RiseCallback.onSceneReady] has fired.
     */
    external fun nativeGetFramebuffer(): ByteBuffer?

    /**
     * Reset the render-time estimator and capture the current time as the
     * start of a new render session. Call at the moment the UI transitions
     * into [RenderState.Rendering] (or just before [nativeRasterize]).
     */
    external fun nativeEtaBegin()

    /** Milliseconds since the last [nativeEtaBegin] call. */
    external fun nativeEtaElapsedMs(): Long

    /**
     * Predicted remaining milliseconds for the current render, or -1 when
     * the estimator is still warming up. The UI should show "estimating…"
     * when this returns -1.
     */
    external fun nativeEtaRemainingMs(): Long
}
