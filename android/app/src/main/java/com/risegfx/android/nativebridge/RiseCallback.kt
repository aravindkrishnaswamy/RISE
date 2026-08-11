package com.risegfx.android.nativebridge

/**
 * Callback interface invoked from native code. All methods may arrive on
 * arbitrary library worker threads — implementations MUST be thread-safe
 * and must not do expensive work inline (the library's progress mutex is
 * held while these fire). Callback delivery does not hold the bridge's
 * callback-reference mutex, so [RiseNative.nativeOwnsCallback] and
 * [RiseNative.nativeCancel] may be called inline. Callbacks MUST NOT call
 * scene-lifecycle or viewport APIs (`nativeSetCallback`, `nativeClearCallback`,
 * `nativeLoadScene`, `nativeRasterize`, or any `nativeViewport*` method): a
 * load/render may already own the process-wide scene lifecycle on the current
 * thread, and re-entering it would be both recursive and semantically invalid.
 *
 * The JNI layer caches these method IDs in JNI_OnLoad, so the class name
 * and method signatures here are load-bearing. Do not rename without
 * updating jni_util.cpp.
 */
interface RiseCallback {

    /**
     * Progressive render progress in [0f, 1f]. Serialized by the library
     * across all worker threads, so the value is monotonic in practice
     * (modulo unavoidable races on the final tile).
     */
    fun onProgress(progress: Float)

    /**
     * Fired whenever the native display framebuffer is allocated or resized,
     * and once for a replacement callback when a same-sized framebuffer is
     * already available. Adaptive interactive rendering can therefore fire
     * this multiple times within one scene. After each notification,
     * [RiseNative.nativeCopyFramebuffer] can copy a snapshot of size w*h*4
     * into caller-owned storage.
     */
    fun onSceneReady(width: Int, height: Int)

    /**
     * The native display framebuffer has been refreshed. Current production
     * and interactive paths publish full-frame invalidations at display/frame
     * cadence rather than one callback per render tile. The affected rectangle
     * is packed into [packedRect] as ((top & 0xFFFF) << 48) |
     * ((left & 0xFFFF) << 32) | ((bottom & 0xFFFF) << 16) | (right & 0xFFFF),
     * inclusive on all four edges. The UI side coalesces notifications and
     * republishes at display cadence.
     */
    fun onRegionInvalidated(packedRect: Long)

    /**
     * A log line from the RISE library. [level] matches the Android Logcat
     * priority constants (ANDROID_LOG_INFO=4, WARN=5, ERROR=6).
     */
    fun onLog(level: Int, message: String)
}

/** Utility: unpack the four uint16 fields from a rect passed to
 *  [RiseCallback.onRegionInvalidated]. */
data class DirtyRect(val top: Int, val left: Int, val bottom: Int, val right: Int) {
    val width:  Int get() = (right - left + 1).coerceAtLeast(0)
    val height: Int get() = (bottom - top + 1).coerceAtLeast(0)

    companion object {
        fun unpack(packedRect: Long): DirtyRect = DirtyRect(
            top    = ((packedRect ushr 48) and 0xFFFF).toInt(),
            left   = ((packedRect ushr 32) and 0xFFFF).toInt(),
            bottom = ((packedRect ushr 16) and 0xFFFF).toInt(),
            right  = ((packedRect       ) and 0xFFFF).toInt(),
        )

        /** Merge two dirty rects into the smallest bounding box containing both. */
        fun merge(a: DirtyRect?, b: DirtyRect): DirtyRect {
            if (a == null) return b
            return DirtyRect(
                top    = minOf(a.top, b.top),
                left   = minOf(a.left, b.left),
                bottom = maxOf(a.bottom, b.bottom),
                right  = maxOf(a.right, b.right),
            )
        }
    }
}
