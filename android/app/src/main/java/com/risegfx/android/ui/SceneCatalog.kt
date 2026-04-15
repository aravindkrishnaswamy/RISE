package com.risegfx.android.ui

import java.io.File

/**
 * Curated set of bundled scenes the app exposes in the left pane.
 *
 * Scene paths are relative to the extracted RISE root (filesDir/rise/...).
 * The whole tree (scenes + textures + meshes + light probes) is shipped
 * inside the APK under app/src/main/assets/rise/ and copied to filesDir on
 * first launch by [com.risegfx.android.assets.AssetExtractor].
 *
 * If you want to add another scene from the upstream scenes/ directory,
 * the checklist is:
 *   1. Copy the .RISEscene file into the matching subpath under
 *      app/src/main/assets/rise/scenes/
 *   2. Find every dependency it references (`> run`, `load`, `file ...`)
 *      and copy those into the matching subpath under assets/rise/
 *   3. If the scene's `file_rasterizeroutput` pattern writes to a nested
 *      subdirectory like "rendered/foo/frame", add "rendered/foo" to
 *      kRenderedDirs in cpp/RiseBridge.cpp::initialize()
 *   4. Add a [SceneEntry] below
 *   5. Bump versionCode in app/build.gradle.kts so the AssetExtractor
 *      re-extracts on next launch
 */
data class SceneEntry(
    val displayName: String,
    val relativePath: String,
    val description: String,
)

object SceneCatalog {
    val bundled: List<SceneEntry> = listOf(
        // -- Smallest, fastest, no asset deps. Always render-able. ----------
        SceneEntry(
            displayName = "Shapes",
            relativePath = "scenes/Tests/Geometry/shapes.RISEscene",
            description = "Standard shader over spheres, boxes and an ellipsoid. Pure geometry, no textures or meshes — also used by the instrumented test.",
        ),
        SceneEntry(
            displayName = "Sombrero",
            relativePath = "scenes/FeatureBased/Parser/sombrero.RISEscene",
            description = "Parser-generated sombrero surface. Showcases RISE's procedural scene description with no external assets.",
        ),
        SceneEntry(
            displayName = "Pillow",
            relativePath = "scenes/FeatureBased/Parser/pillow.RISEscene",
            description = "Parser showcase: deformed quad with subdivision, lit with the standard colour palette.",
        ),

        // -- Painters and materials ---------------------------------------
        SceneEntry(
            displayName = "Black Body Radiator",
            relativePath = "scenes/Tests/Painters/blackbodyradiator.RISEscene",
            description = "Physically-based black body painter sweep across temperatures.",
        ),
        SceneEntry(
            displayName = "Painters Gallery",
            relativePath = "scenes/Tests/Painters/painters.RISEscene",
            description = "Sampling of RISE painters (Perlin, Worley, checker, texture map). Uses a paradise.png environment texture.",
        ),
        SceneEntry(
            displayName = "Materials Gallery",
            relativePath = "scenes/Tests/Materials/materials.RISEscene",
            description = "BRDF/SPF showcase: Lambertian, Phong, Cook-Torrance, dielectric and refractive surfaces with the classic teapot.",
        ),

        // -- Lighting and global illumination ------------------------------
        SceneEntry(
            displayName = "GI Spheres",
            relativePath = "scenes/FeatureBased/Combined/gi_spheres.RISEscene",
            description = "Cornell-style global illumination test with coloured spheres. Slower — exercises path tracing and direct lighting together.",
        ),

        // -- Hero showcase: a full mesh scene with HDR environment ---------
        SceneEntry(
            displayName = "Teapot",
            relativePath = "scenes/FeatureBased/Geometry/teapot.RISEscene",
            description = "Bezier-patch Utah teapot with a water bumpmap and the rnl HDR light probe. The fastest mesh-based showcase in the upstream catalogue — still significantly slower than the primitive scenes above.",
        ),
    )

    /** Resolve a bundled entry to an absolute path under the RISE root. */
    fun absolutePath(riseRoot: File, entry: SceneEntry): String =
        File(riseRoot, entry.relativePath).absolutePath
}
