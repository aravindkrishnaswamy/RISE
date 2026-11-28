# Cross-platform compile check (Windows and Android)

`.github/workflows/cross-platform-compile.yml` compiles the RISE library on Windows (MSVC) and Android
(NDK, arm64-v8a) so that platform breakage shows up as real compiler errors even when primary
development happens on macOS.

## Triggering it

Manual only (`workflow_dispatch`); it never runs on push.

```
gh workflow run cross-platform-compile.yml --ref <branch> -f target=both     # or windows / android
gh run list --workflow cross-platform-compile.yml --limit 3
gh run watch <run-id>
```

The workflow file must exist on the branch you pass to `--ref`. The Windows job bills at the Windows
runner rate and a cold run takes roughly one to two hours (vcpkg, then ~400 translation units); a
warm vcpkg cache saves the dependency part. A newer run on the same ref cancels an older one.

## Reading the results

* Job summary (Actions page, or `gh run view <id> --log` and look for "error summary"): the number of
  distinct error lines, the most common error codes (Windows) or files (Android), and the first 200
  distinct error lines with the workspace prefix removed.
* Artifacts `windows-compile-logs` and `android-compile-logs` (14 days): the full MSBuild / ninja /
  vcpkg logs. Only logs are uploaded, never sources or build products.
  `gh run download <id> -n windows-compile-logs`.
* The build steps keep going past the first failing file (`cl /MP` and `ninja -k 0`), run every
  stage even if an earlier one failed, and fail the job at the end if anything failed. A red job
  with a short error list is the expected outcome of a first run.

## What is built

| | Windows | Android |
|---|---|---|
| Runner | `windows-latest` | `ubuntu-latest` |
| Toolchain | MSVC from the runner's Visual Studio; toolset chosen automatically (`v143` for VS 2022, `v145` for VS 2026) and passed to MSBuild, overriding the `v145` in the projects | NDK 27.x from the runner image (else its latest NDK), CMake + Ninja, `android-29`, `c++_shared` |
| Build description | `build/VS2022/Library/Library.vcxproj`, `build/VS2022/RISE-CLI/RISE-CLI.vcxproj` (Release x64) | `build/cmake/rise-android/CMakeLists.txt`, targets `rise` (librise.a) then `rise_jni` |
| Dependencies | vcpkg: zlib, libpng, openexr (trimmed copy of `build/VS2022/vcpkg.json`, same baseline; Qt and FFmpeg are GUI-only and skipped); binary packages cached | vendored zlib and libpng from `extlib/` (as in the real Android build) |
| OIDN, OpenPGL | not built; the two `RISE_ENABLE_*` defines and their import libraries are removed from the checked-out projects (CI workspace only) | not used (same as the real Android build) |

Also compiled on Windows with `cl /c` (no link): `tools/rise_sim.cpp` and `tools/simulation_cache_import.cpp`.
There is no `rise-sim` (or `simulation_cache_import`) project in the VS2022 solution, and none was added.
The sources the Windows library project and the Android source list must contain are kept complete by
`SourceHygieneTest`, together with `build/make/rise/Filelist` and the Xcode project.

## What is not covered

* Compile only: no tests are built or run, and the Windows `RISE-CLI` link may fail for reasons
  unrelated to the new code (it is attempted anyway so link-time problems show).
* The Qt GUI (`RISE-GUI`), the Android Gradle app, Kotlin, and the Android test APK.
* Debug configuration, x86 and 32-bit builds, other Android ABIs.
* OIDN, OpenPGL, OpenVDB and FFmpeg-enabled code paths (the code behind `RISE_ENABLE_OIDN`,
  `RISE_ENABLE_OPENPGL`, `RISE_ENABLE_OPENVDB`).
* Anything the macOS-only Metal fire sources do (non-Apple builds use the `*Unsupported.cpp` twins).

## Expected problems (from reading the code, not from a compile)

Written before the Windows build was made clean (see Results); kept as the list of MSVC hazards to check in new
code. Nothing is certain. The simulation sources are already written with `_WIN32` / `__APPLE__` / Android
guards (durable file writes, process spawning, memory sizing, host name, pid, environment variables),
so no missing-include or unguarded-POSIX error was found by reading. The places most likely to fail:

