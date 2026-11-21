# Third-party licences

RISE's own licence is in [LICENSE.TXT](../LICENSE.TXT). Some source files
contain code ported from other projects. Each such file names its sources
and reproduces their copyright notices in its header comment; the files
here are the full licence texts and notices.

| File | Covers |
|---|---|
| [Cycles-NOTICE.txt](Cycles-NOTICE.txt) | The copyright notices for the code ported from Blender's Cycles renderer, which licence covers which part, and what was modified: `src/Library/Utilities/OslNoise.h` and `OslNoise.cpp`. |
| [Apache-2.0.txt](Apache-2.0.txt) | Cycles' fractal noise, Noise Texture node and hashes (Blender Foundation). |
| [BSD-3-Clause.txt](BSD-3-Clause.txt) | The Perlin noise core Cycles adapted from Open Shading Language (Sony Pictures Imageworks Inc., et al.). |
| [Sobol-Joe-Kuo-LICENSE.txt](Sobol-Joe-Kuo-LICENSE.txt) | The Sobol' direction numbers in `src/Library/Sampling/SobolDirectionNumbers.cpp` (Frances Y. Kuo and Stephen Joe). |
| [zlib-LICENSE.txt](zlib-LICENSE.txt) | The vendored zlib in `extlib/zlib`, whose notice lives only in `zlib.h`. |

The lookup3 hash in `OslNoise.cpp` is Bob Jenkins's public-domain code.

## What each distribution ships

[third_party_notices.tsv](third_party_notices.tsv) lists every notice
that lives in this repository and which distributions carry it. All four
packagers read it, and each **fails** if a notice it lists is missing.
Libraries that come from outside the repository are found by the
packager itself, and a missing licence there fails too.

| Distribution | Where the notices go | From the manifest | Found at packaging time |
|---|---|---|---|
| macOS app (`scripts/create_macos_release.sh`) | `RISE-GUI.app/Contents/Resources/Third-Party Licenses/<package>/`; RISE's licence as `Resources/RISE-LICENSE.txt` and `LICENSE.txt` in the DMG | stb, cgltf, mt19937, Sobol', Cycles | every bundled dylib (Homebrew formulae, OIDN and its oneTBB, OpenPGL), via `copy_dependency_license` |
| Android app (`android/app/build.gradle.kts`, task `stageThirdPartyNotices`) | APK asset folder `licenses/<package>/` plus `licenses/THIRD-PARTY-NOTICES.txt`, which the app shows under **Licences** in the scene bar | RISE, stb, cgltf, mt19937, Sobol', Cycles, libpng, zlib | the NDK's LLVM toolchain `NOTICE` (libc++ in `libc++_shared.so` and the C++ runtime linked into `librise_jni.so`) |
| Windows (`build/VS2022`, RISE-CLI and RISE-GUI post-build, `tools/third_party_notices.py`) | `Third-Party Licenses\<package>\` next to the executables in `bin\` (`dbin\` for Debug), with an index `README.txt` | RISE, stb, cgltf, mt19937, Sobol', Cycles | every vcpkg port's `copyright` (Qt and its dependencies, OpenEXR / Imath, libpng, zlib, FFmpeg, x265, OpenSSL, ...); OIDN from `extlib\oidn\install`; OpenPGL from `$(RiseOpenPglRoot)` (default `C:\Dev\openpgl-install`) |
| Blender add-on (`tools/package_blender_addon.py`) | `rise_renderer/third_party_licenses/<package>/` inside the zip, beside `rise_renderer/native/<bridge>` | RISE, stb, cgltf, mt19937, Sobol', Cycles | none: the zip contains only the add-on and the bridge, and the bridge links its libraries dynamically from the build machine (the script lists them) |

Android needs no OIDN, OpenPGL, OpenEXR or TIFF notices: it builds with
none of them. Its Kotlin, kotlinx.coroutines, AndroidX and Compose
libraries are Apache-2.0; the Apache text is included and the notice file
says so.

To package the Blender add-on:

```sh
make -C src/Blender/native
python3 tools/package_blender_addon.py     # -> dist/rise_renderer-<version>-<os>-<arch>.zip
```

## Adding third-party code

Add a line to `third_party_notices.tsv` for each distribution that
compiles it, and put the licence file here if the code does not carry
one. `python3 tools/test_third_party_notices.py` checks the manifest,
the stager, the macOS loop, the Windows post-build wiring and the add-on
zip, and fails if a build compiles an `extlib/` directory that has no
notice for that build.

## Not covered by these notices

Shipping notices satisfies the attribution conditions of the permissive
licences. It does not satisfy the source-availability conditions of
copyleft code, which the Windows build links:

- Qt 6 (vcpkg `qtbase`, `qtsvg`) is LGPL-3.0 when used under the open
  source licence; RISE-GUI loads it as DLLs, so it can be replaced, but a
  binary release must also offer the Qt source.
- vcpkg's FFmpeg is built with the `gpl` and `x265` features
  (`build/VS2022/vcpkg.json`), which makes those DLLs GPL. A RISE-GUI
  binary distributed with them is subject to the GPL. Dropping the two
  features (LGPL FFmpeg, no x265 encoder) avoids that.

The Windows build has no installer; its distribution is the `bin\`
folder, and the vcpkg DLLs land there through vcpkg's app-local
deployment (on by default in its MSBuild integration). The OIDN and OpenPGL DLLs are not copied there by the build;
whoever packages a Windows release has to add them (their notices already
are).
