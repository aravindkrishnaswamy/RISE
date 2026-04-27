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
     * Advance the in-memory scene to time `t` AND regenerate every
     * populated photon map.  Called by [RenderViewModel] just before
     * [nativeRasterize] so post-scrub production renders pick up
     * caustics consistent with the scrubbed scene state — the
     * interactive viewport's scrub path uses a faster animator-only
     * variant for responsiveness, which leaves photons stale for
     * production.  Photon-heavy scenes can pause many seconds inside
     * this call; the caller should already be in a "rendering" UI
     * state.
     */
    external fun nativeSetSceneTime(t: Double)

    /**
     * True if the loaded scene declares any keyframed objects.  Used
     * to surface the timeline scrubber in the viewport UI.  Safe to
     * call right after [nativeLoadScene]; doesn't require the
     * interactive viewport controller to be running.
     */
    external fun nativeHasAnimatedObjects(): Boolean

    /**
     * Canonical scene time tracked by the SceneEditController's edit
     * history.  Updated by every time-scrub AND by Undo / Redo of a
     * SetSceneTime edit, so this is the truth that production
     * renders should pass to [nativeSetSceneTime] — the local
     * `_sceneTime` StateFlow only tracks slider events, so it goes
     * stale when undo/redo changes scene time without touching the
     * slider.  Returns 0 when no controller is attached.
     */
    external fun nativeViewportLastSceneTime(): Double

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

    // -----------------------------------------------------------------------
    // Interactive viewport — mirrors RISEViewportBridge on macOS and
    // ViewportBridge on Windows.  See the C++ SceneEditController for the
    // canonical contract.  The viewport reuses the same framebuffer +
    // RiseCallback.onRegionInvalidated path as production renders, so the
    // UI doesn't need to know which mode is producing the current frame.
    // -----------------------------------------------------------------------

    /**
     * Build the live-preview rasterizer + sink, create the
     * SceneEditController, and start its render thread.  When
     * [suppressFirstFrame] is true (typical post-production-render
     * restart), the first preview frame is dropped at the sink layer
     * so the production image stays on screen until the user starts
     * dragging — without that, a fast preview pass can blit through
     * before the user notices.  The flag is latched on the sink
     * INSIDE the start call (before the render thread spawns), which
     * closes the race against the previous "Start, then SuppressNext"
     * sequence: a cheap scene could complete pass #1 before the
     * follow-up JNI hop returned.
     */
    external fun nativeViewportStart(suppressFirstFrame: Boolean): Boolean
    external fun nativeViewportStop()
    external fun nativeViewportIsRunning(): Boolean
    external fun nativeViewportHasLivePreview(): Boolean

    /**
     * Drop exactly one upcoming preview frame at the sink layer.  Call
     * just before [nativeViewportStart] after a production render so the
     * production image stays on screen until the user actually starts
     * dragging.  Auto-clears after one drop.
     */
    external fun nativeViewportSuppressNextFrame()

    external fun nativeViewportSetTool(tool: Int)
    external fun nativeViewportPointerDown(x: Double, y: Double)
    external fun nativeViewportPointerMove(x: Double, y: Double)
    external fun nativeViewportPointerUp(x: Double, y: Double)

    /**
     * Stable full-resolution camera dimensions for pointer-event coord
     * conversion in [ViewportPane].  Returns a packed long: hi 32 bits
     * = width, lo 32 bits = height.  Returns 0 (both halves zero) when
     * no camera is attached.  See [SceneEditController::GetCameraDimensions]
     * for why the bridge can't read [QImage::size]-equivalent dims —
     * those flicker with subsampling and produce 4×–32× pan/orbit
     * jumps when the preview-scale state machine steps.
     */
    external fun nativeViewportCameraDimensions(): Long

    /**
     * Scene's animation duration in scene-time units, derived from the
     * `animation_options` chunk's `time_end`.  Used by [ViewportPane]
     * to size the timeline scrubber's slider range.  Returns 0 when
     * the scene declares no animation.
     */
    external fun nativeViewportAnimationTimeEnd(): Double

    /**
     * Scene's frame count from the `animation_options` chunk.  Useful
     * if the UI wants to surface the count alongside the time-based
     * slider.  Returns 0 when no animation is declared.
     */
    external fun nativeViewportAnimationNumFrames(): Int
    external fun nativeViewportScrubBegin()
    external fun nativeViewportScrub(t: Double)
    external fun nativeViewportScrubEnd()

    /**
     * Bracket a property-panel chevron drag — same scale-bump
     * machinery the camera tools use during a viewport drag.  Without
     * these brackets the rapid-fire setProperty stream cancels every
     * in-flight render before the outer tiles get a chance.
     */
    external fun nativeViewportBeginPropertyScrub()
    external fun nativeViewportEndPropertyScrub()
    external fun nativeViewportUndo()
    external fun nativeViewportRedo()
    external fun nativeViewportProductionRender(): Boolean

    external fun nativeViewportRefreshProperties()
    /**
     * 0 = None (Select with no pick), 1 = Camera (camera tool active),
     * 2 = Object (Select with picked object).  Drives whether the
     * right panel renders empty / camera / object content.
     */
    external fun nativeViewportPanelMode(): Int
    external fun nativeViewportPanelHeader(): String
    external fun nativeViewportPropertyCount(): Int
    external fun nativeViewportPropertyName(idx: Int): String
    external fun nativeViewportPropertyValue(idx: Int): String
    external fun nativeViewportPropertyDescription(idx: Int): String
    external fun nativeViewportPropertyKind(idx: Int): Int
    external fun nativeViewportPropertyEditable(idx: Int): Boolean
    external fun nativeViewportSetProperty(name: String, value: String): Boolean
}
