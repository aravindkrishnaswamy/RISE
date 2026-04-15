package com.risegfx.android

import android.app.Application
import android.util.Log
import com.risegfx.android.assets.AssetExtractor
import com.risegfx.android.nativebridge.RiseNative
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File

/**
 * Application entry point. Responsible for:
 *   1. Extracting bundled RISE scenes/textures/meshes from the APK onto the
 *      real filesystem (so std::ifstream inside the library can find them).
 *   2. Calling [RiseNative.nativeInit] with the extracted directory as the
 *      project root and a filesDir path for the log file.
 *
 * Extraction runs on a background coroutine; UI code should await
 * [extractionComplete] before trying to load a scene.
 */
class RiseApplication : Application() {

    val appScope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /** Root of the extracted RISE asset tree: filesDir/rise. */
    lateinit var riseRoot: File
        private set

    @Volatile
    private var initialized = false
    private val initMutex = Mutex()

    override fun onCreate() {
        super.onCreate()
        riseRoot = File(filesDir, "rise").apply { mkdirs() }
        appScope.launch { ensureInitialized() }
    }

    /**
     * Extract assets (if stale) and call into the native bridge. Safe to
     * call repeatedly — only the first call does real work.
     */
    suspend fun ensureInitialized() = initMutex.withLock {
        if (initialized) return
        try {
            val copied = AssetExtractor.extractIfStale(
                context = this@RiseApplication,
                assetSubDir = ASSETS_SUBDIR,
                destDir = riseRoot,
                currentVersionCode = BuildConfig.VERSION_CODE,
            )
            Log.i(TAG, "ensureInitialized: extracted $copied files to $riseRoot")

            val logFile = File(riseRoot, "RISE_Log.txt").absolutePath
            RiseNative.nativeInit(
                projectRoot = riseRoot.absolutePath,
                logFile = logFile,
                threadCount = defaultThreadCount(),
            )
            initialized = true
        } catch (t: Throwable) {
            Log.e(TAG, "ensureInitialized failed", t)
            throw t
        }
    }

    fun isInitialized(): Boolean = initialized

    private fun defaultThreadCount(): Int {
        // Tablets and foldables typically have 6-8 performance cores; phones
        // much less. Leave one core for the UI thread and give the library
        // the rest, capped at 6 so the thermal throttle doesn't kick in on
        // long renders.
        val cores = Runtime.getRuntime().availableProcessors()
        return (cores - 1).coerceIn(2, 6)
    }

    companion object {
        private const val TAG = "RISE-App"
        const val ASSETS_SUBDIR = "rise"
    }
}
