# Blender Renderer Sidecar

This directory contains a Blender renderer integration for RISE that lives entirely outside the core library.

The structure mirrors the host-bridge style of [`src/3DSMax`](/src/3DSMax): Blender-specific code stays in `src/Blender`, while the bridge talks to RISE only through the existing public APIs in [`src/Library/RISE_API.h`](/src/Library/RISE_API.h) and [`src/Library/Interfaces/IJob.h`](/src/Library/Interfaces/IJob.h).

## Layout

- `addons/rise_renderer/`: Blender Python add-on and `RenderEngine` registration.
- `native/`: Small shared-library bridge that feeds Blender scene data into a RISE `IJobPriv` and returns RGBA pixels back to Blender.

## Supported Scope

- Final renders through Blender's external `RenderEngine` API.
- Evaluated mesh export, per-material triangle splits, object instancing, and bump-map modifiers.
- Direct Principled BSDF translation for base color, metallic, roughness, specular, transmission, IOR, emission, and direct image-driven bump.
- PNG, HDR, EXR, and TIFF image textures when the local RISE build has the matching texture readers enabled.
- Homogeneous participating media on material and world volume outputs.
- Heterogeneous VDB-backed volume objects driven by the `density` grid, exported through a temporary slice cache.
- Point, spot, sun, and world ambient approximation. Blender area lights are still approximated as point lights.
- Curated advanced ray controls for path tracing, adaptive sampling, path guiding, stability, and OIDN denoising.
- Native bridge ABI/version validation plus runtime capability reporting for OIDN, path guiding, and VDB volume support.

## Current Limitations

- No viewport renderer yet.
- No arbitrary Blender node-graph compilation; the exporter is intentionally direct-slot and GGX-first.
- No tangent-space normal maps, alpha masking, clearcoat, sheen, subsurface, anisotropy, or mixed opaque/transmissive per-pixel material translation yet.
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

- Materials: opaque Principled sphere or cube, metallic plus roughness textures, emissive Principled material, transmission plus IOR glass, and direct image-driven bump.
- Volumes: homogeneous world fog, homogeneous mesh interior medium, VDB smoke object, and `Principled Volume` with anisotropy plus emission.
- Settings: path guiding on and off, adaptive sampling on and off, OIDN on and off, and stability controls that visibly clamp fireflies or shorten bounce chains.
- Bridge compatibility: load the add-on with a matching bridge build and with an intentionally stale bridge to confirm the ABI mismatch fails fast with a readable message.
- Visual comparison: compare RISE against Cycles on a small fixed fixture set for light directionality, roughness response, transmission, and volume placement.