Windows
* `src/Library/Simulation/SimulationProcess.cpp` (Win32 branch) and `SimulationCache.cpp` (Win32
  branch): both are marked "NOT COMPILED OR TESTED"; a Win32 type or overload mismatch is the most
  likely first error. `SimulationCache.cpp` defines its own `ReadFile` next to the Win32 function of
  the same name (the API calls are qualified with `::`, so it should resolve as an overload).
* `Liquid/*`, `Cloth/*`, `Utilities/RISECBOR64.cpp`: not inspected for MSVC-only
  rules (narrowing in braced initialisers, 16 KB string-literal limit, `near`/`far`/`min`/`max`
  identifier clashes with `windows.h`, LLP64 `long`).
* RISE-CLI link: needs `libpng16.lib;zlib.lib;` and the OpenEXR 3.4 import library names to match the
  baseline's port versions; an LNK1104 for an `OpenEXR-3_x.lib` name means the baseline ships a
  different version.
* If the runner's Visual Studio is older or newer than expected, a `MSB8020` (toolset not found)
  means the automatic toolset pick needs adjusting in the "Pick platform toolset" step.

Android
* `Utilities/FireSequenceCache.cpp`, `Simulation/SimulationProcess.cpp`: the POSIX paths are used
  (`posix_spawn`, `/proc/self/exe`, `flock`, `fsync`); all exist in bionic at API 29. The desktop-only
  importer in `FireSequenceCache.cpp` is excluded on Android.
* `Rendering/FileEncoderObserver.cpp` calls `std::to_chars` for `double`; if the NDK's libc++ lacks
  that overload this fails there (floating-point `std::from_chars` is not used anywhere).
* Fast-math flags are not an error source, but `-Wno-c++11-narrowing` in the Android flags hides
  narrowing diagnostics that MSVC reports on Windows.
* `Liquid/*`, `Cloth/*`, `RISECBOR64.cpp` (hardware SHA-256 path is Apple-only): not inspected.

Files other agents were editing when this was written (not touched): `src/Library/Simulation/Liquid/`,
`Cloth/ClothParallel.cpp`, `ClothSolver.cpp`, `ClothContact.cpp`, `SimulationCache.cpp`,
`Utilities/RISECBOR64.{h,cpp}`, `build/XCode/`.

## Results (2026-11-27, `b6663f1af`)

- **Android (arm64-v8a, NDK 27.3):** `librise.a` and `rise_jni` compile and link with 0 errors (run 2, commit
  `00e1e9455`, after fixing vendored libpng 1.2.8 compatibility in `PNGReader.cpp`). Runtime execution on a
  physical device remains separate.
- **Windows (MSVC v145, `b6663f1af`):** Full native Windows build now compiles and links cleanly in both
  `Release|x64` and `Debug|x64` across `Library` (`RISE.lib`), `RISE-CLI`, `RISE-GUI` (Qt6), and all **503/503**
  CMake `rise_all_tests` targets (`build/cmake/rise-tests`). Earlier CI failures in `GeometricUtilities.cpp`,
  `ManifoldSolver.h` (GDI `RGB` macro clash), `Win32Console.h`, `FireCase.cpp`, `FireSimulationRecords.cpp`,
  `LiquidSheets.cpp`, `AliasTable.h`, `KeyframableHelper.cpp`, and `Library.vcxproj` are all resolved.
- **After `b6663f1af` (branch `simulation-liquid`, merged 2026-11-27):** liquid inflow sources and drains, rigid
  analytic bodies with a mesh coupling surface, and fire solver version 4 (strict fire-adjacent sources, a macOS
  make/Xcode flag change only) were added on macOS and **not compiled on Windows**. They were read against
  `b6663f1af`'s fixes (no `near`/`far`/`small`/`large` identifiers, no `__attribute__`, no new standard facility
  without its header; the two new tests got explicit standard includes). The test glob picks up the two new tests,
  `LiquidSourceTest` and `ClothRigidPrimitiveTest` (505 CMake targets, not yet built on Windows).
- **Remaining platform gaps:** The Windows solution does not yet have a `rise-sim` project and the Windows
  Qt6 GUI does not yet mirror macOS `SimulationsPanel` (see [SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md)).
