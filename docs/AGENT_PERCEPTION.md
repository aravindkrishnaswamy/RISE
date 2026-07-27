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

Empirical status is intentionally separate. A first controlled local diagnostic
now gives positive but narrow evidence: on 12 mirrored cue-isolation cases with
three repeats, `qwen3.6:27b` improved from 15/36 beauty-only answers to 25/36
with the atlas at exactly matched 384x384 input and reported prompt-token cost.
The gain was concentrated in material-versus-lighting diagnosis (5/12 to
12/12); depth and world-normal gains were small. See
[PERCEPTION_ATLAS_AB_RESULTS.md](PERCEPTION_ATLAS_AB_RESULTS.md). This is evidence
that one model can use the atlas, not a universal agent-quality claim.

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

`maxEdge` bounds the complete atlas, not each panel. When it is omitted,
ordinary beauty remains native-size for compatibility, while perception uses
a safe 1024-pixel whole-atlas bound. The result also reports
`sourceWidth`, `sourceHeight`, `validDepthPixels`, `depthMin`, `depthMax`,
`guidePrefilter`, `persistentBytes`, `auxiliaryPeakBytes`, and `encoderRowBytes`.
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
  the scene's display exposure/tone curve applied and then encoded to sRGB.
  Ordinary beauty may honor another authored output space, but the observation
  atlas intentionally uses one conventional sRGB encoding for every panel.
- **Albedo:** first useful surface's diffuse reflectance, clamped to `[0,1]`.
- **Normal:** world-space shading normal, mapped from `[-1,1]` to display RGB;
  misses are black.
- **Depth:** positive distance to the primary camera ray's first geometric
  surface hit. It is independent of the albedo/normal prefilter selection,
  normalized over valid pixels in log space, near-to-far as white-to-black;
  misses are black. At partially covered pixels, only hit samples enter the
  depth average; background samples are coverage, not zero-distance readings.
  The hit is captured before shader alpha/transparency continuation, medium
  scattering, x-ray traversal, or refraction, so it can intentionally describe
  an alpha-masked or transparent front surface. The numeric min/max and valid
  count make the visualization interpretable.

The normal/albedo surface follows the existing OIDN prefilter rule: `fast`
means camera first hit, while `accurate` may walk through delta/specular
vertices to the first non-delta surface. Depth never walks with that selection.
The response's `guidePrefilter` field reports which rule produced the current
atlas rather than requiring a caller to infer it from the scene.
This semantic selection is honored whether or not the build includes OIDN;
it is intentional sharing with the renderer's established auxiliary-signal
semantics, not a dependency on the denoiser.
BDPT path-guiding training passes do not contribute to these planes: a normal
final pass supplies them, while online-only guiding uses the bounded first-hit
fallback. This prevents temporary training images and their independent sample
weights from contaminating the observation.

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
MLT and legacy estimators that cannot attach a `PixelAOV` use one bounded,
parallel primary-ray fallback after beauty. In `fast` mode that retrace stops
at the geometric first hit; in `accurate` mode it runs through the prepared
shader caster so the built-in path-tracing shader can carry delta-surface and
primary-medium continuations to the first non-delta surface. Thus the common
paths are a true single estimator pass, while MLT remains correct without
pretending its Markov-chain samples have a per-pixel first-hit identity.
Accurate MLT pays for the explicitly requested bounded shader retrace rather
than silently degrading to Fast.

The private FrameStore is restored before the render call returns, so an agent
render cannot mutate or enlarge the GUI/display store. The cached agent sink
also detaches from the restored display store after compacting its data.

## Exact managed payload cost

For `P = width * height`, the perception-only payload is:

| Lifetime | Payload | Bytes/pixel |
|---|---|---:|
| Render scratch | 3 float albedo + 3 float normal + float depth + float hit weight | 32 |
| Private FrameStore | `RISEPel` albedo + `Vector3` normal + float depth | 52 |
| Cached sidecar | RGB8 albedo + RGB8 normal + 8-bit log depth | 7 |
| **Peak while rendering** | scratch + FrameStore | **84** |
| **Peak while compacting** | 24-byte guide scratch + FrameStore + sidecar | **83** |
| **Persistent after render** | compact sidecar only | **7** |

For a cold single-frame render, accordingly:

```text
auxiliaryPeakBytes = 84 * P
persistentBytes    =  7 * P
```

For a cold render these are exact logical payload bytes managed by this feature; allocator and
container bookkeeping, the pre-existing beauty cache, and OIDN's own filter
internals are excluded. At 1920x1080 the cold peak is about 166.1 MiB and the
retained sidecar is about 13.8 MiB. Perception depth scratch is released
immediately after propagation; the private FrameStore is restored after
capture. If OIDN is enabled, its independently-required albedo/normal
cache retains 24 bytes/pixel for reuse; perception does not retain an
additional float depth plane. The 84-byte/pixel render phase is the cold peak;
the depth plane's extra hit-weight is released with depth before compaction.
This is not the incremental cost over an already-enabled OIDN render.

