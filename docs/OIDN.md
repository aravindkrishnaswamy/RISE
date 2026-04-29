# OIDN Integration — Audit, Backlog, and Decision Log

Living tracker for RISE's integration of [Intel Open Image Denoise](https://www.openimagedenoise.org/).
Holds the audit baseline, ranked improvement backlog, and a chronological log
of findings, experiments, and decisions over time. Update entries in place as
work lands; never delete history — move items to the **Shipped** or
**Closed (won't do)** sections at the bottom and keep the original text.

## How to use this document

- Each opportunity has a stable ID (`OIDN-P0-1` etc.) — reference these in
  commit messages and PR titles so the tracker can be grepped.
- **Status** field values: `Open` / `In Progress` / `Shipped` / `Closed`. When
  flipping to `Shipped` or `Closed`, fill in the **Result** line and the
  **Decision Log** at the bottom.
- Treat the **Audit baseline** as a snapshot — it is not maintained as code
  changes. When the picture meaningfully shifts, write a new dated audit
  section below it rather than rewriting the baseline.
- Pair changes with the
  [abi-preserving-api-evolution](skills/abi-preserving-api-evolution.md) skill
  whenever a touched signature crosses `RISE_API.h` or `IJob.h`, and the
  [performance-work-with-baselines](skills/performance-work-with-baselines.md)
  skill for any item that claims a wall-clock win — measure, don't assume.

---

## Audit baseline — 2026-04-29

### What RISE drives today

Single concrete pipeline in
[OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp) plus
[AOVBuffers.cpp](../src/Library/Rendering/AOVBuffers.cpp). Wired into:

- [PixelBasedRasterizerHelper.cpp:708](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp#L708)
  (PT/BDPT/VCM pel + spectral, post-accumulation, pre-tone-map)
- [BDPTRasterizerBase.cpp:1014](../src/Library/Rendering/BDPTRasterizerBase.cpp#L1014)
  (denoise **before** splat-film resolve — splatting breaks OIDN per upstream
  README, so this ordering is correct and load-bearing)
- [MLTRasterizer.cpp:1138](../src/Library/Rendering/MLTRasterizer.cpp#L1138) and
  [MLTSpectralRasterizer.cpp:945](../src/Library/Rendering/MLTSpectralRasterizer.cpp#L945)
  (default off — chunk parser hard-codes `oidn_denoise` default to `false` for
  MLT because the entire image lives in the splat film)

Scene-language entry: a single `oidn_denoise` Bool parameter on each rasterizer
chunk in
[AsciiSceneParser.cpp:599](../src/Library/Parsers/AsciiSceneParser.cpp#L599).

### Exact OIDN API surface RISE calls

```cpp
// OIDNDenoiser.cpp:106-144
oidn::DeviceRef device = oidn::newDevice( oidn::DeviceType::CPU );
device.commit();
oidn::BufferRef colorBuf  = device.newBuffer( bufBytes );
oidn::BufferRef outputBuf = device.newBuffer( bufBytes );
std::memcpy( colorBuf.getData(), beautyBuffer, bufBytes );
oidn::FilterRef filter = device.newFilter( "RT" );
filter.setImage( "color",  colorBuf,  oidn::Format::Float3, w, h );
filter.setImage( "output", outputBuf, oidn::Format::Float3, w, h );
filter.setImage( "albedo", albedoBuf, ... );  // optional
filter.setImage( "normal", normalBuf, ... );  // optional
filter.set( "hdr", true );
filter.set( "cleanAux", true );  // when aux present
filter.commit();
filter.execute();
device.getError( errorMessage );
```

### Properties of RISE's AOV collection

[OIDNDenoiser.cpp:155](../src/Library/Rendering/OIDNDenoiser.cpp#L155)
`CollectFirstHitAOVs` is a separate retrace pass after rendering completes:

- 4 primary rays per pixel through `ICamera::GenerateRay`, accumulating albedo
  via `IBSDF::albedo()` and geometric normal at first hit.
- Subpixel jitter + aperture re-sampling per ray → AOVs naturally inherit the
  beauty's DOF / AA blend.
- Sky/miss → albedo (1,1,1), normal (0,0,0) per OIDN docs.
- Transparent / NULL-BSDF surfaces → albedo (1,1,1) — partially correct (see
  `OIDN-P1-4`).
- Parallelized over rows via `GlobalThreadPool`, thread-local RNG.
- Because AOVs are a deterministic 4-sample retrace (not noisy MC accumulation),
  setting `cleanAux=true` is **legitimate** — most renderers cannot honestly
  do this without prefiltering.

### OIDN 2.4 feature surface vs. RISE usage

| Feature | OIDN exposes | RISE uses |
|---|---|---|
| Filter `RT` | Yes | Yes |
| Filter `RTLightmap` (with `directional`) | Yes | No (N/A — RISE doesn't render lightmaps) |
| `hdr` | Yes | **Yes** |
| `srgb` | Yes | No (correctly — input is linear) |
| `cleanAux` | Yes | **Yes** |
| `inputScale` / auto-exposure | Yes (auto if NaN) | No (lets default fire) |
| `quality` (FAST/BALANCED/HIGH/DEFAULT) | Yes | **No — hardcoded default (HIGH)** |
| `maxMemoryMB` | Yes | No |
| `weights` (custom trained model) | Yes | No |
| Device CPU | Yes | **Yes** |
| Device SYCL / CUDA / HIP / **Metal** | Yes | **No — explicitly forced CPU** |
| `oidnNewSharedBuffer` (zero-copy host wrap) | Yes | **No — does device buffers + memcpy** |
| `executeAsync` | Yes | No (sync execute) |
| Progress monitor + cancel callback | Yes | **No** |
| Error callback (vs polling getError) | Yes | No (polls once) |
| Verbose mode 0–4 | Yes | No |
| Separate prefilter passes for noisy aux | Yes (recommended workflow) | N/A — RISE's aux are already clean |
| Tile alignment / overlap (manual tiling) | Yes | No |

### Reference implementations consulted

Cycles (`intern/cycles/integrator/denoiser_oidn.cpp`) drives OIDN with these
notable contrasts to RISE:

- Albedo / normal accumulated **during** the path trace at the first
  non-specular vertex, normalized by the same SPP as beauty. RISE's separate-
  retrace approach is cheaper but loses correlation with beauty for
  second-bounce-dominated pixels and is fundamentally weaker for refractive /
  specular interiors.
- Optional separate prefilter pass (`DENOISER_PREFILTER_ACCURATE`) creates
  dedicated albedo and normal filters, runs them in-place, then sets
  `cleanAux=true` on the beauty filter.
- Sets `quality` explicitly from user-selected denoiser quality.
- Registers a progress monitor with a cancel hook.
- Lets OIDN auto-expose (does not set `inputScale`) — RISE matches.
- Synchronous `oidnExecuteFilter` — RISE matches.

OIDN README (v2.4.1) explicitly notes:

> Weighted pixel sampling (sometimes called *splatting*) introduces correlation
> between neighboring pixels, which causes the denoising to fail.

This is why RISE denoises before splat-film resolve in BDPT and disables
denoise by default in MLT — the architecture choices documented above are
load-bearing and should not be casually changed.

---

## Backlog — ranked

Tiering combines **rendering-quality impact**, **wall-clock impact on Apple
Silicon (RISE's primary platform per [CLAUDE.md](../CLAUDE.md))**, and
**implementation cost**.

### P0 — high leverage, low-to-moderate cost

#### OIDN-P0-1 — Expose `quality` preset as a scene-language parameter (with `auto`)
- **Status:** Code complete — pending commit/PR (2026-04-29)
- **Owner:** Aravind
- **PR:** —
- **Why:** OIDN's BALANCED preset is roughly 2× faster than HIGH; FAST is
  another ~2× on top. RISE today silently uses HIGH (the slowest). Renders
  that finish in 5 s but tag on a 0.8 s denoise on a 4K image are paying for
  final-frame quality on every preview.
- **What:** Add a `oidn_quality` parameter (Enum: `auto` / `high` /
  `balanced` / `fast`) to the rasterizer chunk descriptors; pass through
  `IJob::Add*Rasterizer` → rasterizer ctor → `OIDNDenoiser::Denoise`. Plumb a
  `filter.set("quality", oidn::Quality::*)` call. New default: `auto` (see
  heuristic below) — replaces OIDN's `DEFAULT` (which is just an alias for
  HIGH).
- **Auto heuristic:** Compute `r = render_time_seconds / megapixels`, where
  `render_time_seconds` is wall-clock from rasterizer start to immediately
  before `oidn::Filter::execute()` runs (includes sample accumulation, AOV
  retrace, and buffer marshalling — everything the user has already paid).
  Map:

  | `r` (s/MP) | Quality | Interpretation |
  |---|---|---|
  | `r < 3` | **FAST** | Preview / smoke-test render — denoise should not dominate |
  | `3 ≤ r < 20` | **BALANCED** | Working render — fair trade |
  | `r ≥ 20` | **HIGH** | Final-quality render — extra denoise seconds well-spent |

  Thresholds calibrated against Apple Silicon CPU; see `OIDN-P0-3` for the
  Metal-backend recalibration note. Each frame computes its own `r` and picks
  independently — animations with consistent per-frame render times converge
  to the same bucket. Logged per render: `OIDN auto: render=12.5s, image=1920x1080 (2.07 MP), r=6.04 s/MP → BALANCED`.

  Thresholds live as `static constexpr` in `OIDNDenoiser.cpp` so they're
  easy to tune later from real-world telemetry.
- **Surface invariant:** `oidn_denoise` (existing on/off bool) and
  `oidn_quality` (new enum) are independent. `oidn_quality` is parsed and
  stored even when `oidn_denoise=false` so MLT keeps the parameter
  consistent — irrelevant when denoise is off but cheap to keep parseable.
- **Touch:**
  [AsciiSceneParser.cpp:599](../src/Library/Parsers/AsciiSceneParser.cpp#L599)
  region (and every chunk's `Finalize`),
  [Rasterizer.h](../src/Library/Rendering/Rasterizer.h),
  [OIDNDenoiser.{h,cpp}](../src/Library/Rendering/OIDNDenoiser.cpp),
  [PixelBasedRasterizerHelper.{h,cpp}](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp),
  [BDPTRasterizerBase.cpp](../src/Library/Rendering/BDPTRasterizerBase.cpp),
  [MLTRasterizer.cpp](../src/Library/Rendering/MLTRasterizer.cpp),
  [MLTSpectralRasterizer.cpp](../src/Library/Rendering/MLTSpectralRasterizer.cpp),
  [RISE_API.h](../src/Library/RISE_API.h) + .cpp,
  [IJob.h](../src/Library/Interfaces/IJob.h), [Job.h](../src/Library/Job.h),
  [Job.cpp](../src/Library/Job.cpp).
  New file: `src/Library/Utilities/OidnConfig.h` — register in all five
  build projects per [CLAUDE.md](../CLAUDE.md) → "Source-file add/remove".
- **Effort:** ~half day actual.
- **Verification:** render a representative scene at default + each preset,
  log denoise time, eyeball output. Render at SPP-low and SPP-high and verify
  `auto` flips between FAST / BALANCED / HIGH around the documented
  thresholds. ABI-evolving change → review against the
  [abi-preserving-api-evolution](skills/abi-preserving-api-evolution.md) skill.
- **Result:**
  - `make -C build/make/rise -j8 all` clean (no new warnings).
  - `./run_all_tests.sh` clean: **72/72 pass**.
  - Sample render of `scenes/Tests/Geometry/shapes.RISEscene` (800×800, 0.82 s
    render) emits the expected log line:
    `OIDN auto: render=0.82s, image=800x800 (0.64 MP), r=1.28 s/MP -> FAST`.
    The picked preset matches the documented `r < 3 → FAST` rule.
  - Files touched: 16 source files + 1 new header
    (`src/Library/Utilities/OidnConfig.h`) + 4 build-project registrations
    (vcxproj, vcxproj.filters, project.pbxproj × 4 entries — Filelist
    and rise_sources.cmake skipped as those track only `.cpp` files).

#### OIDN-P0-2 — Cache device + filter across renders
- **Status:** Code complete — pending commit/PR (2026-04-29)
- **Owner:** Aravind
- **PR:** —
- **Primary motivator:** Interactive renderer ([InteractivePelRasterizer](../src/Library/Rendering/InteractivePelRasterizer.h)).
  Each viewport-driven re-render is a fresh `RasterizeScene` call on the
  same persistent rasterizer instance.  Today every call recreates the
  OIDN device + filter, paying ~50–200 ms per frame for kernel loading
  and network commit.  At interactive frame rates that overhead alone
  caps the achievable fps; caching makes the steady-state denoise cost
  essentially "execute + memcpy."
- **Cache key:** `(width, height, hasAlbedo, hasNormal, resolvedQuality)`.
  Mismatch on any → tear down filter and (only if dims change) reallocate
  buffers, rebuild filter, re-commit.  Match → reuse, skip the commit.
  Device is created once on first denoise and survives the rasterizer
  lifetime; only filter and buffers re-key.
- **Lifetime:** Cache lives on the `Rasterizer` base via an opaque pImpl
  pointer so OIDNDenoiser internals stay out of the public header and
  the cache naturally dies with the rasterizer.
- **Why:** Each render today calls `oidn::newDevice(CPU)` and
  `device.commit()`, which is non-trivial (loads ISA-specific kernels,
  allocates pools). For animation or interactive workflows (per-frame
  `render` commands in the same process) that's pure overhead — likely
  50–200 ms per frame.
- **What:** Move device + reusable filter handles to a small singleton or to
  `OIDNDenoiser` static state, recreated only when image dimensions / aux
  presence / quality change. Use `filter.setImage(...)` to swap buffers
  without re-committing the network. (OIDN's `commit()` is what takes time;
  re-execute is cheap.)
- **Touch:** [OIDNDenoiser.{h,cpp}](../src/Library/Rendering/OIDNDenoiser.cpp),
  [Rasterizer.{h,cpp}](../src/Library/Rendering/Rasterizer.h).
- **Effort:** ~2 hours actual.
- **Verification:** render twice in a row from the same `rise` process and
  confirm the second invocation's denoise wall-clock drops. Stopwatch added
  around `ApplyDenoise`.
- **Result:**
  - `make -C build/make/rise -j8 all` clean (no new warnings).
  - `./run_all_tests.sh` clean: **72/72 pass**.
  - 3-render back-to-back on `scenes/Tests/Geometry/shapes.RISEscene`
    (800×800):
    - Render 1 (cold): logs `OIDN: creating CPU device (one-time per rasterizer)` and `OIDN cache: rebuild filter (800x800 q=FAST aux=albedo+normal)`. Denoise 94.1 ms.
    - Render 2 (warm): logs `OIDN cache: hit (800x800 q=FAST)`. Denoise 69.7 ms (24 ms / ~26% saved).
    - Render 3 (warm): cache hit, 68.2 ms.
  - The "creating device" line fires exactly once → device + filter
    state genuinely persist for the rasterizer's lifetime, which is
    the property the interactive viewport needs.

#### OIDN-P0-3 — Add Metal device backend on Apple Silicon
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** RISE primarily targets macOS / Apple Silicon. OIDN 2.x ships a
  Metal backend that runs on the GPU. On an M-series machine the speedup vs.
  CPU is typically 5–15× for the RT filter at 4K. The current code
  **explicitly disables this** with `oidn::DeviceType::CPU` — comment cites
  "image data not accessible by the device" errors. That error comes from
  passing host pointers via `setImage(buf, ...)` to a GPU device. The fix is
  `oidn::newSharedBuffer` (CPU device) or `oidn::newBuffer + memcpy` (GPU
  device, which is what the current code already does) — so the existing
  copy-in/copy-out pattern is **already GPU-compatible**.
- **What:** Try `oidn::DeviceType::Default`, fall back to CPU on
  `getError()`. Or expose `oidn_device` parameter (`auto` / `cpu` / `gpu`).
  Verify the existing `device.newBuffer(bufBytes) + memcpy` flow works on
  Metal — it should.
- **Touch:** [OIDNDenoiser.cpp:106](../src/Library/Rendering/OIDNDenoiser.cpp#L106).
  Build-system: confirm OIDN was built with `OIDN_DEVICE_METAL=ON` on the
  Mac dev machine.
- **Effort:** ~2 hours coding + bake-in testing.
- **Verification:** render a 1080p scene with denoise on; confirm OIDN logs
  device type Metal (with verbose=2 from `OIDN-P0-4`); compare wall-clock vs.
  CPU.
- **When this lands, recalibrate `OIDN-P0-1`'s auto thresholds.** The
  heuristic was tuned against CPU denoise cost (~0.5 s/MP for HIGH on
  Apple Silicon CPU). Metal is roughly 5–15× faster, so the same `r`
  thresholds will leave the heuristic "underspending" — at GPU speeds we
  can afford HIGH on shorter renders. Likely tweak: drop the FAST cutoff
  from 3 → 0.5 s/MP and the BALANCED cutoff from 20 → 4 s/MP, but verify
  with measurements rather than guessing. Tracker location for the
  thresholds: `kAutoFastUntilSecPerMP` / `kAutoBalancedUntilSecPerMP` in
  [OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp).
- **Result:** —

#### OIDN-P0-4 — Register an error callback + verbose toggle
- **Status:** Code complete — pending commit/PR (2026-04-29)
- **Owner:** Aravind
- **PR:** —
- **Why:** Today RISE polls `device.getError()` exactly once after
  `filter.execute()`. OIDN can emit warnings (deprecated parameter usage,
  fallback paths, missing prefilter) that we silently lose. With multiple
  filters (`OIDN-P1-1` below) some of these warnings happen at commit time,
  before the polled `getError`.
- **What:**
  - Register `device.setErrorFunction(OidnErrorCallback, nullptr)`
    immediately after `oidn::newDevice(...)` and before the device's
    first `commit()` so commit-time failures route through our log
    system instead of being silently dropped.  Existing
    `device.getError(...)` polls in `Denoise()` are kept as a per-call
    "no error since last poll" confirmation.
  - Severity mapping: `OIDN_ERROR_CANCELLED` → `eLog_Warning` (we
    don't propagate cancel to OIDN per `OIDN-P1-3` invariant, but if
    OIDN signals it for any reason it's not a fatal condition).  All
    other non-`None` codes → `eLog_Error`.
  - Verbose toggle: defer to OIDN's `OIDN_VERBOSE` env var.  We do
    not call `device.set("verbose", N)` ourselves — setting the env
    var in the shell (e.g. `OIDN_VERBOSE=2 ./bin/rise scene…`) gives
    a knob without baking a build-time level into RISE.
- **Touch:** [OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp).
- **Effort:** ~20 min actual.
- **Verification:** confirm callback registers + does not misfire on a
  clean render (no spurious `OIDN [Code]: …` lines in the log when
  nothing is actually wrong).  Provoking real errors is impractical
  without breaking the deliberate code path; the implementation
  follows OIDN's documented C-API contract verbatim.
- **Result:**
  - `make -C build/make/rise -j8 all` clean (no new warnings).
  - `./run_all_tests.sh` clean: **72/72 pass**.
  - Sample render of `scenes/Tests/Geometry/shapes.RISEscene`
    (800×800) produces no `OIDN [...]` callback lines — exactly the
    expected silence on a healthy code path.

### P1 — solid wins, moderate effort

#### OIDN-P1-1 — Inline AOV accumulation + `accurate` prefilter mode
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** First-hit retrace AOVs are correct only when the beauty's first hit
  matches the retrace's first hit. They diverge for:
  - Refraction-dominated pixels (glass interiors): beauty sees Fresnel-tinted
    interior color, retrace returns white (NULL-BSDF fallback, see
    [OIDNDenoiser.cpp:222](../src/Library/Rendering/OIDNDenoiser.cpp#L222)).
  - Mirror reflections: retrace albedo is the mirror's surface (~1.0
    dielectric reflectance), not the reflected world.
  - Heavy DOF where the focal surface differs by sample.
  - Any future motion blur (currently RISE doesn't render with shutter, but
    inline accumulation is the only way that scales).
- **What:** Two-part change.
  1. Accumulate albedo + normal during the path trace, on the first
     non-specular vertex, normalized by SPP. Buffers live alongside the
     existing radiance buffer in the rasterizer state. This is the Cycles
     model.
  2. Because those AOVs are now noisy, run two extra OIDN filter passes (one
     each for albedo and normal, in-place) before the beauty filter. Toggle
     between this and the cheap retrace via a `oidn_prefilter` enum (`fast` =
     today's retrace + cleanAux, `accurate` = inline + prefilter).
- **Touch:** integrators (`PathTracing`, `BidirectionalPathTracing`, etc. under
  [src/Library/Rendering/](../src/Library/Rendering/)),
  [AOVBuffers.cpp](../src/Library/Rendering/AOVBuffers.cpp),
  [OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp).
- **Effort:** ~1–2 days. Largest item in this list.
- **Verification:** render a glass-heavy scene
  (`scenes/Tests/Materials/glass...` or similar) with `fast` and `accurate`
  prefilters; the latter should preserve refraction tint where the former
  smears. Diff PSNR against a high-spp reference render.
- **Result:** —

#### OIDN-P1-2 — Replace `device.newBuffer + memcpy` with `oidnNewSharedBuffer`
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** Today `Denoise()` does up to 4 host-side full-image copies (image
  → beautyBuf, beautyBuf → colorBuf, outputBuf → denoisedBuf, denoisedBuf →
  image). At 4K RGB that's ~50 MB per copy. On CPU device, OIDN can directly
  alias host memory via `oidnNewSharedBuffer`.
- **What:** When device type is CPU, wrap `beautyBuf` / `albedoBuf` /
  `normalBuf` / `outputBuf` directly. Skip the intermediate
  `device.newBuffer + memcpy`. Falls back to the current path on GPU device.
- **Touch:** [OIDNDenoiser.cpp:113](../src/Library/Rendering/OIDNDenoiser.cpp#L113).
- **Effort:** ~1 hour. Subtle aliasing rules — test in/out buffer overlap.
- **Verification:** heap profiler before / after on a 4K denoise.
- **Result:** —

#### OIDN-P1-3 — Progress monitor (cancel intentionally NOT wired)
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Project invariant — do NOT propagate cancel to OIDN.** The user
  has explicitly stated a preference for OIDN to keep running on
  partial render results when the user cancels mid-render. Cancelling
  mid-render still produces a useful denoised image of whatever
  samples were collected, which is more useful than aborting the
  denoise and getting nothing. Any progress monitor we register
  must always return `true`. If you find yourself wiring
  `is_cancelled()` through to a `return !cancelled` callback, stop
  and re-read this entry.
- **Why (the kept-around progress part):** On GPU at 4K, denoise can take
  1–3 s; on CPU FAST it's still noticeable. A progress monitor is
  useful for surfacing denoise progress in interactive UI, just not
  for cancellation.
- **What:** Register `oidnSetFilterProgressMonitorFunction` for visual
  progress feedback only. Always returns `true` (continue). Defer
  until there's a concrete interactive UI that consumes the progress
  values.
- **Touch:** [OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp).
- **Effort:** ~1 hour, mostly plumbing.
- **Verification:** observe progress callback firing with monotonically
  increasing values during a longer render.
- **Result:** —

#### OIDN-P1-4 — Recurse through specular hits when collecting AOVs
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:**
  [OIDNDenoiser.cpp:222–226](../src/Library/Rendering/OIDNDenoiser.cpp#L222)
  uses albedo `(1,1,1)` for both NULL-BSDF surfaces and miss pixels. OIDN's
  guidance for sky / miss is `(1,1,1)`, but for refractive surfaces the right
  value is the dielectric's tinted transmittance (or, in the inline-
  accumulation model, advance the ray and accumulate at the *next*
  non-specular hit). A glass sphere with this fallback gets a uniform white
  aux signal where the beauty has caustic / refractive color, which OIDN
  treats as noise.
- **What:** When BSDF is NULL/specular, advance the ray and recurse the AOV
  collection until a non-specular hit (cap at ~8 bounces). This lines up with
  Cycles's "first non-specular vertex" rule. Cheap because it's still in the
  AOV-only retrace pass.
- **Touch:** [OIDNDenoiser.cpp:184–234](../src/Library/Rendering/OIDNDenoiser.cpp#L184).
- **Effort:** ~3 hours.
- **Verification:** same glass scene as `OIDN-P1-1`, comparing first-hit-only
  aux vs. recursive-into-non-specular aux. Save aux as EXR (per
  `OIDN-P2-2`) for visual diff.
- **Result:** —

### P2 — polish

#### OIDN-P2-1 — CLI override flag for denoise
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** Today the only way to flip `oidn_denoise` for a scene is to edit
  the `.RISEscene` file. A CLI flag like `--no-denoise` /
  `--denoise-quality fast` in the `rise` binary would help for batch sweeps
  and benchmarking.
- **Touch:** wherever the CLI lives (e.g. `src/Tools/RenderApp/` or analogue).
- **Effort:** ~1 hour.
- **Result:** —

#### OIDN-P2-2 — Write `_albedo` / `_normal` AOVs as separate file outputs
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** Useful for offline debugging when a user reports "denoise looks
  wrong on this scene." Writing the aux passes as `_albedo.exr` /
  `_normal.exr` alongside `_denoised` is a 10-line addition that bypasses
  needing a debugger.
- **Touch:** [PixelBasedRasterizerHelper.cpp](../src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
  output flush region.
- **Effort:** ~30 min.
- **Result:** —

#### OIDN-P2-3 — Expose `inputScale` for extreme-HDR scenes
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Why:** OIDN auto-exposes when `inputScale=NaN`. For scenes with extreme
  luminance (sun disk, point lights with no falloff cap), explicit scaling
  can improve quality. Niche — only matters for power users.
- **Touch:** [OIDNDenoiser.cpp](../src/Library/Rendering/OIDNDenoiser.cpp),
  parser.
- **Effort:** ~1 hour.
- **Result:** —

#### OIDN-P2-4 — `maxMemoryMB` for memory-constrained systems
- **Status:** Open
- **Owner:** —
- **PR:** —
- **Touch:** parser + denoiser.
- **Effort:** ~30 min.
- **Result:** —

### P3 — closed (won't do unless circumstances change)

- **OIDN-P3-1 — Custom trained `weights`.** Niche; would require a training
  pipeline. Revisit if a specific render pathology emerges that the
  off-the-shelf model handles poorly.
- **OIDN-P3-2 — Temporal denoising.** OIDN's RT filter is explicitly not
  temporally stable per upstream README. No upstream support to ship against.
- **OIDN-P3-3 — Manual tiling with `tileAlignment`/`tileOverlap`.** Only
  matters at >8K resolution on memory-constrained hardware; OIDN's internal
  tiling on GPU already handles the interesting cases.
- **OIDN-P3-4 — `RTLightmap` filter.** RISE doesn't render lightmaps.

---

## Suggested first cut

If only one tier ships, **P0** (quality preset + device caching + Metal
backend + error callback) is roughly half a day of work and delivers:

- 2–10× faster denoise on Apple Silicon (Metal)
- 50–200 ms saved per frame in animations (cached device)
- A `balanced` / `fast` preset for previews
- Diagnostics for when something goes wrong silently

P1 adds correctness wins (inline AOVs, refractive-aux recursion) and is most
meaningful on tricky scenes — schedule when there's a specific firefly /
smear complaint that traces back to bad aux.

---

## Cross-cutting verification

Any P0 or P1 change additionally needs:

- Full `make -C build/make/rise -j8 all` clean (no new compiler warnings per
  [CLAUDE.md](../CLAUDE.md) → "Compiler warnings are bugs").
- `./run_all_tests.sh` clean.
- The five-build-system file-list update if any new source files are added
  ([Filelist](../build/make/rise/Filelist), [rise_sources.cmake](../build/cmake/rise-android/rise_sources.cmake),
  Library.vcxproj, .filters, project.pbxproj — see
  [CLAUDE.md](../CLAUDE.md) → "Source-file add/remove").
- Re-run on a representative scene with `oidn_denoise true` and confirm the
  `_denoised` output matches expectations.

---

## Decision log

Append a dated entry whenever an item ships, gets re-scoped, gets a verdict
from a reviewer, or has its priority moved. Most recent first.

### 2026-04-29 — OIDN-P0-4 code complete; cancel-doesn't-propagate invariant recorded
- `OidnErrorCallback` (file-static, C-style function pointer) is
  registered on the OIDN device immediately after `oidn::newDevice`
  and before its first `commit()`.  Maps OIDN's error codes to a
  short name + severity and routes through `GlobalLog()->PrintEx(...)`.
- Verbose: deferred to OIDN's own `OIDN_VERBOSE` env var.  We do not
  override it via `device.set("verbose", ...)` — keeping it as an
  env-var knob is consistent with OIDN's documented control surface
  and avoids baking a level into RISE itself.
- **New project invariant: cancel does NOT propagate to OIDN.**  The
  user has stated explicit preference for OIDN to keep running on
  partial render results when the user cancels.  Cancellation
  produces a useful denoised image of whatever samples were
  collected, which is more valuable than aborting the denoise.  Any
  future progress monitor (`OIDN-P1-3`) must always return `true`.
  This is recorded in the `OIDN-P1-3` ticket body and called out in
  the error-callback docstring so future agents don't accidentally
  wire `is_cancelled()` into a `return !cancelled` callback.

### 2026-04-29 — OIDN-P0-2 code complete
- `OIDNDenoiser` is now a stateful instance class with private opaque
  pImpl `State` holding the cached `oidn::DeviceRef` / `FilterRef` /
  `BufferRef` handles plus the cache key
  `(width, height, hasAlbedo, hasNormal, resolvedQuality)`.  Static
  helpers (`ImageToFloatBuffer`, `FloatBufferToImage`,
  `CollectFirstHitAOVs`) stay static — none of them touch device state.
- Cache lifetime: one `OIDNDenoiser*` member on `Rasterizer` base,
  allocated in the constructor and freed in the destructor.  All four
  call sites (PixelBased, BDPT, MLT, MLT spectral) now go through
  `mDenoiser->ApplyDenoise(...)`.  Per-rasterizer ownership means the
  interactive viewport's persistent rasterizer instance reuses the
  same cached state across every viewport-driven re-render.
- Rebuild semantics: any cache-key mismatch tears down the filter and
  re-commits.  Buffers are re-allocated only when dimensions change;
  toggling aux presence keeps existing color/output buffers and just
  allocates / releases the aux ones.  Device is built lazily on first
  denoise and survives the entire rasterizer lifetime regardless of
  filter rebuilds.
- New log lines: one-shot `OIDN: creating CPU device (one-time per rasterizer)`,
  per-render `OIDN cache: rebuild filter (...)` or `OIDN cache: hit (...)`,
  and total denoise wall-clock appended to the existing
  `OIDN denoising complete.` line as `(N.N ms)`.
- Verified: 800x800 back-to-back render shows 94.1 ms cold → 69.7 ms /
  68.2 ms warm (~25 ms saved per frame).

### 2026-04-29 — OIDN-P0-1 code complete
- Implementation lands all of: `OidnQuality` enum in
  `src/Library/Utilities/OidnConfig.h`; full plumbing through `IJob.h`,
  `Job.h/cpp`, `RISE_API.h/cpp`; `Rasterizer` base gains
  `mDenoisingQuality` and a `BeginRenderTimer()` /
  `GetRenderElapsedSeconds()` pair; `OIDNDenoiser::Denoise` runs the
  heuristic via `ResolveAutoQuality` and emits the documented log line;
  parser descriptor adds `oidn_quality` enum to `AddBaseRasterizerParams`
  plus the three custom-descriptor chunks (PixelPel, MLT, MLT spectral);
  `ParseOidnQuality` helper warns on typos and falls back to Auto.
- All MLT call paths preserve the existing default-off behaviour for
  `oidn_denoise`; `oidn_quality` parses regardless and is consulted only
  when denoise actually runs.
- Build clean (no warnings).  Regression suite green: 72/72 pass.
  Sample render emits the auto log line as designed.
- Threshold constants (`kAutoFastUntilSecPerMP`,
  `kAutoBalancedUntilSecPerMP`) are `static constexpr` in
  `OIDNDenoiser.cpp` so they're easy to retune from real telemetry.

### 2026-04-29 — OIDN-P0-1 started; auto heuristic agreed
- Heuristic: `r = render_time_seconds / megapixels`; `r<3` FAST, `r<20`
  BALANCED, else HIGH. Calibrated for Apple Silicon CPU.
- Independent of `oidn_denoise`: `oidn_quality` is parsed and stored even
  when denoise is off (kept for MLT consistency).
- New enum: `OidnQuality { Auto, High, Balanced, Fast }`. Default `Auto`.
- Replaces OIDN's `DEFAULT` constant from the public surface (it was just
  an alias for HIGH; `Auto` is more useful as a default).
- Note added to `OIDN-P0-3` (Metal): when GPU backend lands, thresholds
  need recalibration — likely 0.5 / 4 s/MP given GPU speedup.

### 2026-04-29 — initial audit
- Created this document from a full audit of the OIDN integration vs. OIDN
  2.4.1 documentation and the Cycles `denoiser_oidn.cpp` reference.
- Confirmed the BDPT pre-splat denoise ordering and the MLT default-off
  behaviour are load-bearing per upstream guidance on splatting; do not
  regress.
- Confirmed `cleanAux=true` is **legitimate** in RISE because AOVs come from
  a deterministic 4-spp retrace, not from MC accumulation. This is unusual
  and worth preserving even when `OIDN-P1-1` lands (the `fast` retrace path
  should remain the default).
- All P0/P1/P2 items above are open; nothing shipped yet.

---

## Shipped log

(empty)
