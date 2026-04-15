package com.risegfx.android

import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.risegfx.android.nativebridge.RiseCallback
import com.risegfx.android.nativebridge.RiseNative
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertFalse
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
 *   4. Confirm [RiseCallback.onSceneReady] fires, tile callbacks land, and
 *      the final framebuffer contains at least one non-zero pixel.
 *
 * shapes.RISEscene has no texture or mesh dependencies beyond the small
 * colours script that is also bundled, so any failure here is a real library
 * or bridge issue rather than a missing-asset issue.
 */
@RunWith(AndroidJUnit4::class)
class RenderSmokeTest {

    @Test(timeout = TIMEOUT_MS)
    fun renderShapesProducesNonZeroFrame() {
        val app = ApplicationProvider.getApplicationContext<RiseApplication>()
        runBlocking { app.ensureInitialized() }

        val sceneFile = File(app.riseRoot, "scenes/Tests/Geometry/shapes.RISEscene")
        assertTrue("scene should exist at $sceneFile", sceneFile.isFile)

        val sceneReadyLatch = CountDownLatch(1)
        val progressCount  = AtomicInteger(0)
        val tileCount      = AtomicInteger(0)

        RiseNative.nativeSetCallback(object : RiseCallback {
            override fun onProgress(progress: Float) { progressCount.incrementAndGet() }
            override fun onSceneReady(width: Int, height: Int) { sceneReadyLatch.countDown() }
            override fun onRegionInvalidated(packedRect: Long) { tileCount.incrementAndGet() }
            override fun onLog(level: Int, message: String) {}
        })

        val loaded = RiseNative.nativeLoadScene(sceneFile.absolutePath)
        assertTrue("LoadAsciiScene failed", loaded)

        // Rasterize blocks the test thread; the library spawns its own
        // worker pool for tile dispatch. Bounded by the @Test timeout.
        val rasterOk = RiseNative.nativeRasterize()
        assertTrue("Rasterize returned false", rasterOk)
        assertTrue(
            "onSceneReady never fired",
            sceneReadyLatch.await(10, TimeUnit.SECONDS),
        )
        assertTrue("expected at least one tile callback", tileCount.get() > 0)

        val fb = RiseNative.nativeGetFramebuffer()
        assertNotNull("framebuffer not allocated", fb)
        assertFrameHasNonZeroPixel(fb!!)
    }

    private fun assertFrameHasNonZeroPixel(buffer: ByteBuffer) {
        buffer.rewind()
        val arr = ByteArray(buffer.remaining())
        buffer.get(arr)
        val hasNonZero = arr.any { it.toInt() and 0xFF != 0 }
        assertFalse("framebuffer is entirely zero — render did not produce output", !hasNonZero)
    }

    companion object {
        // shapes.RISEscene @ 800x800, 4 samples, recursion 10 takes roughly
        // 10-60s on an arm64 emulator depending on host. Give it generous
        // headroom so slow CI/emulator environments don't flake the test.
        private const val TIMEOUT_MS = 5 * 60 * 1000L
    }
}
