# Core Library Guide

`src/Library` is the active heart of RISE. If you are changing rendering behavior, scene representation, parser reachability, or the externally visible API, you will almost always touch this tree.

## Big Picture

The engine is built around a high-level `Job` that owns a `Scene` plus a set of named managers for geometry, painters, functions, materials, modifiers, objects, lights, shaders, and shader ops.

Important anchors:

- Public C-style creation API: [RISE_API.h](RISE_API.h)
- High-level convenience interface: [Interfaces/IJob.h](Interfaces/IJob.h)
- Main implementation: [Job.cpp](Job.cpp)
- Scene representation: [Scene.h](Scene.h)

## Render Pipeline

1. A caller creates a `Job` through `RISE_CreateJob` or `RISE_CreateJobPriv`.
2. `Job::InitializeContainers()` creates the `Scene`, managers, and default assets.
3. A parser loads a scene or script into the `Job`.
4. The `Job` configures cameras, objects, shaders, photon maps, rasterizer outputs, and rasterizer choice.
5. The rasterizer attaches the `Scene` to a `RayCaster`.
6. The pixel renderer drives one or more passes over a raster sequence.
7. Rasterizer outputs write files, intermediate images, callbacks, or window output.

## Directory Map

- `Animation/`: keyframes, timelines, animation helpers
- `Cameras/`: camera models
- `DetectorSpheres/`: detector sphere infrastructure
- `Functions/`: polynomial and parametric math helpers
- `Geometry/`: analytic and mesh geometry plus mesh loaders and UV generators
- `Interfaces/`: public abstract interfaces and contracts
- `Intersection/`: ray-primitive intersection routines
- `Lights/`: classic light implementations
- `Managers/`: named registries and ownership containers
- `Materials/`: BSDF and emitter models
- `Modifiers/`: ray intersection modifiers
- `Noise/`: noise generators used by painters and functions
- `Objects/`: object wrappers and CSG
- `Painters/`: color and texture sources
- `Parsers/`: `.RISEscene`, script, command, and options parsing
- `PhotonMapping/`: photon maps, tracers, and irradiance cache
- `RasterImages/`: image representation, readers, writers, and accessors
- `Rendering/`: rasterizers, ray caster, raster sequences, outputs
- `Sampling/`: sample generators and pixel filters
- `Shaders/`: shader ops, shader composition, and volume shaders
- `Utilities/`: reference counting, logging, media paths, math, optics, threading
- `Volume/`: volume data and accessors

## Important Design Patterns

### Named managers

Many scene elements are registered under string names and later resolved by name. The parser depends heavily on this pattern.

### Explicit ownership

Reference counting is pervasive. Managers `addref()` on insertion and release on shutdown. Follow existing lifetime patterns instead of introducing ad hoc ownership.

### Readability over raw speed

The project intentionally favors physically motivated, readable code over aggressive optimization. Preserve that bias unless a change is explicitly performance-driven.

## Extension Checklists

### Adding a new externally constructible type

- Add the implementation in the appropriate subsystem directory.
- Expose a constructor in [RISE_API.h](RISE_API.h) and [RISE_API.cpp](RISE_API.cpp).
- Add a `Job` wrapper if the type should be reachable through `IJob`.
- Add parser support if the type should be usable from `.RISEscene`.
- Add a scene and or test that demonstrates the feature.
  Use `scenes/Tests` for focused regression or baseline scenes, and `scenes/FeatureBased` for curated showcase scenes when the feature is user-facing.
- Update [../../build/make/rise/Filelist](../../build/make/rise/Filelist) if you added new `.cpp` files.

### Adding a new scene-visible feature

- Decide whether the feature belongs in `Geometry`, `Materials`, `Shaders`, `PhotonMapping`, `Sampling`, or `Rendering`.
- Confirm the feature can be named and resolved through the appropriate manager.
- Register it in the scene parser if users need to author it directly.
- Add at least one focused regression scene under `scenes/Tests`.
- Add a curated scene under `scenes/FeatureBased` as well if the feature benefits from a showcase-quality example.

### Changing rasterization behavior

- Inspect [Job.cpp](Job.cpp) for rasterizer setup and option wiring.
- Inspect [Rendering/PixelBasedRasterizerHelper.cpp](Rendering/PixelBasedRasterizerHelper.cpp) for pass structure.
- Inspect [Rendering/RayCaster.cpp](Rendering/RayCaster.cpp) if the change affects intersection and shading dispatch.

## Docs Nearby

- Interface contracts: [Interfaces/README.md](Interfaces/README.md)
- Scene language: [Parsers/README.md](Parsers/README.md)
