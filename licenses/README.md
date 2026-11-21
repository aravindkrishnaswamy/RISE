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

Other notices stay next to what they cover: `extlib/*/LICENSE*` for the
vendored libraries, `build/XCode/rise/RISE-GUI/Fonts/OFL-LICENSE.txt` and
`build/VS2022/RISE-GUI/fonts/OFL-LICENSE.txt` for the IBM Plex fonts
both GUIs embed, and `build/VS2022/RISE-GUI/icons/LICENSE-lucide.txt` for
the Lucide icons the Windows GUI embeds.

The lookup3 hash in `OslNoise.cpp` is Bob Jenkins's public-domain code.
The Hosek-Wilkie sky model (`src/Library/Utilities/HosekWilkieSkyModel.*`)
is a reimplementation from the published paper; its header explains why
it carries no third-party notice.

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
| Windows (`build/VS2022`, RISE-CLI and RISE-GUI post-build, `tools/third_party_notices.py`) | `Third-Party Licenses\<package>\` next to the executables in `bin\` (`dbin\` for Debug), with an index `README.txt` | RISE, stb, cgltf, mt19937, Sobol', Cycles, Unicode, IBM Plex, Lucide | every vcpkg port's `copyright` (Qt and its dependencies, OpenEXR / Imath, libpng, zlib, FFmpeg, x265, OpenSSL, ...); OIDN from `extlib\oidn\install`, whose `doc\` also covers oneTBB; OpenPGL from `$(RiseOpenPglRoot)` (default `C:\Dev\openpgl-install`) |
| Blender add-on (`tools/package_blender_addon.py`) | `rise_renderer/third_party_licenses/<package>/` inside the zip, beside `rise_renderer/native/<bridge>` | RISE, stb, cgltf, mt19937, Sobol', Cycles, Unicode | the licence of every library the bridge links (libpng, libtiff, OpenEXR / Imath, OIDN, OpenPGL, OpenVDB, oneTBB), found from its install prefix; pass `--license-root NAME=DIR` where none is found (OpenVDB from a Blender library tree has none) |

Android needs no OIDN, OpenPGL, OpenEXR or TIFF notices: it builds with
none of them.

The Windows build has no installer; its distribution is the `bin\`
folder. vcpkg's MSBuild integration copies the vcpkg DLLs there
(app-local deployment, on by default), and `extlib\oidn\build.ps1`
copies the OIDN DLLs and the oneTBB `tbb12.dll` / `tbbbind*.dll` there.
The post-build step runs when an executable relinks: after changing only
the manifest, rebuild RISE-CLI or RISE-GUI to refresh the folder.

The Blender zip contains the add-on and the bridge only. The bridge
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
zip. It fails when a build compiles a source file from an `extlib/`
directory, when a source file carries a third-party copyright or licence
line, or when a GUI bundle holds a licence file, without a matching
entry. Header-only code and code ported without its notice are invisible
to it.

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
- **Not yet audited**: code described as taken from ggLibrary
  (`ThinLensCamera.cpp`, `RayCylinderIntersection.cpp`, `Material.h`),
  photon-map code based on Jensen's book (`PhotonMapping/`), and the SMS
  code adapted from Zeltner et al.'s reference implementation
  (`ManifoldSolver.cpp`). None of them carries a licence statement.
