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

## Supported Scope

- Final renders through Blender's external `RenderEngine` API.
- Evaluated mesh export, per-material triangle splits, object instancing, and bump-map modifiers (a Blender Bump node exports through the ABI-frozen `AddBumpMapModifier` entry point, which since 2026-09-06 registers a `relief_modifier` in the UV domain -- see `docs/RELIEF_MODIFIER_DESIGN.md` §7.5).
- Direct Principled BSDF translation for base color, metallic, roughness, specular (IOR level and tint, DL-151), transmission, IOR, emission, anisotropy, sheen (`fabric_material`, DL-18), coat (`coated_material`, DL-186), subsurface (`randomwalk_sss_material`, DL-186 -- a SEPARATE model, not a coat-style wrap; see `docs/BLENDER_MATERIAL_TRANSLATION.md` "Coat and Subsurface"), and direct image-driven bump.
- PNG, HDR, EXR, and TIFF image textures when the local RISE build has the matching texture readers enabled.
- Homogeneous participating media on material and world volume outputs.
- Heterogeneous VDB-backed volume objects driven by the `density` grid, exported through a temporary slice cache.
- Point, spot, sun, and world ambient approximation. Blender area lights are still approximated as point lights.
- Curated advanced ray controls for path tracing, adaptive sampling, path guiding, stability, and OIDN denoising.
- Native bridge ABI/version validation plus runtime capability reporting for OIDN, path guiding, and VDB volume support.

## Current Limitations

- No viewport renderer yet.
- No arbitrary Blender node-graph compilation; the exporter is intentionally direct-slot and GGX-first.
- No tangent-space normal map for the BASE material (image-driven `bump` only there -- see "Supported Scope" above); Coat Normal is now a coat-lobe-only exception (DL-192, ABI v14).  No mixed opaque/transmissive per-pixel material translation yet.  Alpha masking IS now supported (DL-193, ABI v14) and reaches legacy and modern transport -- see below.  (Anisotropy, sheen, coat, and subsurface are supported -- see "Supported Scope" above; this line previously also listed clearcoat/subsurface by mistake, and continued to after DL-18 closed sheen and before DL-186 closed coat/subsurface.)
- Alpha (constant or RGBA texture, ABI v14) maps to scalar material coverage for legacy and modern integrators. CLIP is MASK; BLEND/HASHED use stochastic BLEND. See `docs/ALPHA_COVERAGE.md` and `docs/BLENDER_MATERIAL_TRANSLATION.md`.
- Coat Normal is bridged in full (`coat_normal_painter_name`/`coat_normal_scale`, ABI v14, DL-192) -- a tangent-space normal map perturbing ONLY the coat GGX lobe, matching Cycles' own layered behaviour.  Tangent is bridged for the two expressible cases (an active-UV-map tangent needs no bridging by construction; a constant Vector-Rotate angle composes onto the existing anisotropy-rotation slot) -- a second UV map's tangent, a procedural direction, or a non-constant rotation angle has no RISE mechanism and is warned-and-dropped (DL-213, a mesh-level tangent-basis-override RISE does not have).  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Coat Normal" and "Tangent".
- Subsurface uses a documented single-scattering-albedo APPROXIMATION (Radius/Scale/Base-Colour -> sigma_a/sigma_s), not Blender's/PBRT's own photon-beam-diffusion (Christensen-Burley) fit -- the right order of magnitude and per-channel colour bias, not a photometric match to Cycles.  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Subsurface -> `randomwalk_sss_material`" (DL-186).
- Specular Tint (like Specular IOR Level) has no effect at `metallic=1` -- RISE's F0 formula routes both through the dielectric branch of its base_color/F0 lerp only, so it does not reproduce Blender 4.x Principled's additional metallic-edge (F82-style) tint from the same socket.  See `docs/BLENDER_MATERIAL_TRANSLATION.md` "Specular Tint" (DL-151).
- Area lights are still reduced to point lights, so softness and directionality will not match Cycles exactly.
- World surface nodes are still reduced to a simple ambient approximation; only world volume nodes are exported as participating media.
- Heterogeneous media currently treat color and emission as uniform coefficients modulated by the exported density field.

## Build

Build the native bridge after `bin/librise.a` exists:

```sh
make -C src/Blender/native
```

That produces `rise_blender_bridge.dylib` on macOS or `rise_blender_bridge.so` on Linux in `src/Blender/native/`.

## Install In Blender

1. For active development, create a user add-on symlink that points directly at `src/Blender/addons/rise_renderer`. On macOS that user path is `~/Library/Application Support/Blender/5.2/scripts/addons/rise_renderer`.
2. Build the bridge in `src/Blender/native/`. When the add-on is loaded from the source tree, it auto-discovers `src/Blender/native/rise_blender_bridge.dylib` through its relative-path fallback.
3. For a packaged Blender app bundle, copying `src/Blender/addons/rise_renderer` into Blender's bundled `scripts/addons_core/rise_renderer` directory also works, but it is less convenient while iterating.
4. Enable `RISE Render Engine` in Blender Preferences.
5. Open the add-on preferences and set `Bridge Library` only if auto-discovery does not find the bridge.
6. Choose `RISE` as the active render engine.

## Manual Validation Matrix

- Materials: opaque Principled sphere or cube, metallic plus roughness textures, emissive Principled material, transmission plus IOR glass, coated (Coat Weight/Tint/Roughness/IOR) plastic or clearcoat paint, subsurface (Subsurface Weight/Radius/Scale/IOR) skin or wax, and direct image-driven bump.
- Volumes: homogeneous world fog, homogeneous mesh interior medium, VDB smoke object, and `Principled Volume` with anisotropy plus emission.
- Settings: path guiding on and off, adaptive sampling on and off, OIDN on and off, and stability controls that visibly clamp fireflies or shorten bounce chains.
- Bridge compatibility: load the add-on with a matching bridge build and with an intentionally stale bridge to confirm the ABI mismatch fails fast with a readable message.
- Visual comparison: compare RISE against Cycles on a small fixed fixture set for light directionality, roughness response, transmission, and volume placement.
