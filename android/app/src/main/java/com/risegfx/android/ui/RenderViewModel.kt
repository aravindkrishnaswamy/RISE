package com.risegfx.android.ui

import android.app.Application
import android.graphics.Bitmap
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.risegfx.android.RiseApplication
import com.risegfx.android.nativebridge.DirtyRect
import com.risegfx.android.nativebridge.RiseCallback
import com.risegfx.android.nativebridge.RiseNative
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicReference
import kotlin.time.Duration.Companion.milliseconds
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.sample
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Owns the render pipeline for the UI. Exposes three observable streams:
 *   - [state]    : high-level [RenderState]
 *   - [progress] : [0f, 1f] progress of an active render
 *   - [frame]    : [ImageBitmap] that recomposes as tiles arrive
 *
 * All native calls are routed through [RiseNative]; the view model never
 * touches library types directly. Tile-dirty callbacks are merged into a
 * bounding rectangle and drained at ~30 Hz — doing a recomposition per tile
 * would overwhelm Compose and burn CPU.
 */
class RenderViewModel(app: Application) : AndroidViewModel(app), RiseCallback {

    private val _state = MutableStateFlow<RenderState>(RenderState.Idle)
    val state: StateFlow<RenderState> = _state.asStateFlow()

    private val _progress = MutableStateFlow(0f)
    val progress: StateFlow<Float> = _progress.asStateFlow()

    // Elapsed ms since the estimator was started for the current render.
    // Zero while idle, frozen on final value once the render finishes.
    private val _elapsedMs = MutableStateFlow(0L)
    val elapsedMs: StateFlow<Long> = _elapsedMs.asStateFlow()

    // Predicted remaining ms, or null while the estimator is still warming
    // up. The UI should render a placeholder when this is null.
    private val _remainingMs = MutableStateFlow<Long?>(null)
    val remainingMs: StateFlow<Long?> = _remainingMs.asStateFlow()

    private val _hasAnimation = MutableStateFlow(false)
    val hasAnimation: StateFlow<Boolean> = _hasAnimation.asStateFlow()

    // Live scrubber position, hoisted from ViewportPane so the VM can
    // pass the value to nativeSetSceneTime before kicking the
    // production rasterizer.  Without this, hitting Render after a
    // timeline scrub would render geometry at the scrubbed time but
    // caustics frozen at the pre-scrub time (the viewport's scrub
    // path skips photon regen for responsiveness).
    private val _sceneTime = MutableStateFlow(0.0)
    val sceneTime: StateFlow<Double> = _sceneTime.asStateFlow()

    // True once nativeLoadScene has succeeded for the current scene.
    // Drives the UI's "show ViewportPane vs SceneList" branching.
    private val _sceneLoaded = MutableStateFlow(false)
    val sceneLoaded: StateFlow<Boolean> = _sceneLoaded.asStateFlow()

    // Counter that increments each time the underlying viewport
    // bridge / SceneEditController is restarted (after scene load,
    // after a production render, etc).  ViewportPane observes this
    // to re-apply persisted UI state — most importantly the
    // selected tool — to the freshly-constructed controller, which
    // defaults to Select internally.
    private val _viewportEpoch = MutableStateFlow(0)
    val viewportEpoch: StateFlow<Int> = _viewportEpoch.asStateFlow()

    // Path of the currently-loaded scene, used by [startRender] to retrigger
    // a production render after the user has been interacting in the
    // viewport.  Null until a scene loads successfully.
    private var currentScenePath: String? = null

    // Polling job that samples elapsed/remaining from the native estimator
    // while a render is in flight. Cancelled when the render ends.
    private var etaPollJob: Job? = null

    var frame by mutableStateOf<ImageBitmap?>(null)
        private set

    // Active render coroutine — cancelled on onCleared or user cancel request.
    private var renderJob: Job? = null

    // Android-side RGBA8 bitmap. Reused across tiles; rebuilt on scene resize.
    private var bitmap: Bitmap? = null
    private var bitmapW = 0
    private var bitmapH = 0

