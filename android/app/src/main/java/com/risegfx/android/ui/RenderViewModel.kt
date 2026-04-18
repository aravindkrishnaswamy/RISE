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
     * Load and render [scenePath]. Cancels any in-flight render first.
     * Blocks on [Dispatchers.IO] for the parse and the entire raytrace.
     */
    fun loadAndRender(scenePath: String) {
        renderJob?.cancel()
        etaPollJob?.cancel()
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

                // Transition to Rendering as soon as the first callback fires
                // (via onSceneReady). Until then we stay in Loading.
                val ok = withContext(Dispatchers.IO) { RiseNative.nativeRasterize() }

                // Final drain — pick up any tiles emitted after the last
                // invalidate signal tick.
                drainDirtyAndRepublish()

                etaPollJob?.cancel()
                etaPollJob = null
                _elapsedMs.value = RiseNative.nativeEtaElapsedMs()
                _remainingMs.value = null

                _state.value = if (ok) RenderState.Done else RenderState.Cancelled
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
        RiseNative.nativeSetCallback(null)
    }

    // -------------------------------------------------------------------------
    // RiseCallback — all methods arrive on library worker threads.
    // -------------------------------------------------------------------------

    override fun onProgress(progress: Float) {
        _progress.value = progress.coerceIn(0f, 1f)
    }

    override fun onSceneReady(width: Int, height: Int) {
        // Called from a worker thread. Build the Bitmap here (cheap) and
        // transition state. Compose will recompose on the next dirty drain.
        ensureBitmap(width, height)
        _state.value = RenderState.Rendering(width, height)
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
