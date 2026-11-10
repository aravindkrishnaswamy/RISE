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

- `Agent/`: embedded scene-editing agent loop, LLM provider codecs, perception/viewport observers, and evaluation runner
- `Animation/`: keyframes, timelines, animation helpers
- `Cameras/`: camera models
- `Cst/`: lossless Concrete Syntax Tree (`Cst::ParseToCst` + `Cst::DeriveToJob`) — the canonical `.RISEscene` parse and derive path
- `DetectorSpheres/`: detector sphere infrastructure
- `Functions/`: polynomial and parametric math helpers
- `Geometry/`: analytic and mesh geometry plus mesh loaders and UV generators.  Includes `DisplacedGeometry` — a composite that wraps any `IGeometry`, tessellates it via `IGeometry::TessellateToMesh`, and displaces vertices along their normals using an `IFunction2D`.  Parser chunk: `displaced_geometry`.
- `Importers/`: external asset importers (glTF 2.0, `.hair` curve files)
- `Interfaces/`: public abstract interfaces and contracts
- `Intersection/`: ray-primitive intersection routines
- `Lights/`: classic light implementations
- `Managers/`: named registries and ownership containers
- `Materials/`: BSDF and emitter models
- `Modifiers/`: ray intersection modifiers
- `Noise/`: noise generators used by painters and functions
- `Objects/`: object wrappers and CSG
- `Painters/`: color and texture sources
- `Parsers/`: descriptor-driven scene chunk registry (`ChunkParserRegistry.cpp`), plus script, command, and options parsers (native `.RISEscene` tokenization and derivation live in `Cst/` via `Cst::ParseToCst` + `Cst::DeriveToJob`)
- `PhotonMapping/`: photon maps, tracers, and irradiance cache
- `RasterImages/`: image representation, readers, writers, and accessors
- `Rendering/`: rasterizers, ray caster, raster sequences, outputs, FilteredFilm, AOVBuffers
- `Sampling/`: sample generators and pixel filters
- `SceneEditor/`: interactive scene editing model, transactions, gizmos, and revision history
- `SceneEditorSuggestions/`: grammar-driven completions, hover diagnostics, and editor suggestions
- `Shaders/`: shader ops, shader composition, volume shaders, `PathTracingIntegrator`, `BDPTIntegrator`, `VCMIntegrator`
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
- Update all **5** lock-step build projects when adding or removing `.cpp` / `.h` files under `src/Library/`:
  - [../../build/make/rise/Filelist](../../build/make/rise/Filelist)
  - [../../build/cmake/rise-android/rise_sources.cmake](../../build/cmake/rise-android/rise_sources.cmake)
  - [../../build/VS2022/Library/Library.vcxproj](../../build/VS2022/Library/Library.vcxproj)
  - [../../build/VS2022/Library/Library.vcxproj.filters](../../build/VS2022/Library/Library.vcxproj.filters)
  - [../../build/XCode/rise/rise.xcodeproj/project.pbxproj](../../build/XCode/rise/rise.xcodeproj/project.pbxproj)

### Adding a new scene-visible feature

- Decide whether the feature belongs in `Geometry`, `Materials`, `Shaders`, `PhotonMapping`, `Sampling`, or `Rendering`.
- Confirm the feature can be named and resolved through the appropriate manager.
- Register it in the scene parser if users need to author it directly.
- Add at least one focused regression scene under `scenes/Tests`.
- Add a curated scene under `scenes/FeatureBased` as well if the feature benefits from a showcase-quality example.

### Changing rasterization behavior

- Inspect [Job.cpp](Job.cpp) for rasterizer setup and option wiring.
- Inspect [Rendering/PixelBasedRasterizerHelper.cpp](Rendering/PixelBasedRasterizerHelper.cpp) for pass structure, filtered film lifecycle, and OIDN denoiser integration.
- Inspect [Rendering/RayCaster.cpp](Rendering/RayCaster.cpp) if the change affects intersection and shading dispatch.

### Rasterizer hierarchy

