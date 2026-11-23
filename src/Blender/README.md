# Blender Renderer Sidecar

This directory contains a Blender renderer integration for RISE that lives entirely outside the core library.

The structure mirrors the host-bridge style of
[`src/3DSMax`](../3DSMax): Blender-specific code stays in `src/Blender`, while
the bridge talks to RISE only through the existing public APIs in
[`src/Library/RISE_API.h`](../Library/RISE_API.h) and
[`src/Library/Interfaces/IJob.h`](../Library/Interfaces/IJob.h).

## Layout

- `addons/rise_renderer/`: Blender Python add-on and `RenderEngine` registration.
- `native/`: Small shared-library bridge that feeds Blender scene data into a RISE `IJobPriv` and returns RGBA pixels back to Blender.

Image dimensions in the native ABI are 32-bit integers. RGBA channel counts,
byte sizes, and offsets use checked native `size_t` arithmetic, including preview
regions; there is no separate 32-bit channel-count limit. Images that cannot fit
the host's native storage or container limits are rejected, and allocation or
callback exceptions become a failed render with an error message at the C boundary.

## Supported Scope

- Final renders through Blender's external `RenderEngine` API.
- Evaluated mesh export, per-material triangle splits, object instancing, image-driven tangent-space normal-map modifiers, and bump-map modifiers (a Blender Bump node exports through the ABI-frozen `AddBumpMapModifier` entry point, which since 2026-09-06 registers a `relief_modifier` in the UV domain -- see `docs/RELIEF_MODIFIER_DESIGN.md` §7.5).
- Direct Principled BSDF translation for base color, metallic, roughness, specular (IOR level and tint, DL-151), transmission, IOR, emission, anisotropy, sheen (`fabric_material`, DL-18), coat (`coated_material`, DL-186), subsurface (`randomwalk_sss_material`, DL-186 -- a SEPARATE model, not a coat-style wrap; see `docs/BLENDER_MATERIAL_TRANSLATION.md` "Coat and Subsurface"), thin film (Blender 4.2+, ABI v18: a `ggx_material` `thinfilm` base over the opaque material -- a metal substrate uses Cycles' own Gulbrandsen estimate applied per wavelength -- and a one-layer AR coating on glass), transmission dispersion (Blender 5.3 / OpenPBR, ABI v18: a Cauchy IOR curve on the dielectric -- true dispersion under the Spectral integrators, per-channel under Pel), and direct image-driven bump/normal maps.  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Thin film and dispersion".
- Blender `Curves` hair object export: evaluates curve geometry, stages temporary `.hair` strand files, and translates `Principled Hair BSDF` (direct coloring, melanin concentration, or absorption coefficient) to `hair_material` + `hair_geometry`.
- PNG, HDR, EXR, and TIFF image textures when the local RISE build has the matching texture readers enabled.
- Homogeneous participating media on material and world volume outputs.
- Heterogeneous VDB-backed volume objects driven by the `density` grid, exported through a temporary slice cache.
- Point, spot and sun lights, area lights as real emitting panels (square / rectangle / disk / ellipse meshes with a Lambertian luminaire, exitance = power / the panel's world area), Blender's `exposure`, colour temperature and "Normalize" (off: point/spot power x 4 pi r^2 or x4 at radius 0, sun x pi sin^2(angle/2), area x its area -- factors measured in Cycles 5.3).  `tools/blender_light_parity.py` compares Cycles and RISE on a white floor for 20 light cases (including non-uniform scale) with a +/-0.5% gate: all within 0.33% (point/spot) and 0.09% (sun/area) on 2026-11-14; the formulas are unit-tested in `LightMathTest`.  A genuinely sheared area light (rotated child of a non-uniformly scaled parent) keeps its stated power in RISE but not in Cycles (reported, ungated: 0.645 / 0.516).  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Lights".  The world as an environment light: an Environment Texture becomes an image-based radiance map, a uniform Background (or, on Blender 4.x, a world that does not use nodes) a uniform one. The deprecated "Approximate World" option (`use_world_ambient`, default OFF) still exports the world colour as an unshadowed `ambient_light` when enabled.
- Full integrator selector across Pel (RGB) and Spectral modes: `Path Tracing`, `BDPT`, `VCM`, and `Auto` (`auto_rasterizer` / `auto_spectral_rasterizer`), plus `[legacy]` `Pixel` (`pixelpel` / `pixelintegratingspectral`) and `[legacy]` `MLT` (`mlt` / `mlt_spectral`).
- Curated advanced ray controls for path tracing, adaptive sampling, path guiding, stability, and OIDN denoising.
- Extended SMS (`sms_extended`) is exposed as the **Extended SMS** render setting (since ABI v17); see `docs/SMS_EXTENDED_DESIGN.md` "Using extended SMS".
- Native bridge ABI/version validation (ABI v19) plus runtime capability reporting for OIDN, path guiding, and VDB volume support, render-region / Crop to Render Region support, transparent film (`film_transparent`), and native procedural `expression_painter` compilation for Voronoi / Noise / White Noise / Mix / Hue-Saturation / ColorRamp / 3D Mapping subgraphs.
- Tested with Blender 5.3 (and the 5.2 source build); 4.x remains supported.  On 5.x the add-on never reads the deprecated `Material.use_nodes` / `World.use_nodes` (pinned True since 5.0, removed in 6.0).

## Current Limitations

- No viewport renderer yet.
- No arbitrary Blender node-graph compilation; the exporter is intentionally direct-slot and GGX-first.
- Base Normal supports a direct image-driven tangent-space Normal Map (or direct image-driven Bump), exported as a normal/relief modifier. Coat Normal supports its own image-driven tangent-space map. Unsupported normal-node spaces or input chains are diagnosed; these paths do not compile arbitrary normal graphs or introduce independent secondary-UV normal decoding. New shader directions preserve the primary UV decode frame independently for base and coat; legacy noattribute/global modifier layering remains unchanged.
- No mixed opaque/transmissive per-pixel material translation yet. Alpha masking is supported (ABI v14) and reaches legacy and modern transport.
- Alpha (constant or RGBA texture, ABI v14) maps to scalar material coverage for legacy and modern integrators. CLIP is MASK; BLEND/HASHED use stochastic BLEND. See `docs/ALPHA_COVERAGE.md` and `docs/BLENDER_MATERIAL_TRANSLATION.md`.
- Coat Normal is bridged (ABI v14); Tangent has ABI v16 independent shader-direction corner payloads; primary UV normal/coat decode stays separate. Active-UV fallback is unchanged; new second-UV nodes follow Cycles inverse-transpose/projection semantics and linked graphs use a separate Cycles export-time bake. Invalid/unwritten backend buffers abort, including the measured 4.5.7 huge-world geometry limit. See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Tangent" for frame conventions, cleanup and export-time sampling limits.
- Subsurface uses a documented single-scattering-albedo APPROXIMATION (Radius/Scale/Base-Colour -> sigma_a/sigma_s), not Blender's/PBRT's own photon-beam-diffusion (Christensen-Burley) fit -- the right order of magnitude and per-channel colour bias, not a photometric match to Cycles.  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Subsurface -> `randomwalk_sss_material`" (DL-186).
- Thin film and dispersion read their socket defaults (a linked input is warned about); a film needs one substrate, so textured or partial Metallic resolves to the nearer of dielectric / metal (warned) and Anisotropic is dropped under a film (warned).  As in Cycles, Specular IOR Level sets the index a dielectric film sits on; Specular Tint is clamped to 1 and applied uniformly, where Cycles fades it toward grazing.  A metal under a film uses Cycles' Gulbrandsen n, k but RISE's exact conductor Fresnel, so it matches Cycles at normal incidence only.  Under the Pel integrators the film integrates a per-wavelength reflectance back to RGB: a saturated red metal under a vanishing film reads +14.7% on its dim green channel (grey 0.02%, gold 3.3%).  Principled Diffuse Roughness (4.3) and Thin Wall (5.3) have no RISE equivalent and are warned about and ignored.
- Procedural `ShaderNodeTexVoronoi`, `ShaderNodeTexNoise`, and `ShaderNodeTexWhiteNoise` (plus 3D Mapping, Mix, Hue/Saturation, ColorRamp RGB linear/constant, Invert/BrightContrast/Gamma, and Separate/Combine XYZ/Color) compile natively to RISE `expression_painter` on 12 Principled colour/scalar sockets (ABI v19).  `ShaderNodeTexChecker`, `ShaderNodeMath`, image-mixed procedural chains, and untranslated sockets (such as Bump or Alpha) still warn and fall back to socket defaults; use **Bake Procedural Materials** if you want Cycles to bake them to PNGs.
- Without a thin film, Specular Tint (like Specular IOR Level) has no effect at `metallic=1` (with a film, the tint feeds the metal substrate's n, k) -- RISE's F0 formula routes both through the dielectric branch of its base_color/F0 lerp only, so it does not reproduce Blender 4.x Principled's additional metallic-edge (F82-style) tint from the same socket.  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Specular Tint" (DL-151).
- Lights are not hidden from camera rays: Cycles' light objects default to `visible_camera = False`, but a RISE area-light panel is an ordinary emitting object and shows up in frame (RISE object visibility is all-or-nothing).  Area-light Spread below 180 degrees is ignored (warned); point/spot Radius and the sun's Angle only enter through the Normalize-off power -- the lights themselves stay deltas, so shadows are hard.
- World surface graphs are reduced to an environment light: an Environment Texture (optionally gated by Light Path > Is Camera Ray), a uniform Background colour, or, for procedural skies, a sky-tinted uniform placeholder plus a synthesised sun; bake procedural worlds to an HDR for an exact match. World volume nodes are exported as participating media.
- Heterogeneous media currently treat color and emission as uniform coefficients modulated by the exported density field.

## Build

Build the native bridge after `bin/librise.a` exists:

```sh
make -C src/Blender/native
```

That produces `rise_blender_bridge.dylib` on macOS or `rise_blender_bridge.so` on Linux in `src/Blender/native/`.

After a macOS / Xcode upgrade, rebuild the library CLEAN first
(`make -C build/make/rise clean && make -C build/make/rise -j8 all`).  The
build sets no deployment target, so objects compiled before and after the
upgrade carry different target triples (e.g. `arm64-apple-macosx26.0.0` vs
`27.2.0`); Xcode 27's `ar` then writes `bin/librise.a` as an **album** (a
container of per-triple archives, magic bytes `📚💿` instead of `!<arch>`),
and linking the bridge or any test fails with `ld: linking with album file,
but -target option was not specified`.  A clean rebuild gives every object
one triple and a normal archive.

The bridge Makefile includes OpenVDB (Blender's bundled copy, when present)
with `-isystem`: its headers use `[[assume]]`, a C++23 extension, which
would otherwise warn under `-std=c++17 -pedantic`.  That silences the
vendored headers only; the bridge's own code compiles with the inherited
`-Wall -pedantic`.

## Install In Blender

1. For active development, create a user add-on symlink that points directly at `src/Blender/addons/rise_renderer`. On macOS that user path is `~/Library/Application Support/Blender/<version>/scripts/addons/rise_renderer` (e.g. `5.3`); each Blender version has its own scripts directory.
2. Build the bridge in `src/Blender/native/`. When the add-on is loaded from the source tree, it auto-discovers `src/Blender/native/rise_blender_bridge.dylib` through its relative-path fallback.
3. For a packaged Blender app bundle, copying `src/Blender/addons/rise_renderer` into Blender's bundled `scripts/addons_core/rise_renderer` directory also works, but it is less convenient while iterating.
4. To install from a zip, package one with `python3 tools/package_blender_addon.py` (after building the bridge) and use Blender's Install from Disk. The zip carries the bridge in `rise_renderer/native/` and the licence notices of RISE's third-party code and of every library the bridge links in `rise_renderer/third_party_licenses/` (see `licenses/README.md`). The libraries themselves are not bundled: the bridge loads them from the absolute paths it was linked against, so the zip works only on machines with the same library layout (the script prints the paths).
5. Enable `RISE Render Engine` in Blender Preferences.
6. Open the add-on preferences and set `Bridge Library` only if auto-discovery does not find the bridge.
7. Choose `RISE` as the active render engine.

## Manual Validation Matrix

- Materials: opaque Principled sphere or cube, metallic plus roughness textures, emissive Principled material, transmission plus IOR glass, coated (Coat Weight/Tint/Roughness/IOR) plastic or clearcoat paint, subsurface (Subsurface Weight/Radius/Scale/IOR) skin or wax, direct image-driven bump/normal maps, and simultaneous base/coat normal maps with an alternate anisotropy direction.
- Hair curves: Blender `Curves` object with `Principled Hair BSDF` across melanin, absorption, and direct-tint modes (`hair_material` + staged `.hair` `hair_geometry`).
- Volumes: homogeneous world fog, homogeneous mesh interior medium, VDB smoke object, and `Principled Volume` with anisotropy plus emission.
- Thin film and dispersion: a dark thin-film dielectric sphere and a thin-film metal (interference hue changes with thickness), and dispersive Principled glass over an image-textured floor (coloured fringes, strongest under the Spectral integrators).
- Lights: `tools/blender_light_parity.py` (Cycles vs RISE, 20 gated cases at +/-0.5%, plus the sheared-panel divergence reported ungated).
- Shader-graph walks: `addons/rise_renderer/test_graph_walk_blender_runtime.py` (run inside Blender). Walks key nodes on `blender_compat.rna_key`, never Python `id()` or `is`: Blender returns a new wrapper per node access, so the old `id()`-keyed walks stopped short on some graph shapes. Fixing that (2026-11-14) changes what a few existing scenes do, always toward what the graph says: a material with unaccepted nodes hidden behind accepted ones now classifies as complex and is gated for a bake where it used to render with those nodes ignored; a Principled, Bump or image behind a long reroute / pass-through chain is now found; a hair material with unsupported nodes upstream now falls back to the default hair material.
- Integrators & settings: Pel and Spectral switches across `Path Tracing`, `BDPT`, `VCM`, and `Auto` (plus `[legacy]` `Pixel` / `MLT`), `Extended SMS` (`sms_extended`), path guiding on and off, adaptive sampling on and off, OIDN on and off, and stability controls that visibly clamp fireflies or shorten bounce chains.
- Bridge compatibility: load the add-on with a matching ABI v19 bridge build and with an intentionally stale bridge to confirm the ABI mismatch fails fast with a readable message.
- Visual comparison: compare RISE against Cycles on a small fixed fixture set for light directionality, roughness response, transmission, hair shading, and volume placement.