If rendering, denoising, or an output callback throws, an unwind guard releases
all AOV scratch rather than retaining an unreported 24- or 32-byte/pixel failed
attempt. The exact in-memory output is detached from its exact rasterizer on
every exit, before the controller releases its render park. On failure this
releases any partial image or sidecar immediately; on success the session cache
is the sole owner, so switching integrators cannot strand one full observation
on each inactive rasterizer. The last successful compact observation remains
available at its already-reported persistent cost.

`auxiliaryPeakBytes` is a conservative session-aware high-water bound. RISE preserves the last successful
observation until its replacement render and PNG encode succeed, so the prior
7-byte/pixel sidecar is still live during a repeated render. For equal-sized
frames this reports 91 bytes/pixel (84 current + 7 prior). A multi-frame sink
also retains the preceding animation frame while the next frame renders, so a
cold animation peaks at 91 bytes/pixel; replacing an equal-sized cached still
with a multi-frame render can peak at 98 bytes/pixel. Different-sized renders
use the exact prior sidecar byte count rather than a per-pixel approximation.
Lock-dropped image reads register their sink leases too: if a slow reader still
holds an older observation across multiple replacements, every distinct live
7-byte/pixel sidecar is included in the next render's reported peak. There is
therefore no fixed concurrency ceiling. The render-entry snapshot deliberately
keeps counting a lease even if that reader finishes before scratch allocation,
so concurrent release can make the reported peak higher than the actual peak;
it never makes the bound lower. Persistent bytes and the cold-render formulas
remain exact.

The atlas encoder sends one RGBA scanline at a time directly to libpng and
reports that bounded uncompressed working set as `encoderRowBytes = 4 *
atlasWidth`. It never builds four additional full-resolution RGB images or an
uncompressed atlas. The returned compressed PNG byte vector is the response
payload itself and is necessarily retained until transport; it is not included
in the auxiliary-memory figures. Base64 text, the JSON envelope, and any
MCP/chat transport copies are likewise excluded. Every atlas panel is sRGB and
the PNG carries an `sRGB` chunk, so one image-wide declaration is truthful.

## Deliberate limits and extension path

- Direct and indirect lighting are not synthesized from beauty. Their split
  depends on an explicit transport convention (emission, NEE, specular chains,
  volumes, and denoising), and a misleading decomposition is worse than no
  channel. RISE's existing `mode:"direct"` and `mode:"indirect"` diagnostic
  renders remain available on demand at extra render cost.
- Accurate fallback guide discovery requires a path-tracing-capable shader
  chain. A custom or legacy terminal shader that never invokes the path
  tracer cannot identify a non-delta continuation for the collector, so its
  albedo/normal guide pixels remain black. Fast mode still records that
  shader's geometric first hit. The built-in production beauty and MLT paths
  use the supported path-tracing chain.
- A bounded fallback retrace observes the deterministic nominal frame/field
  scene time. It does not replay per-sample animation times for camera exposure,
  scanning, or pixel-rate motion blur. Built-in pixel integrators capture AOVs
  inline at each beauty sample; this limitation applies only to fallback-only
  custom/legacy producers (and MLT's bounded post-pass collector).
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

`FrameStoreTest` locks channel planning, 8/32-byte scratch costs, typed
albedo/normal/depth propagation, and the zero-consumer plan. The end-to-end
`AgentFirstSliceTest` locks transport defaults, the stable atlas layout, PNG
validity and dimensions, bounded encoder-row metadata, depth metadata,
84/7-byte cold accounting, 91-byte replacement/animation peaks, and additional
concurrent read-leased sidecars,
whole-atlas `maxEdge`, invalid representation handling,
the allocation/stale-cache behavior of `perception:false`, and byte parity
of the atlas's declared sRGB beauty under a non-sRGB ordinary output setting. It
also locks Accurate guide continuation after an HWSS primary-medium scatter
and MLT's bounded Accurate retrace through transparent geometry.
`AgentFrameStoreIsolationTest` additionally crosses shader dispatch, PT, BDPT,
and VCM across their supported RGB/spectral modes to lock primary-hit depth
semantics through glass. Shader dispatch covers RGB, scalar wavelength, and
HWSS; pure-integrator families cover their implemented Pel/NM/HWSS paths.
It also renders a partially covered sphere through shader dispatch, PT, BDPT,
and VCM to prove background samples cannot dilute silhouette depth.
It also verifies that both plain PT and AutoRasterizer detach successful and
failed production sinks while retaining the last successful session cache.
`AnimationProgressTest` locks interlaced AOV composition: the first field starts
the auxiliary frame and the second appends its rows without erasing the first,
keeping beauty and perception spatially aligned.