Pixel-based rasterizers inherit from `PixelBasedRasterizerHelper` (common block dispatch, filtered film lifecycle, OIDN denoising, and output flushing) along several branches:

**Shader-dispatch rasterizers — FROZEN legacy pipeline** (use `RayCaster::CastRay` → ShaderOp dispatch; frozen since 2026-10-09):
- `PixelBasedPelRasterizer` — legacy shader-dispatch RGB (**FROZEN**)
- `PixelBasedSpectralIntegratingRasterizer` — legacy shader-dispatch spectral (**FROZEN**)

**Pure integrator rasterizers** (bypass ShaderOp dispatch and call the modern integrators directly):
- `PathTracingPelRasterizer` (extends `PixelBasedPelRasterizer`, overrides `IntegratePixel()`) — iterative PT, RGB (`PathTracingIntegrator`)
- `PathTracingSpectralRasterizer` (extends `PixelBasedSpectralIntegratingRasterizer`, overrides `IntegratePixel()`) — iterative PT, spectral NM/HWSS (`PathTracingIntegrator`)
- `BidirectionalRasterizerBase` (extends `PixelBasedRasterizerHelper`) — shared bidirectional subpath/light-pass base; calls integrators directly (NOT ShaderOp dispatch):
  - `BDPTRasterizerBase` → `BDPTPelRasterizer` / `BDPTSpectralRasterizer` — bidirectional path tracing (`BDPTIntegrator`)
  - `VCMRasterizerBase` → `VCMPelRasterizer` / `VCMSpectralRasterizer` — vertex connection and merging (`VCMIntegrator` + `BDPTIntegrator`)
- `AutoRasterizer` / `AutoSpectralRasterizer` — automatic scene-analysis selector dispatching to PT, BDPT, or VCM (Pel and spectral)
- `MLTRasterizer` / `MLTSpectralRasterizer` — PSSMLT Markov-chain transport over `BDPTIntegrator` (**FROZEN**)

**Interactive preview rasterizer**:
- `InteractivePelRasterizer` — shared low-latency material/AO preview pipeline used identically across macOS, Windows, and Android viewports (`CreateInteractiveMaterialPreviewPipeline`).

## Path Guiding And Adaptive Alpha

The path guiding subsystem uses Intel OpenPGL to learn incident radiance distributions during training passes, then samples from them during the final render.  Two directional sampling strategies are available: one-sample MIS (default) and RIS (Resampled Importance Sampling, selectable via `pathguiding_sampling_type ris`).

The guiding alpha (blend probability) is adaptively scaled each training iteration.  The current approach uses a variance-aware scheme inspired by Rath et al. 2020: the coefficient of variation (CoV) of indirect sample energy determines how much guiding helps.  A simpler Cycles-style approach (using `sqrt(indirectFraction)`) was also tested and can be substituted — see the inline comments in `Rendering/PixelBasedPelRasterizer.cpp` and `Rendering/BDPTRasterizerBase.cpp`.

Key files:
- `Utilities/PathGuidingField.h` / `.cpp` — OpenPGL wrapper, config, training statistics including `indirectSampleEnergySquaredSum` for variance estimation.
- `Utilities/PathTransportUtilities.h` — RIS helper structs and functions (`GuidingRISTarget`, `GuidingRISProposalPdf`, `GuidingRISSelectCandidate`).
- `Rendering/PixelBasedPelRasterizer.cpp` — PT adaptive alpha logic.
- `Rendering/BDPTRasterizerBase.cpp` — BDPT adaptive alpha logic.
- `Shaders/PathTracingIntegrator.cpp` — Iterative PT integrator with inline guiding dispatch (RGB, NM, and HWSS).
- `Shaders/PathTracingShaderOp.cpp` — PT ShaderOp wrapper, delegates to PathTracingIntegrator.
- `Shaders/BDPTIntegrator.cpp` — BDPT RIS/MIS dispatch (RGB and spectral).

## Docs Nearby

- Interface contracts: [Interfaces/README.md](Interfaces/README.md)
- Scene language: [Parsers/README.md](Parsers/README.md)