    // Latest dirty rect from native land — merged into a bounding box and
    // drained to the UI at [INVALIDATE_HZ].
    private val dirtyRect = AtomicReference<DirtyRect?>(null)
    private val invalidateSignal = MutableSharedFlow<Unit>(
        replay = 0,
        extraBufferCapacity = 1,
    )

    init {
        RiseNative.nativeSetCallback(this)

        viewModelScope.launch {
            invalidateSignal
                .sample((1000 / INVALIDATE_HZ).milliseconds)
                .collect { drainDirtyAndRepublish() }
        }
    }

    /**
     * Load [scenePath] and run an initial production render, then transition
     * to interactive viewport mode.  Stops any active viewport before
     * loading; restarts it on completion so the user can immediately drag.
     */
    fun loadAndRender(scenePath: String) {
        renderJob?.cancel()
        etaPollJob?.cancel()
        // Tear down the previous viewport (if any) before swapping scenes —
        // the controller borrows pointers into the IJob that nativeLoadScene
        // is about to replace.
        if (RiseNative.nativeViewportIsRunning()) {
            RiseNative.nativeViewportStop()
        }
        _sceneLoaded.value = false
        currentScenePath = scenePath
        renderJob = viewModelScope.launch {
            try {
                (getApplication<RiseApplication>()).ensureInitialized()

                _progress.value = 0f
                _elapsedMs.value = 0L
                _remainingMs.value = null
                _state.value = RenderState.Loading(scenePath)
                Log.i(TAG, "loadAndRender: $scenePath")

                val loaded = withContext(Dispatchers.IO) { RiseNative.nativeLoadScene(scenePath) }
                if (!loaded) {
                    _state.value = RenderState.Error("Failed to load $scenePath")
                    return@launch
                }

                _sceneLoaded.value = true

                // New scene resets the scrub position.  hasAnimation
                // has to flip after each load (and clear back to
                // false on scenes that don't declare animation,
                // otherwise a static-scene load right after an
                // animated one would leave the slider stuck visible).
                // We use nativeHasAnimatedObjects (direct IJob query)
                // rather than the controller-scoped nativeViewport*
                // getters, because the controller isn't started until
                // the end of runProductionRenderInternal.
                _sceneTime.value = 0.0
                _hasAnimation.value = RiseNative.nativeHasAnimatedObjects()

                runProductionRenderInternal()
            } catch (c: CancellationException) {
                etaPollJob?.cancel()
                etaPollJob = null
                _state.value = RenderState.Cancelled
                throw c
            } catch (t: Throwable) {
                etaPollJob?.cancel()
                etaPollJob = null
                Log.e(TAG, "loadAndRender failed", t)
                _state.value = RenderState.Error(t.message ?: t::class.simpleName.orEmpty())
            }
        }
    }

    /**
     * Re-run the production rasterizer on the currently-loaded scene
     * (typically after the user has been interacting in the viewport and
     * wants to bake the current state into a high-quality image).  No-op
     * if no scene is loaded.
     */
    fun startRender() {
        if (currentScenePath == null) return
        if (renderJob?.isActive == true) return
        renderJob = viewModelScope.launch {
            try {
                runProductionRenderInternal()
            } catch (c: CancellationException) {
                etaPollJob?.cancel()
                etaPollJob = null
                _state.value = RenderState.Cancelled
                throw c
            } catch (t: Throwable) {
                etaPollJob?.cancel()
                etaPollJob = null
                Log.e(TAG, "startRender failed", t)
                _state.value = RenderState.Error(t.message ?: t::class.simpleName.orEmpty())
            }
        }
    }

