# RISE Android app

Native Android front-end for the RISE ray tracer, structured the same way
as the macOS and Windows apps: the library (`src/Library/`) is unchanged and
statically linked; a thin JNI bridge adapts it to Kotlin + Jetpack Compose.

See `C:\Users\micro\.claude\plans\fancy-sprouting-peacock.md` for the full
design document, or `../build/cmake/rise-android/toolchain_notes.md` for
NDK-specific build notes.

## Requirements

- **Android Studio Hedgehog (2023.1.1) or newer**. On first open, Android
  Studio will offer to install the matching NDK (r26.3+) and CMake (3.22.1+)
  through the SDK Manager — accept both prompts.
- **JDK 17** (bundled with Android Studio).
- An **arm64-v8a** Android 14 (API 34) emulator image. On an Apple Silicon
  host the ARM image is hardware-accelerated; on Intel hosts it runs under
  translation (slower but usable for the smoke test).

## Opening the project

1. In Android Studio: *File → Open* → select the `android/` directory.
2. Wait for Gradle sync. First sync will download AGP 8.5.2 and the Compose
   dependencies, then invoke CMake against `../build/cmake/rise-android/
   CMakeLists.txt` and compile ~180 library TUs. Expect the first build to
   take several minutes; incremental builds are fast.
3. Select the `app` run configuration and an arm64 emulator, then Run.

The first launch extracts bundled scenes (`scenes/Tests/Geometry/shapes.RISEscene`
and its `colors.RISEscript` dependencies) from the APK to `filesDir/rise/`.
Subsequent launches skip extraction if `BuildConfig.VERSION_CODE` is
unchanged.

## Gradle wrapper

The `gradle/wrapper/gradle-wrapper.properties` file points at Gradle 8.9,
but the `gradle-wrapper.jar` is NOT committed — Android Studio will generate
it automatically on first sync. If you prefer command-line builds, run
`gradle wrapper` once in this directory (requires a system Gradle install)
and then use `./gradlew assembleDebug` thereafter.

## Running the instrumented test

```
./gradlew connectedDebugAndroidTest
```

`RenderSmokeTest.renderShapesProducesNonZeroFrame` loads shapes.RISEscene,
calls `nativeRasterize()`, and asserts that the output framebuffer has at
least one non-zero pixel. Timeout is 5 minutes.

## Running host-side tests

```
./gradlew testDebugUnitTest
```

Only `DirtyRectTest` at the moment — pure JVM, no emulator needed.

## Layout

```
android/
  settings.gradle.kts                  root project
  build.gradle.kts                     plugin aliases
  gradle/libs.versions.toml            version catalog
  app/
    build.gradle.kts                   app module, NDK config
    src/
      main/
        AndroidManifest.xml
        java/com/risegfx/android/
          RiseApplication.kt           onCreate → extract + nativeInit
          MainActivity.kt              Compose host
          assets/AssetExtractor.kt     APK → filesDir copy
          nativebridge/
            RiseNative.kt              external fun declarations
            RiseCallback.kt            progress / tile / log interface
          ui/
            RenderScreen.kt            adaptive two-pane layout
            RenderViewModel.kt         state orchestration
            RenderState.kt             sealed UI state
            SceneCatalog.kt            bundled scene list
            theme/Theme.kt
        cpp/
          rise_jni.cpp                 JNIEXPORT entry points, JNI_OnLoad
          RiseBridge.h/.cpp            owns IJobPriv*, framebuffer, init
          RiseCallbacks.h/.cpp         IProgressCallback + IJobRasterizerOutput adapters
          RiseLogPrinter.h/.cpp        ILogPrinter → Logcat + Kotlin
          jni_util.h/.cpp              JNIEnv attach-as-daemon helper
        assets/rise/                   bundled scene tree (extracted at runtime)
      androidTest/java/.../RenderSmokeTest.kt
      test/java/.../DirtyRectTest.kt
```

## Hot topics

### Why are there no gradle-wrapper.jar, local.properties or settings.gradle 'gradle.xml'?

Those are environment-specific artifacts that Android Studio regenerates on
first open. Committing them causes more pain than they save.

### Changing the thread count

`RiseApplication.defaultThreadCount()` picks `Runtime.availableProcessors() - 1`
clamped to `[2, 6]`. It's written into
`filesDir/rise/global.options` on first launch and read by the library
through `GlobalOptions()` on the first `Rasterize()` call. The value is
cached by the library singleton — changing it mid-session does nothing; kill
the app (swipe from Recents) and relaunch to apply.

### Why only `arm64-v8a`?

Chosen in the plan. Intel hosts can still run the ARM emulator image —
slower, but fine for the smoke test. Adding `x86_64` is a one-line change
in `app/build.gradle.kts` → `ndk { abiFilters += "x86_64" }`.

### OpenEXR / TIFF support

Disabled via `-DNO_EXR_SUPPORT -DNO_TIFF_SUPPORT` in the shared CMake
config. The vendored `extlib/openexr` is pre-C++17 vintage and fails under
NDK clang; see `../build/cmake/rise-android/toolchain_notes.md` for the
plan to add OpenEXR 3.x via `FetchContent` in a follow-up.
