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
    "sounds/", "tables/", "media/", "colors/",
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

// ----------------------------------------------------------------------------
// Third-party licence notices
//
// librise_jni.so compiles in stb, cgltf, mt19937, the Sobol' direction
// numbers, the Cycles noise, Voronoi and colour ports, the Unicode NFC tables, libpng
// and zlib, and the APK ships the NDK's libc++_shared.so.  The in-repo notices
// are listed once, in licenses/third_party_notices.tsv (lines tagged
// "android" or "all"); the LLVM notice covering libc++ comes from the NDK the
// build uses.  The stageThirdPartyNotices task copies them into the generated
// asset folder `licenses/` -- one folder per package plus
// THIRD-PARTY-NOTICES.txt, the concatenation the app's Licences screen shows
// -- and FAILS the build if any of them is missing.
// ----------------------------------------------------------------------------
val riseNoticeManifest: File = File(riseRepoRoot, "licenses/third_party_notices.tsv")
val riseNoticesOutputDir: Provider<Directory> =
    layout.buildDirectory.dir("generated/rise-notices")

// (package, repo-relative path) for every manifest line tagged android/all.
// Read at configuration time so a malformed manifest fails the build early.
val riseManifestNotices: List<Pair<String, String>> = run {
    if (!riseNoticeManifest.isFile) {
        throw GradleException("Third-party notice manifest is missing: $riseNoticeManifest")
    }
    // Same rules as tools/third_party_notices.py read_manifest().
    val known = setOf("macos", "android", "windows", "blender")
    val packageName = Regex("^[A-Za-z0-9][A-Za-z0-9._+@-]*$")
    val seen = mutableSetOf<Pair<String, String>>()
    riseNoticeManifest.readLines().mapIndexedNotNull { index, line ->
        if (line.isBlank() || line.trimStart().startsWith("#")) return@mapIndexedNotNull null
        val where = "$riseNoticeManifest:${index + 1}"
        val fields = line.split("\t")
        if (fields.size != 3) throw GradleException("$where: expected 3 tab-separated fields")
        val pkg = fields[0].trim()
        val rel = fields[1].trim()
        if (!packageName.matches(pkg)) throw GradleException("$where: bad package name '$pkg'")
        if (rel.isEmpty() || rel.startsWith("/") || rel.replace("\\", "/").split("/").contains("..")) {
            throw GradleException("$where: path must be repo-relative: '$rel'")
        }
        if (!seen.add(pkg to rel)) throw GradleException("$where: $pkg $rel is listed twice")
        val distField = fields[2].trim()
        val dists = if (distField == "all") known else distField.split(",").map { it.trim() }.toSet()
        if (dists.isEmpty() || dists.any { it !in known }) {
            throw GradleException("$where: unknown distribution in '$distField'")
        }
        if ("android" in dists) pkg to rel else null
    }.also {
        if (it.isEmpty()) throw GradleException("$riseNoticeManifest lists no notices for android")
    }
}

val stageThirdPartyNotices = tasks.register("stageThirdPartyNotices") {
    description = "Copy the third-party licence notices the APK must carry into its assets."
    group = "rise"
    val outDir = riseNoticesOutputDir.map { it.dir("licenses") }
    inputs.file(riseNoticeManifest)
    for ((_, rel) in riseManifestNotices) {
        val f = File(riseRepoRoot, rel)
        if (f.isFile) inputs.file(f)
    }
    // libc++_shared.so and the libc++ / compiler-rt code linked into
    // librise_jni.so are LLVM's; the NDK carries their notice beside the
    // prebuilt toolchain (one host directory per install).  Resolved when
    // the task is configured so the file is a real task input.
    val ndkDir = android.ndkDirectory
    val llvmNotices = File(ndkDir, "toolchains/llvm/prebuilt")
        .listFiles()?.map { File(it, "NOTICE") }?.filter { it.isFile }.orEmpty()
    if (llvmNotices.size == 1) inputs.file(llvmNotices.single())
    outputs.dir(outDir)
    doLast {
        val notices = mutableListOf<Pair<String, File>>()
        for ((pkg, rel) in riseManifestNotices) {
            val f = File(riseRepoRoot, rel)
            if (!f.isFile) throw GradleException("Third-party notice for $pkg is missing: $f")
            notices += pkg to f
        }
        if (llvmNotices.size != 1) {
            throw GradleException(
                "Expected one LLVM toolchain NOTICE under $ndkDir/toolchains/llvm/prebuilt, " +
                "found ${llvmNotices.size}")
        }
        notices += "LLVM-libcxx" to llvmNotices.single()

        val dest = outDir.get().asFile
        dest.deleteRecursively()
        dest.mkdirs()
        val combined = StringBuilder()
        combined.append("RISE for Android -- licences and third-party notices\n\n")
        combined.append("RISE's own licence comes first, then the licence and copyright\n")
        combined.append("notices of the third-party code compiled into the app's native\n")
        combined.append("library.  The app's Kotlin, kotlinx.coroutines, AndroidX and\n")
        combined.append("Jetpack Compose libraries are distributed under the Apache\n")
        combined.append("License, Version 2.0, whose text is included below under\n")
        combined.append("Apache-2.0.\n\n")
        val seen = mutableSetOf<String>()
        for ((pkg, f) in notices) {
            val name = if (pkg == "LLVM-libcxx") "NOTICE.txt" else f.name
            val target = File(dest, "$pkg/$name")
            if (!seen.add("$pkg/$name")) {
                // Identical repeats are fine, as in tools/third_party_notices.py.
                if (!target.readBytes().contentEquals(f.readBytes())) {
                    throw GradleException("Two different $pkg notices are both named $name")
                }
                continue
            }
            target.parentFile.mkdirs()
            f.copyTo(target, overwrite = true)
            combined.append("=".repeat(72)).append("\n")
            combined.append("$pkg -- $name\n")
            combined.append("=".repeat(72)).append("\n\n")
            combined.append(f.readText(Charsets.UTF_8).trimEnd()).append("\n\n")
        }
        File(dest, "THIRD-PARTY-NOTICES.txt").writeText(combined.toString(), Charsets.UTF_8)
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
        // Third-party notices, under the asset path licenses/.
        assets.srcDir(riseNoticesOutputDir)
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
    .configureEach { dependsOn(syncRiseAssets, stageThirdPartyNotices) }

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
