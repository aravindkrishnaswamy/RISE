package com.risegfx.android.ui

import java.io.File

/**
 * Curated set of bundled scenes the app exposes in the left pane.
 *
 * The `relativePath` strings here are the SOURCE OF TRUTH for what gets
 * bundled into the APK.  At Gradle configuration time, `app/build.gradle.kts`
 * scrapes this file with a regex (`relativePath = "..."`) to discover the
 * scene list, then recursively walks each scene's `> run`, `load`, and
 * `file` directives to drag in every dependency (scripts, textures, meshes,
 * light probes).  The closure is synced into
 * `${buildDir}/generated/rise-assets/rise/...` and packaged with the APK.
 *
 * A SHA-256 fingerprint of the synced files is pushed through
 * `BuildConfig.RISE_ASSETS_FINGERPRINT` so [com.risegfx.android.assets.AssetExtractor]
 * re-extracts on first launch after a content change — no versionCode bump
 * required for routine scene edits.
 *
 * To bundle a different scene:
 *   1. Add a [SceneEntry] below.  The `relativePath` must point at a real
 *      file under the repo root; the build fails fast if not.
 *   2. Rebuild — Gradle picks up the new dependencies automatically.
 *
 * Caveat: scenes whose `file_rasterizeroutput` pattern writes to a nested
 * subdirectory like "rendered/foo/bar" still need that directory to exist
 * at runtime.  Add it to `kRenderedDirs` in cpp/RiseBridge.cpp::initialize().
 * Plain `rendered/<sceneName>` patterns only need the top-level "rendered"
 * directory, which is always created.
 */
data class SceneEntry(
    val displayName: String,
    val relativePath: String,
    val description: String,
)

object SceneCatalog {
    val bundled: List<SceneEntry> = listOf(
        SceneEntry(
            displayName = "PT Jewel Vault",
            relativePath = "scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene",
            description = "Path-guiding showcase: an enclosed gallery lit only through a narrow slot window. Tests deep indirect bounces against gold/copper/bronze metallics and amber/sapphire glass.",
        ),
        SceneEntry(
            displayName = "Kaleidoscope Atrium",
            relativePath = "scenes/FeatureBased/Parser/kaleidoscope_atrium.RISEscene",
            description = "Parser-generated mirrored atrium — recursive reflections without external assets.",
        ),
        SceneEntry(
            displayName = "MLT Veach Egg (VCM displaced)",
            relativePath = "scenes/FeatureBased/MLT/mlt_veach_egg_vcm_displaced.RISEscene",
            description = "Metropolis Light Transport on the displaced Veach-egg: caustics through a heavily-deformed dielectric shell. Slow, rewarding.",
        ),
        SceneEntry(
            displayName = "GGX Showcase",
            relativePath = "scenes/FeatureBased/Materials/ggx_showcase.RISEscene",
            description = "GGX microfacet sweep across roughness and metallic parameters.",
        ),
        SceneEntry(
            displayName = "Tidepools",
            relativePath = "scenes/FeatureBased/Combined/tidepools.RISEscene",
            description = "Procedural cracked-mud tidepool surface lit by the Uffizi HDR light probe. Combines voronoi painters, dielectric water, and image-based lighting.",
        ),
        SceneEntry(
            displayName = "Pool Caustics (VCM)",
            relativePath = "scenes/Tests/VCM/pool_caustics_vcm.RISEscene",
            description = "Vertex-Connection-and-Merging stress test: refractive caustics on a pool floor.",
        ),
        SceneEntry(
            displayName = "Diacaustic (VCM)",
            relativePath = "scenes/Tests/VCM/diacaustic_vcm.RISEscene",
            description = "Mirror-ring diacaustic — VCM merges resolve the bright reflection caustic that PT misses.",
        ),
    )

    /** Resolve a bundled entry to an absolute path under the RISE root. */
    fun absolutePath(riseRoot: File, entry: SceneEntry): String =
        File(riseRoot, entry.relativePath).absolutePath
}
