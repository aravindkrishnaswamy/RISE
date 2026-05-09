import java.security.MessageDigest

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// ----------------------------------------------------------------------------
// RISE asset bundling
//
// The bundled scene catalogue is the source-of-truth list of `relativePath`
// entries in [com.risegfx.android.ui.SceneCatalog].  At configuration time we
// scrape those paths out of the Kotlin source, recursively walk each scene's
// `> run` / `load` / `file` directives to discover its dependencies (scripts,
// textures, meshes, light probes), and register a Sync task that copies the
// closure into `${buildDir}/generated/rise-assets/rise/...`.  The generated
// directory is wired into `assets.srcDirs`, so AGP packages it into the APK.
//
// Adding a scene = add a [SceneEntry] to SceneCatalog.kt.  Adding a new
// `file ...` reference inside an already-bundled scene = nothing — the next
// build picks it up automatically.  The fingerprint pushed through
// `BuildConfig.RISE_ASSETS_FINGERPRINT` is what AssetExtractor checks to
// decide whether to re-extract on first launch after install.
// ----------------------------------------------------------------------------
val riseRepoRoot: File = file("${rootDir}/..")
val sceneCatalogKt: File = file("src/main/java/com/risegfx/android/ui/SceneCatalog.kt")

// Subfolder prefixes that mark a string as a media reference rather than a
// painter name or other identifier.  Anything else `file ...` refers to is
// ignored by the dependency scanner.
val riseMediaPrefixes = listOf(
    "scenes/", "textures/", "models/", "lightprobes/",
    "sounds/", "tables/", "media/",
)

val riseDirectivePattern = Regex("""^\s*(?:>\s*run|load|file)\s+(\S+)""", RegexOption.MULTILINE)
// Anchored at the start of a (possibly indented) line so doc-comment
// occurrences inside the SceneCatalog.kt header — which use the same
// `relativePath = "..."` literal as an example — don't get scraped.
val riseRelativePathPattern = Regex("""^\s*relativePath\s*=\s*"([^"]+)"""")

fun walkRiseSceneDeps(rootDir: File, initial: Collection<String>): Set<String> {
    val collected = LinkedHashSet<String>()
    val queue = ArrayDeque(initial)
    while (queue.isNotEmpty()) {
        val rel = queue.removeFirst()
        if (!collected.add(rel)) continue
        val f = File(rootDir, rel)
        if (!f.isFile) continue
        val name = f.name
        if (!(name.endsWith(".RISEscene") || name.endsWith(".RISEscript"))) continue
        val text = f.readText()
        for (m in riseDirectivePattern.findAll(text)) {
            val ref = m.groupValues[1]
            if (riseMediaPrefixes.any { ref.startsWith(it) }) {
                queue.addLast(ref)
            }
        }
    }
    return collected
}

val bundledScenePaths: List<String> = sceneCatalogKt.readLines()
    .mapNotNull { riseRelativePathPattern.find(it)?.groupValues?.get(1) }
    .also { paths ->
        if (paths.isEmpty()) {
            throw GradleException(
                "No bundled scenes found in $sceneCatalogKt — the regex " +
                "`$riseRelativePathPattern` must match the SceneEntry literals."
            )
        }
        for (p in paths) {
            val f = File(riseRepoRoot, p)
            if (!f.isFile) {
                throw GradleException(
                    "Bundled scene $p (referenced from SceneCatalog.kt) does not exist " +
                    "under $riseRepoRoot — fix the path or remove the entry."
                )
            }
        }
    }

val riseAssetDeps: List<String> = walkRiseSceneDeps(riseRepoRoot, bundledScenePaths)
    .toList()
    .also { deps ->
        // Surface the resolved dependency closure at configure time so a
        // dropped texture / mesh shows up cleanly in the build log.
        for (rel in deps) {
            val f = File(riseRepoRoot, rel)
            if (!f.isFile) {
                throw GradleException(
                    "RISE asset $rel (referenced from a bundled scene) does not exist under " +
                    "$riseRepoRoot."
                )
            }
        }
    }

// SHA-256 over (relPath, content) for every synced file.  Stable across
// platforms since the repo paths are POSIX-style and contents are bytes.
// Truncated to 16 hex chars — enough collision resistance for a
// drop-staleness signal.
val riseAssetsFingerprint: String = run {
    val md = MessageDigest.getInstance("SHA-256")
    for (rel in riseAssetDeps.sorted()) {
        md.update(rel.toByteArray(Charsets.UTF_8))
        md.update(0)
        md.update(File(riseRepoRoot, rel).readBytes())
        md.update(0)
    }
    md.digest().joinToString("") { "%02x".format(it) }.take(16)
}

val riseAssetsOutputDir: Provider<Directory> =
    layout.buildDirectory.dir("generated/rise-assets")

val syncRiseAssets = tasks.register<Sync>("syncRiseAssets") {
    description = "Copy bundled RISE scenes plus transitive media dependencies into the APK assets bundle."
    group = "rise"
    into(riseAssetsOutputDir.map { it.dir("rise") })
    from(riseRepoRoot) {
        for (rel in riseAssetDeps) include(rel)
        // Anything not on the include list is dropped — the Sync task also
        // deletes stale files left behind from a previous run.
    }
}

android {
    namespace = "com.risegfx.android"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.risegfx.android"
        minSdk = 29
        targetSdk = 35
        versionCode = 3
        versionName = "0.3.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Surface the bundled-asset fingerprint so AssetExtractor can detect
        // a content change between APK installs without requiring a
        // versionCode bump on every scene tweak.
        buildConfigField("String", "RISE_ASSETS_FINGERPRINT", "\"$riseAssetsFingerprint\"")

        ndk {
            // arm64-v8a only for v1. Paired with the ARM64 Android 14 emulator
            // image the plan assumes. Adding x86_64 is a one-line change here
            // if/when host-architecture coverage is needed.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    // Pass the path to our JNI C++ sources down to the shared
                    // CMake config so librise_jni.so gets built as part of the
                    // rise_android project.
                    "-DRISE_JNI_SOURCES_DIR=${projectDir.absolutePath.replace("\\", "/")}/src/main/cpp"
                )
                cppFlags += "-std=c++17"
            }
        }
    }

    externalNativeBuild {
        cmake {
            // The CMakeLists lives outside the Android module on purpose —
            // it's reused for both the Android build and the standalone
            // library smoke test described in build/cmake/rise-android/toolchain_notes.md.
            path = file("${rootDir}/../build/cmake/rise-android/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets.named("main") {
        // Layer the Sync output beneath the in-tree assets dir so generated
        // RISE scenes/textures/meshes are packaged like any other asset.
        assets.srcDir(riseAssetsOutputDir)
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            isJniDebuggable = true
            packaging {
                jniLibs.keepDebugSymbols += "**/*.so"
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

// Run the sync ahead of every variant's mergeAssets so the generated tree
// is in place before AGP packages the APK.  `tasks.matching` is the
// lifecycle-safe form: AGP's per-variant `mergeXxxAssets` tasks are
// registered after this script's evaluate phase, so `tasks.named` would
// fail eagerly here.  `configureEach` defers the wiring until the task is
// actually realised.
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(syncRiseAssets) }

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)
    implementation(libs.androidx.material3.window.size)
    implementation(libs.androidx.window)
    implementation(libs.kotlinx.coroutines.android)

    debugImplementation(libs.androidx.ui.tooling)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)

    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
}
