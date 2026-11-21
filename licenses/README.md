# Third-party licences

RISE's own licence is in [LICENSE.TXT](../LICENSE.TXT). Some source files
contain code or data from other projects. Each such file names its
sources in its header comment, and the files here are the full licence
texts and notices. One ported file cannot be covered by a notice at all;
see [Not covered by these notices](#not-covered-by-these-notices).

| File | Covers |
|---|---|
| [Cycles-NOTICE.txt](Cycles-NOTICE.txt) | The copyright notices for the code ported from Blender's Cycles renderer, which licence covers which part, and what was modified: `src/Library/Utilities/OslNoise.h` and `OslNoise.cpp`. |
| [Apache-2.0.txt](Apache-2.0.txt) | Cycles' fractal noise, Noise Texture node and hashes (Blender Foundation). On Android it also covers the Kotlin, kotlinx.coroutines, AndroidX and Compose libraries. |
| [BSD-3-Clause.txt](BSD-3-Clause.txt) | The Perlin noise core Cycles adapted from Open Shading Language (Sony Pictures Imageworks Inc., et al.). |
| [Sobol-Joe-Kuo-LICENSE.txt](Sobol-Joe-Kuo-LICENSE.txt) | The Sobol' direction numbers in `src/Library/Sampling/SobolDirectionNumbers.cpp` (Frances Y. Kuo and Stephen Joe). |
| [Unicode-LICENSE.txt](Unicode-LICENSE.txt) | The Unicode NFC tables in `src/Library/Utilities/UnicodeNFCData.inc`, generated from Unicode 17.0.0 data (Unicode License v3). |
| [zlib-LICENSE.txt](zlib-LICENSE.txt) | The vendored zlib in `extlib/zlib`, whose notice lives only in `zlib.h`. |

Other notices stay next to what they cover: `extlib/{stb,cgltf,mt19937,libpng}/LICENSE*`
for the vendored libraries, `build/XCode/rise/RISE-GUI/Fonts/OFL-LICENSE.txt` and
`build/VS2022/RISE-GUI/fonts/OFL-LICENSE.txt` for the IBM Plex fonts
both GUIs embed, and `build/VS2022/RISE-GUI/icons/LICENSE-lucide.txt` for
the Lucide icons the Windows GUI embeds.

The lookup3 hash in `OslNoise.cpp` is Bob Jenkins's public-domain code.
The sky model in `src/Library/Utilities/HosekWilkieSkyModel.*` implements
Preetham et al. 1999 from the paper (Hosek-Wilkie's CC-BY coefficient
tables are not used); its header explains why it carries no notice.

## What each distribution ships

[third_party_notices.tsv](third_party_notices.tsv) lists every notice
that lives in this repository and which distributions carry it. All four
packagers read it, and each **fails** if a notice it lists is missing.
Libraries that come from outside the repository are found by the
packager itself, and a missing licence there fails too.

| Distribution | Where the notices go | From the manifest | Found at packaging time |
|---|---|---|---|
| macOS app (`scripts/create_macos_release.sh`) | `RISE-GUI.app/Contents/Resources/Third-Party Licenses/<package>/`; RISE's licence as `Resources/RISE-LICENSE.txt` and `LICENSE.txt` in the DMG | stb, cgltf, mt19937, Sobol', Cycles, Unicode, IBM Plex | every bundled dylib (Homebrew formulae, OIDN and its oneTBB, OpenPGL), via `copy_dependency_license` |
| Android app (`android/app/build.gradle.kts`, task `stageThirdPartyNotices`) | APK asset folder `licenses/<package>/` plus `licenses/THIRD-PARTY-NOTICES.txt`, which the app shows under **Licences** in the scene bar | RISE, stb, cgltf, mt19937, Sobol', Cycles, Unicode, libpng, zlib, Apache-2.0 | the NDK's LLVM toolchain `NOTICE` (libc++ in `libc++_shared.so` and the C++ runtime linked into `librise_jni.so`) |
| Windows (`build/VS2022`, RISE-CLI and RISE-GUI post-build, `tools/third_party_notices.py`) | `Third-Party Licenses\<package>\` next to the executables in `bin\` (`dbin\` for Debug), with an index `README.txt` | RISE, stb, cgltf, mt19937, Sobol', Cycles, Unicode, IBM Plex, Lucide | every vcpkg port's `copyright` (Qt and its dependencies, OpenEXR / Imath, libpng, zlib, FFmpeg, x265, OpenSSL, ...); OIDN from `extlib\oidn\install` (its Apache licence and third-party lists, including oneTBB's); OpenPGL from `$(RiseOpenPglRoot)` (default `C:\Dev\openpgl-install`). The step then checks every DLL in the output folder and its subfolders, by file name, against these sources and fails on any it cannot account for; the Microsoft C/C++ runtime needs no notice, and oneTBB's DLLs are covered by OIDN's oneTBB list, or by `%TBB_ROOT%`'s licence when that list is absent |
| Blender add-on (`tools/package_blender_addon.py`) | `rise_renderer/third_party_licenses/<package>/` inside the zip, beside `rise_renderer/native/<bridge>` | RISE, stb, cgltf, mt19937, Sobol', Cycles, Unicode | the licence of every library the bridge links (libpng, libtiff, OpenEXR / Imath, OIDN, OpenPGL, OpenVDB, oneTBB), found from its install prefix; pass `--license-root NAME=DIR` where none is found (OpenVDB from a Blender library tree has none) |

Android needs no OIDN, OpenPGL, OpenEXR or TIFF notices: it builds with
none of them.

The Windows build has no installer; its distribution is the `bin\`
folder. vcpkg's MSBuild integration copies the vcpkg DLLs there
(app-local deployment, on by default), and `extlib\oidn\build.ps1`
copies the OIDN DLLs and the oneTBB `tbb12.dll` / `tbbbind*.dll` there.
A DLL copied into `bin\` by hand passes the check if a DLL of the same
name is in one of those installs (an `openpgl.dll` from the OpenPGL
install, say); one that differs from a known DLL only in its version,
typically left over from an older vcpkg, passes with a warning; anything
else stops the build until it is deleted or its install is added with
`/p:RiseExtraNoticeArgs="--license-root NAME=DIR"`. Visual Studio's up-to-date check skips the
post-build step when nothing relinks, so after changing only the
manifest, rebuild RISE-CLI or RISE-GUI (command-line msbuild runs it on
every build). It needs `python` on `PATH`, as the Library project's
build-identity step already does. `RiseOpenPglRoot` must name the same
install as the hard-coded OpenPGL include and library paths.

The Blender packager runs on macOS and Linux (it reads the bridge's
dependencies with `otool` / `ldd`; the bridge Makefile builds nowhere
else). The zip contains the add-on and the bridge only. The bridge
finds its libraries at the absolute paths it was linked against, so the
zip installs on machines with the same library layout; the script prints
those paths.

```sh
make -C src/Blender/native
python3 tools/package_blender_addon.py     # -> dist/rise_renderer-<version>-<os>-<arch>.zip
```

## Adding third-party code

Add a line to `third_party_notices.tsv` for each distribution that
carries it, and put the licence file here if the code does not come with
one. `python3 tools/test_third_party_notices.py` checks the manifest,
the stager, the macOS loop, the Windows post-build wiring and the add-on
zip. It fails, without a matching entry, when the make Filelist (which the
Xcode project mirrors), the Android CMake lists or the VS2022 Library
project compile a source file from an `extlib/` directory; when a C/C++
or Swift source file in the library, the CLI, the GUIs, the Blender bridge
or the Android JNI code carries a third-party copyright or licence line;
or when a GUI source tree holds a licence file. Header-only code, code
ported without its notice, a file added to the Xcode project only, and
bundled media (below) are invisible to it.

## Not covered by these notices

Shipping notices satisfies the attribution conditions of the permissive
licences. It does not settle these:

- **The OQS quartic solver** in `src/Library/Functions/Polynomial.cpp`
  is ported from the reference code of Orellana and De Michele, ACM TOMS
  Algorithm 1010 (`github.com/cridemichel/quartic_C`). That repository
  has no licence file. Its README grants copying only for personal or
  classroom use, not for profit or commercial advantage, and requires
  prior permission to redistribute. Every RISE binary compiles it. It
  needs the author's permission, or a reimplementation from the paper.
- **Qt 6** (vcpkg `qtbase`, `qtsvg`) is LGPL-3.0 when used under the
  open-source licence. RISE-GUI loads it as DLLs, so it can be replaced,
  but a binary release must also offer the Qt source.
- **FFmpeg**: vcpkg builds it with the `gpl` and `x265` features
  (`build/VS2022/vcpkg.json`), which makes those DLLs GPL. A RISE-GUI
  binary distributed with them is subject to the GPL. Dropping the two
  features (LGPL FFmpeg, no x265 encoder) avoids that.
- **OpenVDB** is MPL-2.0. A binary that contains it, as the Blender
  bridge does through OpenVDB's inline header code, must also tell
  recipients where to get its source.
- **Not yet audited** (none carries a licence statement):
  - code described as taken from ggLibrary (`ThinLensCamera.cpp`,
    `RayCylinderIntersection.cpp`, `Material.h`, `RuntimeContext.h`, and
    part of the RLE decoding in `HDRReader.cpp`);
  - photon-map code based on the sample code in Jensen's book
    (`PhotonMapping/`);
  - the SMS code adapted from Zeltner et al.'s reference implementation
    (`ManifoldSolver.cpp`);
  - `Polynomial::bessi0`, "from Numerical Recipes in C", whose licence
    restricts redistribution of its code (about ten lines evaluating
    Abramowitz-Stegun coefficients; used by the windowed-sinc filter);
  - the value noise in `src/Library/Noise/`, from Hugo Elias's Perlin
    noise tutorial;
  - Graphics Gems derivations (`Polynomial.cpp`, `TorusGeometry.cpp`,
    `RayTriangleIntersection.cpp`, `MultiJitteredSampling2D.h`); the
    Graphics Gems code licence is permissive.
- **Bundled media**: the Android APK packages the scenes in its catalogue
  with their textures and light probes, among them Paul Debevec's
  `lightprobes/uffizi_probe.hdr` and `textures/wood.png` (provenance
  unknown). Their terms are not recorded here.
