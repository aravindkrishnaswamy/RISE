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
- **Status:** Shipped — code complete, Metal verified end-to-end via
  the in-tree OIDN install at `extlib/oidn/install/` (2026-04-29).
  Auto-quality threshold recalibration deferred — see decision log
  for the measurement-driven reasoning.
- **Owner:** Aravind
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
- **What:**
  - Add `oidn_device` scene-language parameter (enum: `auto` / `cpu` /
    `gpu`, default `auto`).  Adds `OidnDevice` enum to
    `src/Library/Utilities/OidnConfig.h` and threads it through every
    rasterizer factory + `IJob` virtual + parser Finalize, mirroring
    `oidn_quality`.
  - Device-selection helper does try-with-fallback:
    `auto` → `oidn::DeviceType::Default` (which OIDN picks; on macOS
    that's Metal), fall back silently to CPU if Default fails.
    `gpu` → same as auto but emits a warning when the fallback fires
    so explicit GPU requests don't silently downgrade.  `cpu` → CPU
    always.  After the (committed) device is in hand, query
    `device.get<int>("type")` and log the actual selected type so
    logs surface "Metal" vs "CPU" rather than "what we asked for".
  - Convert the buffer copy paths from `std::memcpy(buf.getData(), …)`
    to `buf.write(0, bytes, hostPtr)` / `buf.read(0, bytes, hostPtr)`.
    The existing memcpy approach only works for CPU devices (where
    `getData()` returns a host pointer); GPU buffers may not be
    host-mapped.  The C++ wrapper's `read`/`write` handle both cases
    correctly with no slowdown on CPU (reduces to memcpy under the
    hood).
- **NOT in this PR:** auto-quality threshold recalibration.  The
  existing `r < 3 → FAST` / `r < 20 → BALANCED` thresholds were
  calibrated against CPU denoise cost.  Once Metal is verified working
  and we have measured timings, threshold retuning becomes a
  measurement-driven follow-up commit (the constants are already
  `static constexpr` for that purpose).
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
- **Install gotcha — Metal needs an OIDN built with `OIDN_DEVICE_METAL=ON`,
  not just the dylib.**  Symlinking Blender's bundled Metal device
  dylib into Homebrew's location is NOT sufficient — the OIDN core's
  list of attempted device backends is baked at compile time, not
  discovered at runtime.  Confirmed empirically with `OIDN_VERBOSE=4`:
  Homebrew's core only ever tries `libOpenImageDenoise_device_cpu.dylib`,
  even with the Metal dylib in place.  **RISE's solution: an in-tree
  build at `extlib/oidn/install/`.**  OIDN is added as a git submodule
  at `extlib/oidn/source`; `extlib/oidn/build.sh` (macOS / Linux) or
  `extlib/oidn/build.ps1` (Windows) configures cmake with the right
  device-backend flags and installs to `extlib/oidn/install/`.  All
  RISE build configs (Makefile, every VS2022 vcxproj, the
  rise-tests CMakeLists) prefer this install when present and fall
  back to the system OIDN otherwise.  See `extlib/README.TXT` and
  CLAUDE.md "Quickstart" for the one-time setup commands.
- **Result:**
  - `make -C build/make/rise -j8 all` clean (no new warnings).
  - `./run_all_tests.sh` clean: **72/72 pass**.
  - Verified all three device paths on `shapes.RISEscene`:
    - `oidn_device auto` (default): silently picks Metal when an
      OIDN install with the Metal backend is loadable
      (`extlib/oidn/install/` after running `build.sh`); falls back
      to CPU on a Homebrew-only install.
    - `oidn_device gpu` (explicit): emits the documented warning
      when CPU fallback fires.
    - `oidn_device cpu`: silent CPU, no warning.
  - Subtle correctness fix during verification: `oidn::DeviceType::Default`
    does not fail when no GPU backend is loadable — it silently
    returns a CPU device.  `ResolveOidnDevice` introspects the
    returned device's actual type via `device.get<int>("type")` and
    only suppresses the warning when the user asked for `auto`.
  - **Metal performance vs CPU on Apple M1 Max, warm-cache, 1920×1080
    (2.07 MP):**

    | Quality   | CPU      | Metal    | Speedup |
    |-----------|----------|----------|---------|
    | FAST      | 250 ms   | 107 ms   | 2.3×    |
    | BALANCED  | 480 ms   | 150 ms   | 3.2×    |
    | HIGH      | 1010 ms  | 226 ms   | 4.5×    |

    Smaller speedup than the doc's original "5–15×" estimate — the
    M1 Max's BNNS-accelerated CPU backend is genuinely fast.  Metal's
    win grows with quality (4.5× at HIGH where the network is biggest).
    Cold-rebuild is ~200 ms slower on Metal than CPU because of GPU
    pipeline / shader-compile overhead, but the cache from `OIDN-P0-2`
    amortises that across the rasterizer's lifetime.

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
- **Status:** Shipped (v1 PT-only on 2026-04-29; v2 BDPT + VCM
  Pel/Spectral on 2026-04-29).  Supersedes OIDN-P1-4 — inline
  accumulation handles glass refraction probabilistically via
  per-sample `ScatteredRay::isDelta` instead of a separate recursive
  retrace.  v1 shipped the full plumbing + the 3-filter prefilter
  pipeline + first-non-delta AOV recording in `PathTracingIntegrator`
  (used by `pathtracing_pel_rasterizer`).  v2 extends the
  first-non-delta AOV walk to BDPT (Pel + Spectral) and VCM
  (Pel + Spectral) by walking the post-trace `eyeVerts[]` vector
  for the first non-delta SURFACE vertex — exploiting BDPT/VCM's
  reified-path representation so no integrator-signature changes
  are needed.  PathTracingIntegrator's Fast-mode IntegrateRay
  hook intentionally does NOT walk past delta — PT can't reify
  the path the way BDPT/VCM do, and the Accurate-mode inline-
  during-trace path is the principled answer when walk-past
  matters; in Fast mode PT records at the camera ray's first
  hit (using the dielectric's view-dependent albedo for glass,
  white for NULL-BSDF transparency).
- **Owner:** Aravind
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
- **Result (v1):**
  - `make -C build/make/rise -j8 all` clean (no new warnings).
  - `./run_all_tests.sh` clean: **72/72 pass**.
  - 3-filter pipeline verified end-to-end on M1 Max + Metal:
    - Cold rebuild with `oidn_prefilter accurate`: ~755 ms (3 filter
      compiles vs 1 in fast).
    - Warm cache hit with `accurate`: ~60 ms (within noise of fast
      mode's 43-67 ms — the prefilter networks are smaller than the
      beauty filter and don't dominate).
    - Cache key extended with `prefilterMode` so toggling the knob
      triggers a clean rebuild without leaking state.
  - Per-sample `ScatteredRay::isDelta` is the principled signal — no
    static `isPureSpecular` check on IBSDF needed (RISE's IBSDF is
    purely an evaluation primitive, sampling lives in shaders).
    Rough dielectrics handled correctly: each sample's Fresnel
    decision flips the per-sample AOV target, averaging to a
    Fresnel-weighted mix that matches the beauty.
  - **v2 scope** (BDPT / VCM / PixelBasedPel first-non-delta hooks)
    is tracked at the bottom of this ticket.
  - **Re-verification on 2026-04-29 after rebase + stash pop:**
    Stash auto-merged with the upstream "Add preview OIDN denoise
    ladder" commit (`6ac3824`) cleanly — both branches' edits to
    `OIDNDenoiser::State`, `Denoise()`/`ApplyDenoise()` signatures,
    and `PixelBasedRasterizerHelper::PrepareRuntimeContext` were
    additive and orthogonal.  Rebuild clean, all 72 tests pass.
    Smoke test on glass scene (200×150, M1 Max + Metal):
    `oidn_prefilter accurate` cold-cache 461 ms (3 commits +
    Metal device init), `fast` cold-cache 197 ms.  The accurate /
    fast ratio holds — accurate is paying for the two extra filter
    commits as designed, not regressing.
- **Result (v2):**
  - **BDPT (Pel)** — `BDPTPelRasterizer.cpp:133` rewritten from
    "use eyeVerts[1]" to a walk-past-delta loop matching
    BDPTSpectralRasterizer's pre-existing pattern.  The walk uses
    `BDPTVertex::isDelta` which carries the per-sample
    `pScat->isDelta` value already populated by `GenerateEyeSubpath`
    at line 2990.  No signature change to the integrator was
    required — BDPT reifies the path into `eyeVerts[]` so the
    walk happens at the rasterizer level after the subpath is
    generated.
  - **BDPT (Spectral)** — already did walk-past-delta as of v1;
    no change needed.
  - **VCM (Pel)** — `VCMPelRasterizer.cpp:348` mirrored to the
    same walk-past-delta pattern as BDPT Pel.
  - **VCM (Spectral)** — `VCMSpectralRasterizer.cpp:366` rewritten
    from "check `eyeVerts[1].isDelta` and silently skip if delta"
    (which was actually a regression in glass scenes — the
    sample's AOV contribution was dropped) to the proper
    walk-past-delta loop matching BDPTSpectralRasterizer.
  - **PathTracingIntegrator** — intentionally NOT changed.
    The Fast-mode `IntegrateRay` hook still records at the
    camera ray's first hit using the dielectric's `albedo()`
    method, which gives the view-dependent Fresnel-tinted
    albedo for glass (better than white-fallback).  The
    Accurate-mode hook in `IntegrateFromHit` continues to
    walk past delta scatters via per-sample `ScatteredRay::isDelta`.
  - The `oidn_prefilter` knob now has consistent semantics across
    all integrators: Fast = 1 OIDN filter (cleanAux=true, AOV
    treated as clean) + cheap AOV recording; Accurate = 3 OIDN
    filters (prefilter passes treat AOV as noisy) + (for PT)
    inline first-non-delta AOV recording.  For BDPT/VCM the AOV
    quality is uniform across modes — the walk-past-delta is
    essentially free since `eyeVerts[]` already exists; the only
    mode-driven difference is the OIDN filter pipeline cost.
  - Build clean, 72/72 tests pass.  Smoke tests on M1 Max + Metal
    at 256×256 with `oidn_prefilter accurate`:
    - BDPT cornellbox + glass spheres: 4.5 s render, 518 ms
      cold-cache denoise.
    - VCM cornellbox + glass spheres: 4.3 s render, 437 ms
      cold-cache denoise.
    - BDPT same scene with `oidn_prefilter fast`: 228 ms denoise
      (vs 518 ms accurate, 2.3× speedup as designed).

#### OIDN-P1-2 — Replace `device.newBuffer + memcpy` with `oidnNewSharedBuffer`
- **Status:** Shipped — 2026-04-29.  `oidn::DeviceRef::newBuffer(void*, size_t)`
  (the C++ wrapper for `oidnNewSharedBuffer`) wraps host memory
  directly; the device-side `newBuffer(size_t)` + `buffer.write` /
  `read` round trip is now skipped on CPU device.  GPU devices
  (Metal / SYCL / CUDA / HIP) keep the device-owned-buffer path
  because their memory isn't host-mapped.  Mode is auto-detected
  by introspecting the actual device type after creation, so
  `oidn_device auto` correctly picks zero-copy when OIDN falls
  back to CPU on a Mac without the metal device dylib.
- **Owner:** Aravind
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
- **Result:**
  - `OIDNDenoiser::State` gains `useSharedBuffers` (bool, set after
    device commit by introspecting `device.get<int>("type")` == CPU)
    and four `boundXxxPtr` host-pointer trackers.
  - Cache-key extension: when `useSharedBuffers` is true, a change
    in any of (color / albedo / normal / output) host pointer
    triggers a cache miss → filter rebuild against the new pointer.
    In practice the staging vectors (`beautyStaging`, `denoisedStaging`)
    inside State are stable across calls of the same dimensions
    (vector capacity is preserved), and `AOVBuffers` lives on the
    rasterizer for its lifetime, so cache hits are the common case.
  - `Denoise()` rebuild branch: shared mode calls
    `device.newBuffer(host_ptr, bytes)` for color / output / albedo /
    normal (with `const_cast` on the input-only aux pointers — safe
    in Fast mode since OIDN doesn't write through them; in Accurate
    mode the prefilter writes back in-place, which is intentional
    and harmless because `AOVBuffers::Reset()` zeroes the buffers
    before each render).  Non-shared mode keeps the original
    `newBuffer(bytes)` device-owned path.
  - Per-call write/read are gated on `!useSharedBuffers` — in shared
    mode the data is already aliased and the copies are no-ops.
  - Build clean, 72/72 tests pass.  Smoke tests on M1 Max:
    - **CPU shared** (`oidn_device cpu`) at 200×150 + glass scene:
      cold-cache 12 ms, warm-cache 5.9 ms.  Log shows
      `[zero-copy shared buffers]` suffix on device creation.
    - **Metal** (`oidn_device auto`, default) at 200×150 + glass:
      cold 209 ms (Metal init dominates), warm 5.2 ms.  No
      `[zero-copy]` suffix as expected.
    - **CPU shared + Accurate prefilter**: 24 ms cold (vs 12 ms
      Fast).  In-place prefilter through shared aliases works.
  - Memory savings scale with image size: at 4K RGB a single
    image is ~96 MB; eliminating 4 such copies saves ~380 MB of
    transient bandwidth per denoise on the CPU path.

#### OIDN-P1-3 — Progress monitor (cancel intentionally NOT wired)
- **Status:** Closed (won't do unless an interactive UI consumer
  arrives) — 2026-04-29.  No call site reads denoise progress
  today; the cancel-no-propagate invariant is documented below
  for future reference if the call gets revisited.
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
- **Status:** Closed — superseded by OIDN-P1-1 (2026-04-29).  The
  retrace-based recursion would have been a stepping stone toward
  the principled inline-accumulation answer; once we picked the
  inline approach, P1-4's straight-through recursion in the retrace
  pass is no longer the right shape of fix.  The `fast` retrace
  path keeps its existing first-hit-only behavior; the `accurate`
  prefilter mode (P1-1) handles the multi-bounce glass case
  through per-sample `ScatteredRay::isDelta` detection during the
  actual path trace.
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
- **Status:** Closed (won't do, low ROI) — 2026-04-29.  Editing
  the scene file is acceptable friction for batch sweeps; the
  parser already supports per-chunk `oidn_denoise` and
  `oidn_quality` parameters.  Reopen if a benchmarking workflow
  emerges that genuinely needs flag-driven override.
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
- **Status:** Closed (won't do, low ROI) — 2026-04-29.  No
  concrete debugging request.  Reopen when a real "denoise
  looks wrong on this scene" report arrives that's hard to
  diagnose without aux dumps.
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
- **Status:** Closed (won't do, niche) — 2026-04-29.  OIDN's
  auto-exposure (`inputScale=NaN` default) handles all
  observed scenes correctly.  Reopen if a specific extreme-HDR
  pathology is identified.
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
- **Status:** Closed (won't do, niche) — 2026-04-29.  Apple
  Silicon's unified memory + RISE's typical render scale don't
  hit memory pressure.  Reopen if a memory-bound user reports
  OOM during denoise.
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

### 2026-04-29 — Cancel-still-denoises: align code with stated invariant
- Found a doc/code inconsistency.  The OIDN-P1-3 entry stated the
  user invariant: *"Cancelling mid-render still produces a useful
  denoised image of whatever samples were collected, which is more
  useful than aborting the denoise and getting nothing."*  But
  `PixelBasedRasterizerHelper::ShouldDenoiseCompletedRender` was
  gated on `bDenoisingEnabled && passCompleted`, so cancelling a
  render mid-flight skipped OIDN entirely and flushed the raw
  noisy partial.  Per the user's verdict ("doc captures intent,
  code should change"), the gate is removed.
- Renamed `ShouldDenoiseCompletedRender(passCompleted, w, h)` to
  `ShouldDenoise()` and dropped now-unused parameters.  The base
  predicate is just `bDenoisingEnabled`; the
  `InteractivePelRasterizer` override stacks
  `mPreviewDenoiseMode != PreviewDenoise_Off` on top.  Header
  documents the new contract — cancellation is *intentionally*
  not consulted.
- Call site in `RasterizeScene` no longer passes `mainPassCompleted`;
  the local is preserved (with a `(void)` cast) so future code can
  log or surface completion state without re-deriving it.
- The OIDN-P1-3 invariant ("if a progress monitor were ever wired
  up, never propagate cancel") still holds — that's a separate
  concern about OIDN's *own* cancel mechanism.  The two-layer
  design (RISE rasterizer cancel, OIDN progress cancel) is now
  consistent: neither aborts denoise.
- Risk acknowledged: cancelled renders may have un-rendered or
  under-sampled tiles where OIDN smears valid data toward zero
  at the boundary.  The user prefers this to no denoise — for
  interactive cancel-restart loops the smoothed partial is more
  readable than raw MC noise.  If the smear ever becomes a
  problem in production batch renders, gate the new behavior
  on a per-rasterizer config flag (e.g.
  `bDenoiseOnCancel = false` for production, `true` for
  interactive).  Not done now; revisit if a concrete complaint
  arises.
- 72/72 tests pass; build clean.  Regression render confirmed
  no change to the completed-render path (Metal cold 214 ms,
  matching pre-change numbers).

### 2026-04-29 — Backlog wrap-up: P1-3 / P2-* closed as not worth doing
- After shipping P0-1..P0-4, P1-1 (v1+v2), and P1-2, the user
  evaluated the remaining backlog and decided none of it is worth
  the effort right now.  Items closed:
  - **P1-3** progress monitor — no current consumer; reopen when
    an interactive UI needs denoise progress feedback.
  - **P2-1** CLI override flag — editing the scene file is
    acceptable friction; the parser surface is already enough.
  - **P2-2** aux-buffer EXR dump — no concrete debugging request.
  - **P2-3** inputScale knob — OIDN's auto-exposure handles
    observed scenes; niche.
  - **P2-4** maxMemoryMB knob — Apple Silicon unified memory
    means RISE doesn't hit pressure here.
- The "Why" / "What" sections of each closed entry are preserved
  verbatim so a future reopener has the full context without
  needing to re-derive the design.  Each Status field carries
  the dated rationale and a "reopen if…" trigger condition.
- This closes out the OIDN integration audit started 2026-04-29
  with five P0 + two P1 wins shipped (`9eb9b31`, `dc237d1`,
  `3369f09`, `db50bbe`, plus prior P0 commits).  All future
  OIDN work has its own concrete trigger; the tracker is no
  longer carrying speculative items.

### 2026-04-29 — OIDN-P1-2 shipped (zero-copy shared buffers on CPU device)
- `OIDNDenoiser::Denoise` now uses
  `oidn::DeviceRef::newBuffer(host_ptr, bytes)` (the C++ wrapper for
  `oidnNewSharedBuffer`) on CPU device, eliminating up to 4
  image-sized memcpy operations per denoise (color in, albedo in,
  normal in, output out — each was ~50 MB at 4K RGB).
- **Auto-detection over user knob:** mode is decided by introspecting
  `device.get<int>("type") == CPU` AFTER device creation.  This is
  more robust than trusting the user's `oidn_device` parameter
  because `Default` silently picks CPU when no GPU backend is
  loadable (e.g., Mac without the metal device dylib — see
  OIDN-P0-3 install gotcha).  The log line gains a
  `[zero-copy shared buffers]` suffix when shared mode kicks in,
  so it's visible at a glance.
- **Cache-key extension for pointer stability:** shared buffers pin
  to a specific host address at filter-commit time; if the caller
  passes a different pointer next call, we must rebuild.  Added
  `boundColorPtr` / `boundOutputPtr` / `boundAlbedoPtr` /
  `boundNormalPtr` to State and treat any mismatch as a cache
  miss.  In practice the State staging vectors (`beautyStaging`,
  `denoisedStaging`) and `AOVBuffers` are stable across calls of
  the same dimensions, so cache hits are the common case.
- **Const correctness deliberately relaxed:** input-only aux
  pointers are `const float*` in the API, but
  `oidn::Buffer::newBuffer(void*, size_t)` requires a non-const
  pointer.  `const_cast` is safe in Fast mode (OIDN doesn't write
  inputs).  In Accurate mode the in-place prefilter writes back
  to the aux buffer through the shared alias, which mutates the
  host AOV vector — that's intentional and harmless because
  `AOVBuffers::Reset()` zeroes the buffers before each render.
  Documented in the OIDN-P1-2 entry.
- **GPU path unchanged:** Metal / SYCL / CUDA / HIP devices keep
  the original `newBuffer(bytes)` + `buffer.write` / `read` round
  trip because their memory is not host-mapped.  Smoke test
  confirmed no regression on Metal (cold 209 ms / warm 5.2 ms,
  same as pre-P1-2 numbers).
- 72/72 tests pass; build clean.  No new source files.

### 2026-04-29 — OIDN-P1-1 v2 shipped (BDPT + VCM walk-past-delta AOV)
- BDPT Pel, VCM Pel, and VCM Spectral rasterizers now walk
  `eyeVerts[]` for the first non-delta SURFACE vertex when
  extracting OIDN AOVs, mirroring the pattern BDPTSpectralRasterizer
  already used.  Glass / mirror first-hits are skipped so the AOV
  represents the surface visible *through* the delta material —
  matching what the beauty pass shows.
- **Key insight:** BDPT/VCM reify the path into `eyeVerts[]` so the
  per-sample `isDelta` walk happens after the subpath is generated,
  at zero ray-casting cost.  No integrator-signature changes were
  needed; the original v2 plan (thread `PixelAOV*` through
  `BDPTIntegrator::GenerateEyeSubpath` to plant inline hooks)
  was scrapped in favour of this simpler, equivalent post-walk.
- **PathTracingIntegrator** intentionally not changed.  PT cannot
  reify the path the way BDPT/VCM do; its Fast-mode IntegrateRay
  hook stays at first-hit (using the dielectric's view-dependent
  `albedo()` for glass, which is reasonable — better than
  white-fallback).  Walk-past-delta semantics in PT live in
  Accurate mode where the inline-during-trace hook in
  `IntegrateFromHit` already does this via per-sample
  `ScatteredRay::isDelta`.
- VCM Spectral additionally fixes a small pre-existing issue:
  before v2, glass-front samples silently dropped their AOV
  contribution (the `if v1.isDelta` check guarded against writing
  but didn't fall back to a deeper vertex).  Now the walk
  recovers a useful AOV from the surface behind the glass.
- Smoke tests on M1 Max + Metal at 256×256 with both modes:
  BDPT 4.5 s render + 518 ms accurate / 228 ms fast denoise;
  VCM 4.3 s render + 437 ms accurate.  All 72 tests pass; build
  clean.  No new source files added so the five-build-system
  file-list update from CLAUDE.md does not apply.

### 2026-04-29 — OIDN-P1-1 v1 code complete; OIDN-P1-4 closed as superseded
- New scene-language parameter `oidn_prefilter` (enum: `fast` /
  `accurate`, default `fast`).  Plumbed through the full surface:
  `OidnPrefilter` enum in OidnConfig.h, threaded through every
  IJob virtual + RISE_API factory + Job override + parser Finalize
  call.  MLT integrators always force `Fast` regardless of the knob
  — the splat-film invariant from OIDN-P1-1 remains intact.
- `RuntimeContext` gains `aovPrefilterMode` so integrators that
  inline-accumulate AOVs can branch on it.  Set by the rasterizer's
  `PrepareRuntimeContext` from `mDenoisingPrefilter`.
- OIDN denoiser State extended with `albedoFilter` + `normalFilter`.
  In `accurate` mode they're committed alongside the beauty filter
  during the cache rebuild path and run in-place on the aux buffers
  before the beauty filter executes.  Cache key adds `prefilter`
  field so toggling modes triggers a clean rebuild.
- `PathTracingIntegrator::IntegrateRay` now gates first-hit AOV
  recording on `aovPrefilterMode == Fast`.  In `Accurate` mode the
  hook moves to the scatter sites in `IntegrateFromHit` (multi-
  select + single-select branches) where `ScatteredRay::isDelta`
  is the per-sample signal: first non-delta scatter on a path
  records the AOV at that vertex, walking past glass / mirror.
- Per-sample detection (rather than a static `IBSDF::isPureSpecular`
  check) avoids the brittleness in mixed-Fresnel materials: rough
  dielectrics record at the rough surface or behind it depending
  on each sample's Fresnel decision, averaging to a Fresnel-
  weighted mix that matches the beauty.  Confirmed against
  RISE's `IBSDF` interface — it's purely an evaluation primitive
  (no `Sample` method); scattering decisions live in `IShader` /
  `IShaderOp` and produce `ScatteredRay` records with `isDelta`
  already populated.
- **v1 scope:** `pathtracing_pel_rasterizer` (and any rasterizer
  using `PathTracingIntegrator`) gets the full first-non-delta
  AOV in `accurate` mode.  BDPT pel/spectral, VCM pel/spectral,
  `pixelpel_rasterizer`, and `PixelBasedSpectralIntegratingRasterizer`
  pay the prefilter cost in `accurate` mode but their AOV still
  records at first hit (their integrator chains haven't been
  extended yet).  v2 follow-up.
- **OIDN-P1-4 closed as superseded** — the recursive-retrace
  approach in P1-4 was always going to be a stepping stone.  P1-1's
  inline + per-sample-isDelta is the principled answer; once it
  lands fully (v2), P1-4 has no remaining value.
- **Rebase + ship addendum (same day):** prior to commit, the
  branch picked up an upstream commit `6ac3824 "Add preview OIDN
  denoise ladder"` (interactive viewport denoise capability —
  staging buffers in `OIDNDenoiser::State`, `samplesPerPixel` knob
  on `CollectFirstHitAOVs`).  Stash auto-merged cleanly with all
  P1-1 hunks; both sides' edits to the State struct, denoise
  signatures, and `PrepareRuntimeContext` were additive.  Build,
  72/72 tests, and a glass-scene smoke test (`accurate` 461 ms
  cold, `fast` 197 ms cold on M1 Max + Metal at 200×150) all
  clean.  Committed as a follow-up commit on top of the preview
  ladder.

### 2026-04-29 — OIDN-P0-3 shipped (Metal verified end-to-end via in-tree submodule build)
- OIDN added as a git submodule at `extlib/oidn/source` pinned to
  v2.4.1 (matches the Homebrew bottle version we were already on, so
  no API drift).  `extlib/oidn/build.sh` (macOS / Linux) and
  `.../build.ps1` (Windows) install to `extlib/oidn/install/` with
  the right per-platform device backends compiled in.
- Build-config rewiring: `build/make/rise/Config.OSX` adds an
  `OIDN_PREFIX` variable that prefers `extlib/oidn/install/` when
  present and falls back to Homebrew otherwise.  All 8 affected
  Windows vcxproj files (Library, RISE-CLI, RISE-GUI, 5 Tools/
  test projects) and `build/cmake/rise-tests/CMakeLists.txt` swap
  hardcoded `C:\Dev\oidn-2.4.1.x64.windows\` paths for
  `$(SolutionDir)..\..\extlib\oidn\install\` (Windows) /
  `${RISE_ROOT}/extlib/oidn/install` (CMake).  Hardcoded
  `C:\Dev\GitHub\RISE\bin\` in `Tools/ExrFireflyInspect.vcxproj`
  fixed in the same sweep.
- `build/make/rise/Config.OSX` rpath is `$(abspath ...)` rather than
  the relative path that `$(OIDN_EXTLIB_PREFIX)` would naturally
  produce: dyld interprets non-absolute non-`@token` rpaths
  relative to cwd, which would silently fall through to
  `/opt/homebrew/lib` and load the CPU-only Homebrew OIDN even
  though the Makefile pointed at extlib.  This took ~30 minutes
  to track down; the comment in Config.OSX records the gotcha so
  future agents don't reintroduce it.
- **Threshold recalibration NOT done.**  After measuring
  CPU vs Metal at 1080p (see OIDN-P0-3 result block), the
  speedup turned out to be 2.3–4.5× — much smaller than the
  doc's original 5–15× projection.  More importantly, the
  `r = render_seconds / megapixels` heuristic was originally
  framed as a USER-INTENT signal (short render = preview =
  FAST denoise; long render = final = HIGH denoise), not a
  cost-budget calculation.  Keeping the same thresholds means
  the user gets consistent quality decisions across CPU and
  Metal, with Metal just delivering them faster.  Reopen if a
  real animation flow makes the cost-budget framing more
  important than intent matching.

### 2026-04-29 — OIDN-P0-3 code complete (Metal-ready, install-gated)
- New scene-language parameter `oidn_device` (enum: `auto` / `cpu` /
  `gpu`, default `auto`).  Plumbed through the same surface as
  `oidn_quality`: `OidnDevice` enum in OidnConfig.h, threaded through
  10 IJob virtuals, 10 RISE_API factories, and all matching Job
  overrides + parser Finalize calls.  The two MLT legacy IJob
  overloads forward `OidnDevice::Auto`.
- `ResolveOidnDevice` honours the knob with try-with-fallback:
  - `Auto`: try `oidn::DeviceType::Default`; silently use whatever
    OIDN picks (Metal where available, CPU otherwise).
  - `GPU`: try Default; if the returned device's actual type is CPU,
    emit a warning so explicit GPU requests aren't silently
    downgraded.  This is the subtle case — Default does NOT fail when
    no GPU backend is loadable; it returns CPU.  We introspect via
    `device.get<int>("type")` after commit.
  - `CPU`: bypass Default entirely.
- The error callback (P0-4) is registered AFTER ResolveOidnDevice has
  returned a working device, not before, so first-attempt failures
  during fallback don't spam the log.
- `std::memcpy(buf.getData(), …)` replaced throughout with
  `buf.write(0, bytes, hostPtr)` / `buf.read(0, bytes, hostPtr)` so
  the same code works on GPU buffers (where `getData()` may return
  NULL).  No CPU regression — the C++ wrapper reduces to memcpy on
  CPU.
- Verified: build clean, 72/72 tests pass.  All three device paths
  produce the right log lines.  Metal selection itself is gated by
  whether the user's OIDN install ships the Metal device dylib — see
  the OIDN-P0-3 ticket result block for the install-gotcha
  workaround.

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
