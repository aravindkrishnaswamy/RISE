# Interface Guide

This directory defines the abstract contracts that hold the engine together. When you are unsure what an implementation is allowed to do, start here.

## How To Read This Directory

- Read `IJob.h` first for the high-level user-facing construction surface.
- Read `IScene.h` and `IScenePriv.h` next for the assembled runtime scene.
- Read `IRasterizer.h`, `IRayCaster.h`, `IShader.h`, `IShaderOp.h`, `IMaterial.h`, and `IBSDF.h` for the render pipeline contracts.
- Read `IManager.h` and the concrete manager implementations when tracking named object ownership.

## Taxonomy

- Scene and job: `IJob.h`, `IJobPriv.h`, `IScene.h`, `IScenePriv.h`
- Rendering: `IRasterizer.h`, `IRasterizeSequence.h`, `IRasterizerOutput.h`, `IRayCaster.h`, `IJobRasterizerOutput.h`
- Shading and materials: `IShader.h`, `IShaderOp.h`, `IMaterial.h`, `IBSDF.h`, `ISPF.h`, `IEmitter.h`
- Scene elements: `ICamera.h`, `IGeometry.h`, `IObject.h`, `ILight.h`, `IPainter.h`, `IRadianceMap.h`, `IPhotonMap.h`, `IIrradianceCache.h`
- Infrastructure: `IReference.h`, `IManager.h`, `ILog*.h`, plus buffer, image, and sampling interfaces

## Public Vs Privileged Interfaces

Several subsystems use paired interfaces:

- `IJob` vs `IJobPriv`
- `IScene` vs `IScenePriv`
- `IObject` vs `IObjectPriv`
- `ILight` vs `ILightPriv`

The public interface is usually the mutation or integration surface meant for ordinary construction. The `Priv` variant usually exposes getters or extra implementation-facing hooks that higher layers need once the scene is assembled.

## Manager Semantics

`IManager<T>` is central to RISE's named-object architecture.

- `AddItem` inserts an object under a string name.
- `GetItem` looks it up without changing long-term ownership semantics.
- `RequestItemUse` and `NoLongerUsingItem` exist for longer-lived use with deletion callbacks.
- `Shutdown` releases managed items.

To understand real behavior, read:

- [IManager.h](IManager.h)
- [../Managers/GenericManager.h](../Managers/GenericManager.h)

## Ownership Rules

- Most interfaces inherit from `IReference`.
- Concrete implementations usually derive from `Implementation::Reference`.
- Do not assume raw pointer ownership based on return type alone.
- Before altering lifetime behavior, inspect the implementation and manager ownership path.

## Search Tips

- If you know an interface name, search for `public virtual <InterfaceName>`.
- If you know a creation API, search in `RISE_API.cpp` to find the concrete class used.
- If you know a scene chunk name, search in `AsciiSceneParser.cpp` to find the parser class and then the corresponding `Job` method or API constructor.

## High-Leverage Files

- Scene construction: [IJob.h](IJob.h)
- Scene parsing: [ISceneParser.h](ISceneParser.h)
- Rendering: [IRasterizer.h](IRasterizer.h)
- Shading: [IShader.h](IShader.h), [IShaderOp.h](IShaderOp.h), [IMaterial.h](IMaterial.h)
- Ownership: [IReference.h](IReference.h), [IManager.h](IManager.h)