    private suspend fun runProductionRenderInternal() {
        // Capture the canonical scrubbed time from the viewport
        // controller BEFORE stopping the viewport.  On Android,
        // nativeViewportStop destroys the controller (unlike macOS /
        // Windows where stop only halts the render thread); querying
        // afterwards returns 0 regardless of where the user scrubbed.
        // Falls back to `_sceneTime.value` when no viewport is
        // running (initial load, no scrubs possible).
        val canonical = if (RiseNative.nativeViewportIsRunning()) {
            RiseNative.nativeViewportLastSceneTime()
        } else {
            _sceneTime.value
        }

        // Stop the viewport before kicking the production rasterizer —
        // the production renderer takes the same scene + framebuffer the
        // viewport's interactive renderer is writing to.
        if (RiseNative.nativeViewportIsRunning()) {
            RiseNative.nativeViewportStop()
        }

        // Advance scene state to the canonical scrubbed time AND
        // regenerate photon maps before the production rasterizer
        // fires.  The viewport's scrub path calls
        // SetSceneTimeForPreview (animator-only, no photon regen) for
        // responsiveness; without this full SetSceneTime, hitting
        // Render after scrubbing renders the right object positions
        // but caustics frozen at the pre-scrub time.  We use the
        // controller's tracked time (captured above) rather than the
        // slider's local copy because Undo / Redo can change scene
        // time without going through the slider.
        RiseNative.nativeSetSceneTime(canonical)

        // Engage Rendering state HERE, not from the worker-thread
        // onSceneReady callback.  onSceneReady fires from BOTH the
        // production rasterizer and the viewport preview sink (each
        // time the framebuffer is resized) — letting it transition
        // state from Done back to Rendering would falsely engage the
        // production progress UI on a pan or scrub, and nothing on
        // the interactive path ever transitions it back to Done.
        // We use the last known bitmap dims (or 0×0 on the very
        // first render) — onSceneReady will refine the dims once
        // the rasterizer actually starts emitting tiles.
        _progress.value = 0f
        _state.value = RenderState.Rendering(bitmapW, bitmapH)

        // Start the ETA session just before rasterize so elapsed
        // time tracks the render phase, not the parse phase.
        RiseNative.nativeEtaBegin()
        etaPollJob = viewModelScope.launch {
            while (isActive) {
                _elapsedMs.value = RiseNative.nativeEtaElapsedMs()
                val r = RiseNative.nativeEtaRemainingMs()
                _remainingMs.value = if (r >= 0L) r else null
                delay(ETA_POLL_INTERVAL_MS)
            }
        }

        val ok = withContext(Dispatchers.IO) { RiseNative.nativeRasterize() }

        drainDirtyAndRepublish()

        etaPollJob?.cancel()
        etaPollJob = null
        _elapsedMs.value = RiseNative.nativeEtaElapsedMs()
        _remainingMs.value = null

        _state.value = if (ok) RenderState.Done else RenderState.Cancelled

        // Restart the viewport so the user can interact with the scene.
        // The post-production restart needs the first preview frame
        // dropped — otherwise a fast preview pass would flash a
        // half-rendered image right over the just-finished production
        // result.  We thread the suppression intent INTO start (rather
        // than setting it after) so the flag is latched on the sink
        // before Start spawns the render thread.  Doing it after is
        // a race: on a cheap scene the first OutputImage can fire
        // before the follow-up JNI hop sets the flag.
        RiseNative.nativeViewportStart(suppressFirstFrame = true)
        // Bump the epoch so ViewportPane re-applies the persisted
        // tool selection to the freshly-constructed controller.
        _viewportEpoch.value = _viewportEpoch.value + 1
    }

    /**
     * Update the scrubbed time.  Called by ViewportPane on every
     * timeline-slider tick alongside the live nativeViewportScrub
     * call — keeps the VM's view of "where the timeline is" in sync
     * so the next [startRender] passes the right time to
     * [RiseNative.nativeSetSceneTime] before kicking the production
     * rasterizer.
     */
    fun updateSceneTime(t: Double) {
        _sceneTime.value = t
    }

    /** Cooperatively cancel any active render. */
    fun cancel() {
        _state.value = RenderState.Cancelling
        RiseNative.nativeCancel()
        // NOTE: we do NOT cancel renderJob here. nativeRasterize() is still
        // blocking on the IO thread and the library needs to finish its
        // worker join before we allow the coroutine to unwind (otherwise the
        // native framebuffer could be freed while a worker is writing to it).
        // The library will return false from Rasterize() within a few hundred
        // ms; the coroutine will transition to RenderState.Cancelled then.
    }

