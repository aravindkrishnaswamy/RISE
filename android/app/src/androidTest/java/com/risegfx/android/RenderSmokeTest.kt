package com.risegfx.android

import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.risegfx.android.nativebridge.DirtyRect
import com.risegfx.android.nativebridge.RiseCallback
import com.risegfx.android.nativebridge.RiseNative
import com.risegfx.android.ui.SceneCatalog
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * End-to-end smoke test that runs on the emulator / device:
 *   1. Wait for [RiseApplication.ensureInitialized] so bundled assets are
 *      extracted.
 *   2. Load the bundled shapes.RISEscene.
 *   3. Kick off a synchronous Rasterize() on the test runner thread.
 *   4. Confirm [RiseCallback.onSceneReady] fires, a full-frame display
 *      invalidation lands, and the framebuffer contains non-zero pixels.
 *
 * shapes.RISEscene is self-contained, so any failure here is a real library
 * or bridge issue rather than a missing-asset issue.
 */
@RunWith(AndroidJUnit4::class)
class RenderSmokeTest {

    @Test(timeout = TIMEOUT_MS)
    fun renderShapesProducesNonZeroFrame() {
        val app = ApplicationProvider.getApplicationContext<RiseApplication>()
        runBlocking { app.ensureInitialized() }

        val sceneFile = File(app.riseRoot, SceneCatalog.smokeTests.single().relativePath)
        assertTrue("scene should exist at $sceneFile", sceneFile.isFile)

        val sceneReadyLatch = CountDownLatch(1)
        val progressCount  = AtomicInteger(0)
        val refreshCount   = AtomicInteger(0)
        val lastRefresh    = AtomicLong(-1L)
        val frameWidth     = AtomicInteger(0)
        val frameHeight    = AtomicInteger(0)
        val staleCallbackCount = AtomicInteger(0)
        val activeOwnerForReentry = AtomicLong(0L)
        val staleOwnerForReentry = AtomicLong(0L)
        val callbackReentryCount = AtomicInteger(0)
        val staleCancelRejectionCount = AtomicInteger(0)

        val staleCallback = object : RiseCallback {
            override fun onProgress(progress: Float) { staleCallbackCount.incrementAndGet() }
            override fun onSceneReady(width: Int, height: Int) { staleCallbackCount.incrementAndGet() }
            override fun onRegionInvalidated(packedRect: Long) { staleCallbackCount.incrementAndGet() }
            override fun onLog(level: Int, message: String) { staleCallbackCount.incrementAndGet() }
        }

        val callback = object : RiseCallback {
            override fun onProgress(progress: Float) {
                progressCount.incrementAndGet()
                val activeOwner = activeOwnerForReentry.get()
                if (activeOwner != 0L && RiseNative.nativeOwnsCallback(activeOwner)) {
                    callbackReentryCount.incrementAndGet()
                }
                val staleOwner = staleOwnerForReentry.get()
                if (staleOwner != 0L && !RiseNative.nativeCancel(staleOwner)) {
                    staleCancelRejectionCount.incrementAndGet()
                }
            }
            override fun onSceneReady(width: Int, height: Int) {
                frameWidth.set(width)
                frameHeight.set(height)
                sceneReadyLatch.countDown()
            }
            override fun onRegionInvalidated(packedRect: Long) {
                lastRefresh.set(packedRect)
                refreshCount.incrementAndGet()
            }
            override fun onLog(level: Int, message: String) {}
        }
        val requestBase = System.nanoTime().coerceAtLeast(1_000_000L)
        val firstOwner = RiseNative.nativeSetCallback(staleCallback, requestBase)
        val callbackOwner = RiseNative.nativeSetCallback(callback, requestBase + 2L)
        val delayedStaleOwner = RiseNative.nativeSetCallback(staleCallback, requestBase + 1L)
        assertTrue("first callback ownership token should be nonzero", firstOwner != 0L)
        assertTrue("newest callback ownership token should be nonzero", callbackOwner != 0L)
        activeOwnerForReentry.set(callbackOwner)
        staleOwnerForReentry.set(firstOwner)
        assertTrue("out-of-order stale callback request must reject", delayedStaleOwner == 0L)
        RiseNative.nativeClearCallback(firstOwner)
        assertTrue(
            "clearing an old token must preserve the newest callback owner",
            RiseNative.nativeOwnsCallback(callbackOwner),
        )
        assertTrue(
            "stale callback owner cannot load a scene",
            !RiseNative.nativeLoadScene(sceneFile.absolutePath, firstOwner),
        )
        assertTrue(
            "stale callback owner cannot cancel the current render lifecycle",
            !RiseNative.nativeCancel(firstOwner),
        )

        try {
            val loaded = RiseNative.nativeLoadScene(sceneFile.absolutePath, callbackOwner)
            assertTrue("LoadAsciiScene failed", loaded)

            // Rasterize blocks the test thread; the library spawns its own
            // worker pool for tile dispatch. Bounded by the @Test timeout.
            val rasterOk = RiseNative.nativeRasterize(callbackOwner)
            assertTrue("Rasterize returned false", rasterOk)
            assertTrue(
                "callback may re-enter owner query without deadlocking",
                callbackReentryCount.get() > 0,
            )
            assertTrue(
                "callback re-entry rejects stale cancellation without deadlocking",
                staleCancelRejectionCount.get() > 0,
            )
            assertTrue(
                "onSceneReady never fired",
                sceneReadyLatch.await(10, TimeUnit.SECONDS),
            )
            assertTrue("expected at least one display refresh", refreshCount.get() > 0)
            assertTrue("superseded callbacks receive no render events", staleCallbackCount.get() == 0)
            val refreshed = DirtyRect.unpack(lastRefresh.get())
            assertTrue(
                "production refresh must cover the completed framebuffer",
                refreshed.top == 0 && refreshed.left == 0 &&
                    refreshed.bottom == frameHeight.get() - 1 &&
                    refreshed.right == frameWidth.get() - 1,
            )

            val firstBytes = ByteBuffer.allocateDirect(frameWidth.get() * frameHeight.get() * 4)
            val nullableSnapshot = RiseNative.nativeCopyFramebuffer(firstBytes)
            assertNotNull("framebuffer not allocated", nullableSnapshot)
            val fb = requireNotNull(nullableSnapshot)
            assertTrue("snapshot width should be positive", fb.width > 0)
            assertTrue("snapshot height should be positive", fb.height > 0)
            assertTrue("snapshot generation should be positive", fb.generation > 0)
            assertTrue(
                "snapshot byte count must match dimensions",
                fb.byteCount == fb.width * fb.height * 4,
            )
            assertTrue("framebuffer snapshot should fit caller storage", fb.copied)
            val knownNonZeroIndex = firstNonZeroByteIndex(firstBytes, fb.byteCount)
            assertTrue(
                "framebuffer is entirely zero — render did not produce output",
                knownNonZeroIndex >= 0,
            )

            val originalByte = firstBytes.get(knownNonZeroIndex)
            firstBytes.put(knownNonZeroIndex, (originalByte.toInt() xor 0xFF).toByte())
            val secondBytes = ByteBuffer.allocateDirect(fb.byteCount)
            val poisonByte = (originalByte.toInt() xor 0xFF).toByte()
            secondBytes.put(knownNonZeroIndex, poisonByte)
            val nullableNext = RiseNative.nativeCopyFramebuffer(secondBytes)
            assertNotNull("second framebuffer snapshot not allocated", nullableNext)
            val next = requireNotNull(nullableNext)
            assertTrue(
                "second snapshot envelope must describe the same completed frame",
                next.width == fb.width && next.height == fb.height &&
                    next.byteCount == fb.byteCount && next.generation == fb.generation,
            )
            assertTrue(
                "mutating caller-owned storage must not mutate the native framebuffer",
                next.copied && secondBytes.get(knownNonZeroIndex) == originalByte &&
                    secondBytes.get(knownNonZeroIndex) != poisonByte,
            )

            assertTrue(
                "current owner can start an interactive viewport after rendering",
                RiseNative.nativeViewportStart(false, callbackOwner),
            )
            assertTrue(
                "stale callback owner cannot access the live viewport controller",
                !RiseNative.nativeViewportStart(false, firstOwner) &&
                    !RiseNative.nativeViewportStop(firstOwner) &&
                    !RiseNative.nativeViewportSetSurfaceDimensions(
                        32,24,firstOwner) &&
                    !RiseNative.nativeViewportSetProperty(
                        "exposure","1",firstOwner),
            )
            assertTrue(
                "stale callback owner cannot inspect post-render auto state",
                RiseNative.nativeAutoResolvedIntegrator(firstOwner).isEmpty() &&
                    RiseNative.nativeAutoResolveReason(firstOwner).isEmpty(),
            )

            val startControllerRace = CountDownLatch(1)
            val pointerFinished = CountDownLatch(1)
            val pointerThread = Thread {
                try {
                    startControllerRace.await()
                    repeat(256) {
                        RiseNative.nativeViewportPointerDown(
                            1.0,1.0,callbackOwner)
                        RiseNative.nativeViewportPointerUp(
                            1.0,1.0,callbackOwner)
                    }
                } finally {
                    pointerFinished.countDown()
                }
            }
            pointerThread.start()
            startControllerRace.countDown()
            val stoppedDuringPointerTraffic =
                RiseNative.nativeViewportStop(callbackOwner)
            assertTrue(
                "pointer traffic completes across controller teardown",
                pointerFinished.await(10,TimeUnit.SECONDS),
            )
            pointerThread.join()
            assertTrue(
                "a busy fail-fast stop succeeds after pointer traffic quiesces",
                stoppedDuringPointerTraffic ||
                    RiseNative.nativeViewportStop(callbackOwner),
            )
            assertTrue(
                "viewport is stopped after the contention retry",
                !RiseNative.nativeViewportIsRunning(callbackOwner),
            )
        } finally {
            RiseNative.nativeClearCallback(callbackOwner)
        }
    }

    private fun firstNonZeroByteIndex(buffer: ByteBuffer, byteCount: Int): Int {
        for (i in 0 until byteCount) {
            if (buffer.get(i).toInt() and 0xFF != 0) {
                return i
            }
        }
        return -1
    }

    companion object {
        // shapes.RISEscene @ 800x800, 4 samples, recursion 10 takes roughly
        // 10-60s on an arm64 emulator depending on host. Give it generous
        // headroom so slow CI/emulator environments don't flake the test.
        private const val TIMEOUT_MS = 5 * 60 * 1000L
    }
}
