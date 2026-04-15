package com.risegfx.android.ui

import java.io.File

/**
 * Curated set of bundled scenes that the app exposes in the left pane.
 *
 * Scene paths are relative to the extracted RISE root
 * (filesDir/rise/...). The list is intentionally small for v1 — the
 * first entry is the instrumented-test scene and the rest are showcase
 * geometry scenes with no external texture/mesh dependencies, so they
 * always resolve even before we bundle the full FeatureBased tree.
 */
data class SceneEntry(
    val displayName: String,
    val relativePath: String,
    val description: String,
)

object SceneCatalog {
    val bundled: List<SceneEntry> = listOf(
        SceneEntry(
            displayName = "Shapes (smoke test)",
            relativePath = "scenes/Tests/Geometry/shapes.RISEscene",
            description = "Standard shader over spheres, boxes, and an ellipsoid. Pure geometry — used by the instrumented test.",
        ),
    )

    /** Resolve a bundled entry to an absolute path under the RISE root. */
    fun absolutePath(riseRoot: File, entry: SceneEntry): String =
        File(riseRoot, entry.relativePath).absolutePath
}
