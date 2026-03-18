# Blender Renderer Sidecar

This directory contains a Blender renderer integration for RISE that lives entirely outside the core library.

The structure mirrors the host-bridge style of [`src/3DSMax`](/src/3DSMax): Blender-specific code stays in `src/Blender`, while the bridge talks to RISE only through the existing public APIs in [`src/Library/RISE_API.h`](/src/Library/RISE_API.h) and [`src/Library/Interfaces/IJob.h`](/src/Library/Interfaces/IJob.h).

## Layout

- `addons/rise_renderer/`: Blender Python add-on and `RenderEngine` registration.
- `native/`: Small shared-library bridge that feeds Blender scene data into a RISE `IJobPriv` and returns RGBA pixels back to Blender.

## Supported Scope

- Final renders through Blender's external `RenderEngine` API.
- Evaluated mesh export, per-material triangle splits, and object instancing.
- Core Principled BSDF parameters: base color, metallic, roughness, specular, transmission, IOR, and emission.
- Point, spot, sun, and world ambient approximation. Blender area lights are approximated as point lights for now.

## Current Limitations

- No viewport renderer yet.
- No texture graph translation yet.
- No transparency shader-op mapping yet.
- World nodes are reduced to a simple ambient approximation.

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
