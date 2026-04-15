# RISE Android NDK build notes

This directory contains the CMake build config used by the Android Gradle
project at `../../../android/`. Nothing under `src/Library/**` is modified by
this build — all Android-specific adaptation is here or in the JNI bridge
under `android/app/src/main/cpp/`.

## Prerequisites

- **Android Studio** (Hedgehog 2023.1.1 or newer). When the project is first
  opened, Android Studio will prompt to install the matching NDK and CMake
  from the SDK Manager. Accept both.
- **NDK version**: r26d or newer (any r26.x or r27.x works). CMake 3.22.1+.
- **ABI**: `arm64-v8a` only. Emulator: use an ARM64 Android 14 (API 34) system
  image. On an Intel host, enable hardware-accelerated ARM translation in AVD
  Manager — Android Studio will warn if unavailable.

## Standalone library smoke test

Before running anything through Gradle, you can validate that `librise.a`
compiles cleanly under the NDK toolchain by itself. This is step 1 of the
implementation plan and has no dependencies on the Kotlin layer.

From this directory:

```sh
export ANDROID_NDK=~/AppData/Local/Android/Sdk/ndk/26.3.11579264   # or wherever
cmake -B _out \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared
cmake --build _out --target rise -j8
```

Expected output: a `librise.a` archive under `_out/` containing ~180 object
files. Warnings are fine; errors mean the source list in `rise_sources.cmake`
or the compile flags in `CMakeLists.txt` need adjustment.

## Source list synchronization

`rise_sources.cmake` is a hand-transcribed copy of the `SRCLIB` assembly in
`../../make/rise/Filelist`. If upstream ever adds or removes a `.cpp` file
there, this copy must be updated manually. Do NOT switch to `file(GLOB)` —
doing so would silently pick up Windows-only files (`ThreadsWin32.cpp`,
`LoadLibraryWin32.cpp`, `Win32Console.cpp`, `Win32WindowRasterizerOutput.cpp`),
the C++/CLI `ManagedJob.cpp`, and the DRISE socket stack, none of which
belong in the NDK build.

## Excluded from the build

- **OpenEXR** (`extlib/openexr`): pre-C++17 vintage — uses `register` and
  dynamic exception specs. Incompatible with NDK clang + `-std=c++17`.
  Compiled out via `-DNO_EXR_SUPPORT`, matching how `Config.OSX` handles the
  "no EXR installed" case.
- **libtiff** (`extlib/libtiff`): similar vintage issues. Compiled out via
  `-DNO_TIFF_SUPPORT`.
- **OIDN** (Intel Open Image Denoise): not vendored; defer to a follow-up.
- **OpenPGL** (Intel Open Path Guiding): same.
- **libpng x86 asm files** (`pnggccrd.c`, `pngvcrd.c`): excluded in
  `extlib_png.cmake` — they don't compile for arm64 and the reference C
  fallback is used instead.

## Troubleshooting

- **`png.h: No such file`** — the Gradle build didn't pick up this CMake file.
  Check `app/build.gradle.kts` has
  `externalNativeBuild.cmake.path = ../../build/cmake/rise-android/CMakeLists.txt`
  relative to the android module root.
- **`undefined reference to pthread_create`** — the NDK toolchain provides
  libpthread implicitly for API 21+, so this shouldn't happen. If it does,
  you're probably on a non-Android toolchain; re-check `CMAKE_TOOLCHAIN_FILE`.
- **`cannot find -lOpenImageDenoise`** — the library source includes
  `OIDNDenoiser.cpp` but the real OIDN dep is guarded by
  `#ifdef RISE_ENABLE_OIDN`. As long as you do NOT define `RISE_ENABLE_OIDN`
  (and we don't here), the TU compiles to an empty no-op.
