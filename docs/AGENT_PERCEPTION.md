# Agent Perception AOVs

**Status:** implemented for production beauty renders on the JSON-RPC, MCP,
and chat-tool surfaces (2026-07-26).

## Why the claim holds

Beauty RGB is a lossy projection of scene state. Different geometry,
materials, lighting, and camera arrangements can produce similar RGB pixels.
Depth, surface orientation, and albedo constrain different parts of that
inverse problem, so in general they have positive conditional information
about a scene or task even after beauty is known:

```text
I(task answer; albedo, normal, depth | beauty) > 0
```

This validates the information-theoretic claim, not a universal model-quality
claim. An agent benefits only if its vision stack can interpret the encoding,
the AOV semantics match the task, and the extra image tokens do not crowd out
more useful context. The implementation therefore exposes a bounded,
conventional image plus structured metadata instead of assuming a model API
can ingest arbitrary float tensors.

## Shipped contract

Agent transports enable perception by default for a production beauty render:

```json
{"method":"render","params":{"perception":true}}
```

`AgentRenderParams` keeps `perception=false` as its direct-C++ default, so
existing embedders and display renders do not acquire new allocation. Draft,
object-map, and diagnostic view modes ignore the flag because they already
have specialized observation contracts.

After a successful eligible render:

```json
{"method":"read_image","params":{"representation":"perception","maxEdge":768}}
```

returns one PNG atlas in stable row-major panel order:

```text
+----------------+----------------+
| beauty         | diffuse albedo |
+----------------+----------------+
| world normal   | log depth      |
+----------------+----------------+
```

`maxEdge` bounds the complete atlas, not each panel. The result also reports
`sourceWidth`, `sourceHeight`, `validDepthPixels`, `depthMin`, `depthMax`,
`persistentBytes`, `auxiliaryPeakBytes`, and `encoderRowBytes`.
`representation:"beauty"`
remains the default and is backward-compatible. An unknown representation is
an invalid-params error. If perception was disabled or the last render was
ineligible, the perception result is explicitly unavailable and contains no
stale image.
Builds compiled with `NO_PNG_SUPPORT` may still capture the compact sidecar,
but report the `read_image` perception representation as unavailable because
they have no PNG transport encoder.

The planes use these meanings:

- **Beauty:** the same linear render cached for ordinary `read_image`, with
  the scene's display exposure/tone curve and output color space applied
  during atlas encoding. Guide panels remain stable sRGB display fields.
- **Albedo:** first useful surface's diffuse reflectance, clamped to `[0,1]`.
- **Normal:** world-space shading normal, mapped from `[-1,1]` to display RGB;
  misses are black.
- **Depth:** positive distance to the primary camera ray's first geometric
  surface hit. It is independent of the albedo/normal prefilter selection,
  normalized over valid pixels in log space, near-to-far as white-to-black;
  misses are black. The numeric min/max and valid count make the visualization
  interpretable.

The normal/albedo surface follows the existing OIDN prefilter rule: `fast`
means camera first hit, while `accurate` may walk through delta/specular
vertices to the first non-delta surface. Depth never walks with that selection.
This semantic selection is honored whether or not the build includes OIDN;
it is intentional sharing with the renderer's established auxiliary-signal
semantics, not a dependency on the denoiser.

## One render, planned channels

`FrameStore::Spec` is the demand signal. An eligible agent render installs a
private store with only `Albedo`, `Normal`, and `Depth`. `MakeAOVPlan` unions
those requests with OIDN's albedo/normal requirement. `AOVBuffers` then
allocates only the requested float planes. A normal non-denoised display render
allocates none of them. `perception:false` avoids perception AOV-channel and
compact-sidecar allocation; the agent's private beauty FrameStore is still
installed for output isolation. An OIDN-enabled render may still allocate its
own albedo/normal scratch.

PT, spectral PT, BDPT, VCM, and the shader-dispatch path attach `PixelAOV` to
the beauty estimator and collect the requested data during the render.
Progressive and adaptive paths use the same sample weights and normalize once.
MLT and any legacy estimator that cannot attach a `PixelAOV` use one bounded,
parallel primary-ray fallback after beauty. Thus the common paths are a true
single estimator pass; the API remains correct on every rasterizer without
pretending MLT's Markov-chain samples have a per-pixel first-hit identity.