    override fun onCleared() {
        super.onCleared()
        cancel()
        if (RiseNative.nativeViewportIsRunning()) {
            RiseNative.nativeViewportStop()
        }
        RiseNative.nativeSetCallback(null)
    }

    // -------------------------------------------------------------------------
    // RiseCallback — all methods arrive on library worker threads.
    // -------------------------------------------------------------------------

    override fun onProgress(progress: Float) {
        _progress.value = progress.coerceIn(0f, 1f)
    }

    override fun onSceneReady(width: Int, height: Int) {
        // Worker-thread "framebuffer was (re)allocated at WxH" signal.
        // Fires from BOTH the production rasterizer's first tile AND
        // the viewport preview sink whenever its adaptive scale
        // changes the camera dims (the framebuffer size tracks the
        // rasterizer's output dims).  Job here:
        //   1. Resize the Compose Bitmap so subsequent
        //      copyPixelsFromBuffer doesn't underrun.
        //   2. Refine the Rendering state's reported dims IF we're
        //      already in production-render mode — don't transition
        //      from Done/Idle/Cancelled into Rendering.  That used to
        //      happen here, but the viewport preview path also fires
        //      onSceneReady, so a drag that bounced preview-scale
        //      between 1×, 2×, 4× would falsely engage the production
        //      progress UI and leave the user "Rendering 200×150 ·
        //      87%" with no way to ever transition back to Done —
        //      since the interactive renderer never reports through
        //      the production progress callback.
        ensureBitmap(width, height)
        val cur = _state.value
        if (cur is RenderState.Rendering) {
            _state.value = RenderState.Rendering(width, height)
        }
    }

    override fun onRegionInvalidated(packedRect: Long) {
        val next = DirtyRect.unpack(packedRect)
        // Atomic merge — losing a merge to a race just means the next tile
        // will expand the bounding box. Worst case we draw a slightly larger
        // rectangle than necessary, which is fine.
        dirtyRect.updateAndGet { current -> DirtyRect.merge(current, next) }
        invalidateSignal.tryEmit(Unit)
    }

    override fun onLog(level: Int, message: String) {
        Log.println(level, "RISE", message)
    }

    // -------------------------------------------------------------------------
    // Internal: bitmap bookkeeping
    // -------------------------------------------------------------------------

    @Synchronized
    private fun ensureBitmap(width: Int, height: Int) {
        if (bitmap != null && bitmapW == width && bitmapH == height) return
        bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bitmapW = width
        bitmapH = height
        frame = bitmap?.asImageBitmap()
    }

    /**
     * Copy the latest native framebuffer bytes into the Android Bitmap,
     * then republish [frame] so Compose sees the change. Runs on the
     * viewModelScope's default (Main-immediate) dispatcher because Bitmap
     * mutation is not thread-safe.
     */
    private fun drainDirtyAndRepublish() {
        val rect = dirtyRect.getAndSet(null) ?: return
        val bmp = bitmap ?: return
        val bytes: ByteBuffer = RiseNative.nativeGetFramebuffer() ?: return

        // Full blit of the current framebuffer. The library always passes
        // the full image with a dirty rect; copying only the rect would
        // require a sub-bitmap blit which is noticeably more expensive than
        // the full copy on arm64 for scenes we care about (<= 2048×2048).
        bytes.rewind()
        bmp.copyPixelsFromBuffer(bytes)

        // Rebinding `frame` is what triggers Compose recomposition.
        frame = bmp.asImageBitmap()

        // Log coarse invalidation stats occasionally — helpful when
        // investigating frame-rate issues.
        if (rect.width * rect.height > bitmapW * bitmapH / 2) {
            Log.v(TAG, "drain: rect=$rect (full-frame)")
        }
    }

    companion object {
        private const val TAG = "RISE-VM"
        private const val INVALIDATE_HZ = 30
        private const val ETA_POLL_INTERVAL_MS = 500L
    }
}
