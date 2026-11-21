# Third-party licences

RISE's own licence is in [LICENSE.TXT](../LICENSE.TXT). Some source files
contain code ported from other projects. Each such file names its sources
and reproduces their copyright notices in its header comment; the files
here are the full licence texts and notices. The macOS release
(`scripts/create_macos_release.sh`) copies them into the app's
`Third-Party Licenses` folder; the Android, Windows and Blender add-on
builds do not yet ship third-party notices at all (this predates the
ported code: stb, cgltf and mt19937 are not shipped there either).

| File | Covers |
|---|---|
| [Cycles-NOTICE.txt](Cycles-NOTICE.txt) | The copyright notices for the code ported from Blender's Cycles renderer, which licence covers which part, and what was modified: `src/Library/Utilities/OslNoise.h` and `OslNoise.cpp`. |
| [Apache-2.0.txt](Apache-2.0.txt) | Cycles' fractal noise, Noise Texture and White Noise nodes, and hashes (Blender Foundation). |
| [BSD-3-Clause.txt](BSD-3-Clause.txt) | The Perlin noise core Cycles adapted from Open Shading Language (Sony Pictures Imageworks Inc., et al.). |
| [Sobol-Joe-Kuo-LICENSE.txt](Sobol-Joe-Kuo-LICENSE.txt) | The Sobol' direction numbers in `src/Library/Sampling/SobolDirectionNumbers.cpp` (Frances Y. Kuo and Stephen Joe). |

The lookup3 hash in `OslNoise.cpp` is Bob Jenkins's public-domain code.