The private FrameStore is restored before the render call returns, so an agent
render cannot mutate or enlarge the GUI/display store. The cached agent sink
also detaches from the restored display store after compacting its data.

## Exact managed payload cost

For `P = width * height`, the perception-only payload is:

| Lifetime | Payload | Bytes/pixel |
|---|---|---:|
| Render scratch | 3 float albedo + 3 float normal + 1 float depth | 28 |
| Private FrameStore | `RISEPel` albedo + `Vector3` normal + float depth | 52 |
| Cached sidecar | RGB8 albedo + RGB8 normal + 8-bit log depth | 7 |
| **Peak while rendering** | scratch + FrameStore | **80** |
| **Peak while compacting** | 24-byte guide scratch + FrameStore + sidecar | **83** |
| **Persistent after render** | compact sidecar only | **7** |

Accordingly:

```text
auxiliaryPeakBytes = 83 * P
persistentBytes    =  7 * P
```

These are exact logical payload bytes managed by this feature; allocator and
container bookkeeping, the pre-existing beauty cache, and OIDN's own filter
internals are excluded. At 1920x1080 this is about 164.1 MiB peak and 13.8 MiB
retained. Perception depth scratch is released immediately after propagation;
the private FrameStore is restored after capture. If OIDN is enabled, its independently-required albedo/normal
cache retains 24 bytes/pixel for reuse; perception does not retain an
additional float depth plane. Consequently 83 bytes/pixel is the complete
feature payload at peak, not the incremental cost over an already-enabled OIDN
render.

The atlas encoder sends one RGBA scanline at a time directly to libpng and
reports that bounded uncompressed working set as `encoderRowBytes = 4 *
atlasWidth`. It never builds four additional full-resolution RGB images or an
uncompressed atlas. The returned compressed PNG byte vector is the response
payload itself and is necessarily retained until transport; it is not included
in the auxiliary-memory figures.

## Deliberate limits and extension path

- Direct and indirect lighting are not synthesized from beauty. Their split
  depends on an explicit transport convention (emission, NEE, specular chains,
  volumes, and denoising), and a misleading decomposition is worse than no
  channel. RISE's existing `mode:"direct"` and `mode:"indirect"` diagnostic
  renders remain available on demand at extra render cost.
- The compact atlas is an observation product, not an archival AOV format.
  Raw float/EXR transport can be added behind an explicit request when a model
  endpoint can consume it without base64/token inflation.
- Object and primitive IDs remain separate typed FrameStore channels and the
  existing object-map/query tools remain the more precise semantic interface.
- The canonical C API and JSON-RPC/MCP wire contracts are additive and remain
  compatible. Internal C++ clients must rebuild because the rasterizer AOV
  plumbing extends internal `PixelAOV`/BDPT method signatures.
- AOV samples are resolved per pixel with the estimator's sample weights, but
  are not reconstruction-filtered through the beauty pixel filter. With a
  non-box filter, silhouettes in albedo/normal/depth can therefore differ
  slightly from filtered beauty.
- Accuracy improvement should be measured on task suites (spatial relations,
  occlusion, material diagnosis, relighting, and edit localization), comparing
  beauty-only against beauty-plus-perception at equal model/token budgets.

## Regression gates

`FrameStoreTest` locks channel planning, 4/28-byte scratch costs, typed
albedo/normal/depth propagation, and the zero-consumer plan. The end-to-end
`AgentFirstSliceTest` locks transport defaults, the stable atlas layout, PNG
validity and dimensions, bounded encoder-row metadata, depth metadata,
83/7-byte accounting, whole-atlas `maxEdge`, invalid representation handling,
the allocation/stale-cache behavior of `perception:false`, and byte parity
between conventional and atlas beauty under a non-sRGB output color space.
`AgentFrameStoreIsolationTest` additionally crosses shader dispatch, PT, BDPT,
and VCM across their supported RGB/spectral modes to lock primary-hit depth
semantics through glass. Shader dispatch covers RGB, scalar wavelength, and
HWSS; pure-integrator families cover their implemented Pel/NM/HWSS paths.
