package com.risegfx.android.assets

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import java.io.File
import java.io.FileOutputStream

/**
 * Recursively copies a subtree of the APK's AssetManager onto the real
 * filesystem under the app's filesDir. The RISE library uses std::ifstream
 * to open scene, texture and mesh files, which cannot read APK assets
 * directly — so we extract once on first launch (or after a version bump)
 * and point the library's MediaPathLocator at the extracted copy.
 *
 * Extraction is skipped if the version recorded in SharedPreferences matches
 * the current [android.os.BuildConfig.VERSION_CODE].
 */
object AssetExtractor {

    private const val TAG = "RISE-Extract"
    private const val PREFS = "rise_assets"
    private const val KEY_VERSION = "extracted_version_code"

    /**
     * Extract [assetSubDir] from the APK into [destDir] if the previously
     * recorded version does not match [currentVersionCode]. Deletes the
     * destination first so stale files from an older app version are
     * cleaned up.
     *
     * Returns the number of files copied (0 if no extraction was needed).
     */
    fun extractIfStale(
        context: Context,
        assetSubDir: String,
        destDir: File,
        currentVersionCode: Int,
    ): Int {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val recorded = prefs.getInt(KEY_VERSION, -1)
        if (recorded == currentVersionCode && destDir.isDirectory && destDir.list()?.isNotEmpty() == true) {
            Log.i(TAG, "extractIfStale: up to date (version $recorded), skipping")
            return 0
        }

        Log.i(TAG, "extractIfStale: version $recorded -> $currentVersionCode, extracting $assetSubDir")
        if (destDir.exists()) {
            destDir.deleteRecursively()
        }
        destDir.mkdirs()

        val copied = copyRecursive(context.assets, assetSubDir, destDir)
        prefs.edit().putInt(KEY_VERSION, currentVersionCode).apply()
        Log.i(TAG, "extractIfStale: copied $copied files")
        return copied
    }

    /**
     * Walk [assetPath] in the AssetManager and copy each file into the
     * matching subdirectory of [dest]. Uses the fact that
     * [AssetManager.list] returns an empty array for files and a non-empty
     * array for directories (this is the standard technique — there is no
     * direct "is directory" API in AssetManager).
     */
    private fun copyRecursive(assets: AssetManager, assetPath: String, dest: File): Int {
        val children = try {
            assets.list(assetPath) ?: return 0
        } catch (e: Exception) {
            Log.e(TAG, "list failed for $assetPath", e)
            return 0
        }

        if (children.isEmpty()) {
            // Either a file, or an empty directory — try to open it as a file.
            return copyOne(assets, assetPath, dest)
        }

        dest.mkdirs()
        var total = 0
        for (child in children) {
            val childAssetPath = if (assetPath.isEmpty()) child else "$assetPath/$child"
            val childDest = File(dest, child)
            total += copyRecursive(assets, childAssetPath, childDest)
        }
        return total
    }

    private fun copyOne(assets: AssetManager, assetPath: String, dest: File): Int {
        return try {
            dest.parentFile?.mkdirs()
            assets.open(assetPath).use { input ->
                FileOutputStream(dest).use { output ->
                    input.copyTo(output)
                }
            }
            1
        } catch (e: Exception) {
            Log.w(TAG, "copyOne: $assetPath not a file, skipping: ${e.message}")
            0
        }
    }
}
