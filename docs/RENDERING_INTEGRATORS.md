# Rendering Integrators — Selection Guide

RISE ships ten rasterizer chunks plus an interactive viewport. They
implement five distinct light-transport algorithms (PT, BDPT, VCM,
MLT, and the legacy shader-op pipeline) across two colour modes (RGB
"pel" and spectral) — and not all combinations exist. This doc
explains **which to pick for which scene**, the architectural split
between the two render pipelines, and the support matrix for the
optional features (path guiding, adaptive sampling, SMS, optimal MIS,
OIDN denoising).

It is a selection guide, not a tutorial. Per-parameter behaviour lives
in [src/Library/Parsers/README.md](../src/Library/Parsers/README.md);
algorithmic detail lives in [VCM.md](VCM.md), [SMS.md](SMS.md),
[MLT_POSTMORTEM.md](MLT_POSTMORTEM.md),
[MIS_HEURISTICS.md](MIS_HEURISTICS.md), and the integrator headers
themselves.

## 1. Two render pipelines

RISE has two coexisting render-loop architectures. New scenes should
use the **pure-integrator** pipeline whenever it covers the workload;
the **shader-dispatch** pipeline is retained for the configurations
only it supports.

### 1.1 Shader-dispatch (legacy / extensible)

The classical RISE pipeline. The rasterizer drives a `RayCaster`,
which evaluates a chain of `IShaderOp` operations attached to each
hit point via the `defaultshader` parameter. The shader-op chain is
where path tracing, ambient occlusion, photon-map gather, final-gather,
direct lighting, transparency, alpha-test, and SMS all live as
composable building blocks.

**Rasterizer chunks**: `pixelpel_rasterizer`,
`pixelintegratingspectral_rasterizer`.

**Pick this when** you need:
- Photon-mapping or final-gather hybrids
- Ambient occlusion as a standalone pass
- Custom shader-op chains (e.g. transparency + alpha test composed
  with PT)
- Anything that needs to run a non-PT integrator at hit points

### 1.2 Pure integrator (modern)

The rasterizer holds an integrator object directly
([`PathTracingIntegrator`](../src/Library/Shaders/PathTracingIntegrator.h),
[`BDPTIntegrator`](../src/Library/Shaders/BDPTIntegrator.h),
[`VCMIntegrator`](../src/Library/Shaders/VCMIntegrator.h)) and calls
it from the per-pixel loop. No shader-op chain. This is faster,
clearer, and the only path that wires up modern features (path
guiding, adaptive sampling, optimal MIS, OIDN).

**Rasterizer chunks**: `pathtracing_pel_rasterizer`,
`pathtracing_spectral_rasterizer`, `bdpt_pel_rasterizer`,
`bdpt_spectral_rasterizer`, `vcm_pel_rasterizer`,
`vcm_spectral_rasterizer`, `mlt_rasterizer`, `mlt_spectral_rasterizer`.

**Pick this when** the scene's lighting is well-served by PT, BDPT,
VCM, or MLT and you don't need a custom shader-op chain. The pure
PT rasterizers (`pathtracing_*_rasterizer`) are the closest replacement
for `pixelpel_rasterizer` with a default PT shader-op chain — they
ship better defaults, OIDN integration, and access to the optional-feature
matrix (§4).

The interactive viewport
([`InteractivePelRasterizer`](../src/Library/Rendering/InteractivePelRasterizer.h))
is a special case of the pure-integrator pipeline used by all three
GUI bridges (macOS / Windows / Android) — see
[INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md).

## 2. Quick decision tree

**Matrix-backed routing (2026-06-04 — [UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md) + [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)).** The Phase-1 wall-clock-normalized variance (σ²·T) measurement gives a default-and-route rule — **the policy the planned [`auto_rasterizer`](AUTO_RASTERIZER_DESIGN.md) will encode; until it ships, pick by hand per this map:**

- **Default → PT** (`pathtracing_pel_rasterizer`) — wins σ²·T on 10/13 converged classes (diffuse, glossy-metal, mixed, many-light, most env); **3–7× cheaper per sample** than BDPT, which outweighs BDPT/VCM's lower *raw* variance on the bulk.
- **Strong-indirect / glossy interreflection → BDPT** (`bdpt_pel_rasterizer`) — the only regime where BDPT's connections beat its 3–7× per-sample penalty: gi_spheres-class (**56× σ²·T** over PT), alchemists, env+mesh. Signal: indirect dominates direct; glossy bounces; enclosed geometry.
- **Caustic / refractive / dispersive → VCM** (`vcm_pel_rasterizer`) — the *only* integrator reaching SDS / dielectric-caustic transport (PT/BDPT miss **44–78%** of the energy). Accept its finite-radius bias + photon cost **here only**; contraindicated as a default (loses σ²·T 3–40× off-caustics; ±63–76% env/volume bias).
- **Spectral / dispersion** → the `*_spectral_` variant of the above (BDPT-spectral is HWSS-correct since the 2026-06-04 Bug-3 fix).
- **Scattering-medium and directional-light caveats for VCM (2026-08-13, [SUBMERGED_CAMERA_IOR_SEEDING.md](SUBMERGED_CAMERA_IOR_SEEDING.md))** — VCM/BDPT take no NEE and no merges at MEDIUM vertices, so light whose only route to a mid-volume scatter point is NEE (env/omni in-scatter through a participating medium) is under-counted (~15% of frame energy on the jellyfish scene class), and **VCM cannot sample directional lights at all** (no zero-exitance NEE loop, no photon emission — PT and BDPT both have the deterministic fallback).  On medium-heavy or sun-lit scenes, weigh these against VCM's caustic reach; PT is now a trustworthy reference on enclosed-camera scenes (the 2026-08-13 eye-ray IOR-seeding fix — pre-fix PT silently dropped transmission lobes when the camera sat inside a dielectric).

**Do NOT default to BDPT or VCM** — the data is decisive that PT is the efficient default on the bulk. The longer-form flowchart:

```
Need real-time interactive viewport?            → InteractivePelRasterizer (automatic; not a chunk)

Need spectral / dispersion / per-wavelength IOR?
  → Use the *_spectral_ variant of whichever choice below

Most paths reach lights directly?               → pathtracing_pel_rasterizer
                                                   (or pixelpel_rasterizer if you need a custom shader-op chain)

Caustics or specular chains carry significant energy?
  Mix of caustics + diffuse interreflection?    → vcm_pel_rasterizer (BDPT + photon merging)
  Caustics dominate, scene is glossy/dielectric? → vcm_pel_rasterizer
  Reflective/refractive chains, no merging needed? → bdpt_pel_rasterizer

Sparse important paths (light through keyhole, SDS, hard caustics
through long glass chains)?                     → mlt_rasterizer

Need photon-map gather, final-gather, AO, or shader-op composition? → pixelpel_rasterizer
```

## 3. Catalogue

The ten rasterizer chunks, grouped by algorithm.

### Path tracing (unidirectional)

| Chunk | Pipeline | Notes |
|---|---|---|
| `pixelpel_rasterizer` | shader-dispatch | RGB. Runs the `defaultshader` chain (PT shader-op + others) at every hit. The classic configuration. Use when composability matters more than raw PT throughput. |
| `pixelintegratingspectral_rasterizer` | shader-dispatch | Spectral analogue of `pixelpel_rasterizer`. RGB→SPD conversion happens in the painter pipeline.  **Soft-deprecated** — modern features (path guiding, adaptive sampling, optimal MIS, full inline OIDN AOV) are not wired here; new scenes should prefer `pathtracing_spectral_rasterizer`.  Retained for custom spectral shader-op chains; no removal date.  See [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.1–§2.5. |
| `pathtracing_pel_rasterizer` | pure integrator | RGB. Calls `PathTracingIntegrator` directly. Bypasses shader-op chain. **Default modern PT.** Wires OIDN with the filtered-film resolve correctly skipped (raw MC noise feeds OIDN). |
| `pathtracing_spectral_rasterizer` | pure integrator | Spectral. NM and HWSS modes both available.  Inline OIDN AOV wired 2026-06-02 (audit §2.6) — both the pure-integrator path and the shader-dispatch `pixelintegratingspectral_rasterizer` allocate AOV buffers and accumulate Albedo/Normal per camera sample; the `CollectFirstHitAOVs` retrace fallback is now bypassed when inline data is present.  Through-glass scenes (the canonical retrace-misnames-glass-as-white case) now record the through-glass surface albedo. |

### BDPT (bidirectional path tracing)

| Chunk | Pipeline | Notes |
|---|---|---|
| `bdpt_pel_rasterizer` | pure integrator | RGB. Generates eye + light subpaths, connects all (s,t) strategy pairs, MIS-weights via the **power heuristic (β=2)** — see [MIS_HEURISTICS.md](MIS_HEURISTICS.md). Strong on glossy interreflection and indirect specular. |
| `bdpt_spectral_rasterizer` | pure integrator | Spectral analogue.  Wires the full path-guiding parameter set (parser switched from a hand-rolled subset to `AddPathGuidingParams` on 2026-05-07; see [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.7).  Adaptive sampling wired on 2026-05-24 via `AddAdaptiveSamplingParams` — `adaptive_max_samples`, `adaptive_threshold`, `show_adaptive_map` (audit §2.8); Welford signal is XYZ.Y (CIE photometric luminance). |

### VCM (vertex connection and merging)

| Chunk | Pipeline | Notes |
|---|---|---|
| `vcm_pel_rasterizer` | pure integrator | RGB. BDPT connection strategies + photon merging in one MIS umbrella, MIS-weighted via the **balance heuristic (β=1)** — architecturally required by the Georgiev 2012 dVCM/dVC/dVM running-quantities recurrence; see [MIS_HEURISTICS.md](MIS_HEURISTICS.md). Adds `vc_enabled` / `vm_enabled` switches and `merge_radius` (0 = SPPM-style auto-radius reduction). The right pick whenever caustics carry meaningful energy. See [VCM.md](VCM.md). |
| `vcm_spectral_rasterizer` | pure integrator | Spectral analogue. |

### MLT (Metropolis light transport)

| Chunk | Pipeline | Notes |
|---|---|---|
| `mlt_rasterizer` | pure integrator | PSSMLT (Kelemen 2002) atop BDPT. Bootstrap finds high-contribution paths, Markov chains explore neighbourhoods. Use only when BDPT and VCM both fail to find the important paths — see the postmortem in [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) for when it actually wins (and when it doesn't). |
| `mlt_spectral_rasterizer` | pure integrator | Spectral analogue. |

Two MLT variants (MMLT, PathMLT) shipped briefly and were retired —
[MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) explains why.

### Interactive viewport (not a chunk)

[`InteractivePelRasterizer`](../src/Library/Rendering/InteractivePelRasterizer.h)
is constructed by the GUI bridges, not by parsing a chunk. Multi-level
adaptive scaling, no-throttle preview dispatch, idle refinement, and a
4-SPP polish pass on `OnPointerUp` give the editing-feel responsiveness;
the bridge swaps in the scene-declared production rasterizer when the
user clicks Render. Full architecture in
[INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md).

## 4. Optional-feature support matrix

Not every rasterizer wires every optional feature. The matrix below
reflects the helper-template invocations in
[`CreateAllChunkParsers()`](../src/Library/Parsers/ChunkParserRegistry.cpp);
the table in [Parsers/README.md](../src/Library/Parsers/README.md)
"Helper Templates" is the source of truth. ✓ = supported, ✗ = not
wired, partial = subset.

| Chunk | Path guiding | Adaptive sampling | SMS | Optimal MIS | OIDN denoise |
|---|---|---|---|---|---|
| `pixelpel_rasterizer` | ✓ | ✓ | (via shader-op) | ✓ | ✓ |
| `pixelintegratingspectral_rasterizer` ¹ | ✗ | ✗ | (via shader-op) | ✗ | (limited) |
| `pathtracing_pel_rasterizer` | ✓ | ✓ | ✓ | ✓ | ✓ (full filtered-film bypass) |
| `pathtracing_spectral_rasterizer` | ✗ | ✓ | ✓ | ✗ | ✓ |
| `bdpt_pel_rasterizer` | ✓ | ✓ | ✗ | ✗ | ✓ |
| `bdpt_spectral_rasterizer` | ✓ | ✓ | ✗ | ✗ | (limited) |
| `vcm_pel_rasterizer` | ✗ | ✓ | ✗ | ✗ | ✓ |
| `vcm_spectral_rasterizer` ² | ✗ | ✓ | ✗ | ✗ | (limited) |
| `mlt_rasterizer` | ✗ | ✗ | ✗ | ✗ | ✗ (default off — splat film) |
| `mlt_spectral_rasterizer` | ✗ | ✗ | ✗ | ✗ | ✗ (default off — splat film) |

¹ **`pixelintegratingspectral_rasterizer` is soft-deprecated.** It is
the legacy shader-dispatch spectral chunk; modern features (path
guiding, adaptive sampling, optimal MIS, full inline OIDN AOV)
require the per-integrator pipeline and are not wired here. New
scenes should prefer `pathtracing_spectral_rasterizer`. The chunk is
retained for custom spectral shader-op chains; no removal date.

² **VCM-spectral merging uses a luminance proxy** on a Pel-only
photon store — see [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
§3 for the correctness implications on dispersive caustics and the
per-wavelength-photon-store remediation plan.

A few cross-cutting facts to keep this matrix honest:

- **OIDN + filtered-film bypass** is implemented in
  `PixelBasedRasterizerHelper`; rasterizers that go through it inherit
  the bypass. MLT does its own splat/resolve loop and does not
  integrate with OIDN.
- **OIDN AOV "(limited)" on spectral rows** means: PT-spectral and
  pixelintegratingspectral have *no* inline AOV at all (rely on the
  `CollectFirstHitAOVs` retrace fallback, which records at first hit
  through glass / mirror).  BDPT-spectral and VCM-spectral *do* walk
  the eye subpath inline and call `BSDF->albedo(rig)` against a
  helper-populated `RayIntersectionGeometric` (matching the Pel-side
  pattern as of the 2026-05-07 spectral parity fix; the prior
  `BSDF->value(N,rig) * π` Lambertian-normal-incidence proxy was
  retired).  See [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
  §4 for the per-row decode and §2.11 / §2.17 for the proxy fix.
- **Adaptive sampling** integrates with PT (pel + spectral) and VCM
  (pel + spectral) but not BDPT spectral or MLT — BDPT spectral has
  not had the per-pixel Welford loop ported (deferred plumbing, see
  audit §2.8); MLT has non-pixel-local sample placement.
- **Path guiding** requires per-pixel directional density estimation
  via OpenPGL. The VCM merging pass and MLT mutation pass do not
  cooperate with that; they are not wired.  BDPT-spectral wires the
  full path-guiding param set (fixed 2026-05-07 — `pathguiding_learned_alpha`,
  `pathguiding_light_max_depth`, `pathguiding_complete_path_strategy_selection`,
  and `pathguiding_complete_path_strategy_samples` had previously been
  silently dropped by a hand-rolled descriptor; see audit §2.7).
- **SMS** is wired into PT (pel + spectral) only.  Not in MLT (no
  plumbing), not in VCM (merging handles caustics natively), not in
  BDPT — BDPT was wired through SMS historically; the integration
  was excised in 2026-05 because state-of-the-art renderers don't
  combine BDPT with SMS and the cross-strategy MIS overlap was
  structural complexity for no measurable variance gain.  See
  [CLAUDE.md](../CLAUDE.md) "High-Value Facts" for the removal entry.
- **Optimal MIS** (Kondapaneni 2019) is wired only in
  `pixelpel_rasterizer` (via the shader-op PT chain) and
  `pathtracing_pel_rasterizer`.  Both inherit the
  `OptimalMISAccumulator` allocation from `PixelBasedPelRasterizer`,
  and `PathTracingIntegrator` reads `rc.pOptimalMIS` from the
  per-pixel runtime context.  Spectral-integrating rasterizers
  (`pixelintegratingspectral_rasterizer`, `pathtracing_spectral_rasterizer`,
  BDPT-spectral, VCM-spectral) do not allocate the accumulator —
  `rc.pOptimalMIS` is null on the spectral integrator path.  BDPT
  and VCM (pel and spectral) parsers hard-fail on `optimal_mis*`
  lines as of Step 3 of the spectral parity audit (2026-05-07): the
  Kondapaneni single-step BSDF-vs-NEE formulation has not been
  extended to per-strategy-pair (BDPT) or to the dVCM/dVC/dVM
  recurrence (VCM), so accepting the params would silently drop
  them.  See [MIS_HEURISTICS.md](MIS_HEURISTICS.md) for why this is
  open research, and [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
  §2.4, §2.10 for the rejection rationale.
- **Path-tree branching at multi-lobe delta vertices** was removed in
  2026-05.  All integrators use stochastic single-lobe selection at
  Fresnel splits (matches PBRT/Mitsuba/Arnold/Cycles X) — see
  [CLAUDE.md](../CLAUDE.md) "High-Value Facts" for the rationale and
  the BDPT MIS-vs-tree mismatch that motivated removal.

## 5. Selection criteria — the long version

### 5.1 Pick PT (`pathtracing_pel_rasterizer`) when…

- Most paths reach a light with one or two bounces.
- The dominant transport is diffuse interreflection.
- You want OIDN denoising on a converged-or-nearly-converged image.
- You want adaptive sampling, path guiding, optimal MIS, or SMS —
  PT is the most feature-complete chunk.
- You want a low-variance preview that improves predictably with SPP.

The classic Cornell box and most architectural / product-viz scenes
are PT scenes. Use spectral PT
(`pathtracing_spectral_rasterizer`) when wavelength-dependent
behaviour matters (dispersion, fluorescence, narrow-band spectral
lights).

### 5.2 Pick BDPT (`bdpt_pel_rasterizer`) when…

- Significant energy travels through reflective / refractive chains.
- Indirect lighting from area sources matters more than direct.
- The geometry of the light source makes BSDF-sampled NEE inefficient
  (small luminaires far from the receiver, source occluded by a
  partial portal).
- You don't need merging — i.e. caustics aren't the dominant feature.

BDPT fixes the dim-cup-of-the-spotlight problem PT struggles with.
[scenes/Tests/BDPT/](../scenes/Tests/BDPT/) and
[scenes/Tests/UnifiedLighting/](../scenes/Tests/UnifiedLighting/)
have demonstrative scenes.

### 5.3 Pick VCM (`vcm_pel_rasterizer`) when…

- Caustics, godrays, or specular-diffuse-specular paths carry
  meaningful energy.
- The scene has glass, water, polished metal, or any
  delta-reflection chain that produces concentrated incident energy.
- BDPT renders are blotchy / fireflies-rich after long render times
  (a sign that the connection strategies aren't reaching the caustic
  paths).

VCM ships full BDPT plus photon merging under one MIS umbrella —
it strictly subsumes BDPT in expressive power but pays for the
photon-tracing pass, the kd-tree build, and the per-pixel merge
queries. For a diffuse-dominated scene, BDPT or PT beats VCM on
wall-clock per-pixel quality. See [VCM.md](VCM.md) for the design
rationale and Veach-transparency handling.

### 5.4 Pick MLT (`mlt_rasterizer`) when…

- BDPT and VCM both fail to find the important paths after long
  render times (paths are extremely sparse / narrow / occluded).
- The scene has SDS (specular-diffuse-specular) chains that
  ordinary connection strategies miss.
- You can tolerate the lack of per-pixel convergence and OIDN
  integration.

MLT is rarely the right answer. [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md)
documents two MLT variants that shipped and were retired because the
parameter regimes where they actually win are narrow. Read that
postmortem before reaching for MLT.

### 5.5 Pick `pixelpel_rasterizer` when…

- You need a custom shader-op chain (AO + transparency + alpha test +
  PT, photon-map gather, final-gather, etc.).
- You're rendering something other than path tracing at hit points.
- You're maintaining a legacy scene file that already uses
  `defaultshader` chains.

For straight PT in a new scene, prefer `pathtracing_pel_rasterizer` —
it has better defaults, OIDN bypass, and the modern feature matrix.

## 6. Spectral mode notes

Every algorithm in §3 has both an RGB ("pel") and a spectral chunk.
The spectral chunks accept the same algorithm-level parameters as
their RGB counterparts plus the spectral-core helper
(`spectral_samples`, `nmbegin`, `nmend`, `num_wavelengths`, `hwss`).

Two practical considerations:

- **HWSS (Hero-Wavelength Spectral Sampling)** decorrelates wavelength
  variance by sampling a primary "hero" wavelength per ray and
  weighting the others through a velvet noise process. Defaults to off;
  set `hwss TRUE` on any spectral rasterizer for dispersive scenes.
- **`pixelintegratingspectral_rasterizer` is the shader-dispatch
  variant and is soft-deprecated.** Modern optional features (path
  guiding, adaptive sampling, optimal MIS, full inline OIDN AOV)
  are not wired into the shader-dispatch pipeline.  Prefer
  `pathtracing_spectral_rasterizer` for new scenes; the legacy
  chunk stays around for custom spectral shader-op chains.

## 7.1 The camera path vertex under a finite aperture

How BDPT and VCM treat the camera end of a path when the camera is a
`thinlens_camera` rather than a pinhole.  Written up because debt 28
(below) made it a live question and the answer is easy to get wrong in
the "obvious" direction.

**Three kinds of camera vertex, and only two of them matter to MIS.**

| camera | position | direction | t==1 strategy |
|--------|----------|-----------|---------------|
| `pinhole_camera`, `fisheye_camera` | a point | finite density | valid |
| `thinlens_camera` | a region of AREA | finite density | valid |
| `orthographic_camera` | a point per pixel | **Dirac delta** | **skipped** |

Only the orthographic camera is special-cased.  It is the
importance-side analogue of a directional light: a non-specular light
vertex has zero density of scattering into its single parallel
direction, so `BDPTCameraUtilities::IsDeltaDirection` returns true for
it, `GenerateEyeSubpath` marks the camera vertex `isDelta`, the t==1
sites skip, and `MISWeight`'s eye-side walk excludes the (phantom)
strategy from every other strategy's denominator.

**A finite aperture is NOT a special case.**  The camera vertex stays
non-delta, its `pdfFwd` stays 1, and nothing in `MISWeight` or in
VCM's `wLight` / `cameraPdfA` changes.  The reason is that the camera
vertex's POSITIONAL density is the same under every strategy — each
one samples that vertex from the same aperture with the same density
`1/A_lens` — so it cancels out of every pdf ratio the MIS weight is
built from.  Concretely:

- **BDPT.**  `MISWeight`'s eye-side walk runs `for j = t-1; j > 0`, so
  `eyeVerts[0]`'s own `pdfFwd`/`pdfRev` are never read; vertex 0
  enters only through the `eyeVerts[j-1].isDelta` gate at `j == 1`,
  which decides whether the t==1 strategy is counted at all.  This is
  PBRT-v4's structure exactly (`bdpt.cpp`'s `MISWeight` loops
  `for (int i = t - 1; i > 0; --i)` and never touches
  `cameraVertices[0].pdfFwd`), and PBRT likewise does not branch on
  `lensRadius == 0` anywhere in its BDPT MIS.
- **VCM.**  `cameraPdfA = camPdfDirSA · cosAtLight / dist²` is the
  ratio quantity, i.e. the camera-side density of the light vertex
  RELATIVE to the t==1 strategy's own — the shared `1/A_lens` has
  already divided out.  `InitCamera`'s `dVCM = N / cameraPdfW` uses
  the conditional directional pdf, which for a thin lens with no
  focal-plane tilt does not depend on which aperture point the ray
  left from (the film-sample → focus-point map is a uniform
  magnification, so the area Jacobian is the same from every point on
  the aperture).

PBRT's own way of putting this is worth knowing because it explains
RISE's `cos³` vs PBRT's `cos⁴`: PBRT treats a pinhole as a lens of
area **1** (`lensArea = lensRadius != 0 ? πr² : 1`), so the same
`SampleWi` code path and the same MIS structure serve both cameras.
RISE instead folds the aperture cosine and `1/p_A` into
`BDPTCameraUtilities::Importance` (see its contract in
`CameraUtilities.h`), which makes the pinhole's importance
`d²/(pixelAR·W·H·cos³θ)` where PBRT's raw `We` is
`1/(A·A_lens·cos⁴θ)` — the same number, with the cosine absorbed.
After debt 28 the thin lens's folded importance is that SAME
expression, aperture area and all having cancelled.

**What DOES change for a finite aperture:**

1. The t==1 connection endpoint is a sampled aperture point, not
   `camera.GetLocation()` — shadow ray, direction, distance and
   connection transmittance all use it.
2. The splat's raster position is computed THROUGH that point
   (`RasterizeThrough` → `ThinLensCamera::RasterFromLensPoint`), which
   is what gives light-traced contributions the eye rays' depth of
   field.
3. BDPT's eye-subpath camera vertex 0 sits at `cameraRay.origin` —
   the aperture point `GenerateRay` actually sampled — rather than at
   the lens centre.  Weighting a path whose first vertex is somewhere
   the ray never touched is wrong by the aperture radius; it is a
   ~1e-3 relative perturbation of the first edge's `dist²`, small but
   not principled.  `IORStackSeeding::SeedFromPoint` follows the same
   point.

No MIS heuristic changed: BDPT is still power-2, VCM still balance
(see [MIS_HEURISTICS.md](MIS_HEURISTICS.md), and do not propose
"fixing" that asymmetry).

## 7. Known limitations

- **RESOLVED 2026-09-04 (chip 4 / task_93ff4a8a).** This entry used to
  read "BDPT/VCM over-count a FLAT, ZERO-THICKNESS full-sphere
  transmissive material by 100-350x" (`IMaterial::ScattersFullSphere()
  && CouldLightPassThrough()` — a `weave_material` under `transmission
  thin`), diagnosed against the unguarded vertex-connection geometric
  term `G = cosA·cosB/dist²` in
  [`BDPTUtilities::GeometricTerm`](../src/Library/Utilities/BDPTUtilities.h)
  (floored only at `dist² < 1e-20`), theorised to diverge when an
  eye-subpath vertex on the front face and a light-subpath vertex on the
  back face of the same infinitesimally-thin surface land arbitrarily
  close together. **That measurement does not reproduce.** Re-run
  against the exact scene it was taken on
  (`tests/FabricRenderTest.cpp::TestBacklitSheerCurtain`) with
  `oidn_denoise FALSE`, BDPT/PT settles at 0.899-0.900 and VCM/PT at
  0.932-0.933, stable across five independent seed bases at 256 and 1024
  spp, on both the MEAN and the MAX-pixel luminance (no residual
  firefly); a harsher touching-distance mesh-area-light stress scene
  (`TestTouchingAreaLitCurtainAllIntegrators`, deliberately shaped to
  maximise same-object near-coincident vertex connections) gives
  BDPT/PT = VCM/PT = 1.01 with 0 fireflies.
  The root cause was the OTHER side of an unrelated, same-day fix: PT's
  own NEE shadow ray toward a light behind the curtain had no epsilon
  bump of its own and relied entirely on
  `RayBilinearPatchIntersection`'s self-hit rejection, which pre-fix
  (commit `d01a320a`, docs/CLOTH_FABRIC_DESIGN.md §15 debt 21) accepted
  ANY positive `dRange` — spuriously self-occluding the large majority
  of those shadow rays and deflating PT's OWN reference value by ~370x
  on this exact scene. BDPT's and VCM's connection-visibility shadow
  rays were never meaningfully exposed to that bug: both apply their own
  `BDPT_RAY_EPSILON` / `VCM_RAY_EPSILON` (`= 1e-6`, six orders of
  magnitude above the ~1e-12 FP-noise floor debt 21 measured) via
  `Ray::Advance()` before casting, so their absolute output barely moved
  across the debt-21 fix — the "100-350x" figure was a stable BDPT/VCM
  number compared against a broken PT reference, the "PT may be the
  broken one" trap
  [bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)'s step 0
  pre-flight names. The theoretical risk this entry described
  (`GeometricTerm` has no PRINCIPLED floor beyond the generic
  `dist < BDPT_RAY_EPSILON` / `distSq < 1e-20` checks) remains
  architecturally true — a future full-sphere material on a code path
  lacking BDPT/VCM's own epsilon-bump protection could still, in
  principle, trigger it — but it is not, and evidently never was, what
  this scene's measurement showed. `AutoRasterizer`'s Tier-1 routing
  exclusion for this material class has been LIFTED (it routes like any
  other `CouldLightPassThrough()` material now) and the one-time BDPT/VCM
  warning has been removed.  The smaller, separate ~7-10% BDPT/VCM-under-PT residual on the
  delta-point-light backlit scene specifically was also fully root-caused
  and resolved (2026-09-04): `BDPTIntegrator.cpp` subpath vertex
  connectibility classification was checking `scattered[i].isDelta`
  alone; on mixed delta+continuum materials (`WeaveMaterial` with
  `transmission thin`, `gap > 0`), stochastically drawing the delta
  gap lobe marked the vertex non-connectible and dropped NEE ($s=1$)
  on a `gap` fraction of camera rays, producing a $(1 - \text{gap})$
  NEE deficit ($(1 - \text{gap})^2$ vs PT's $1 - \text{gap}$). Fixed
  by checking `ri.pMaterial->GetBSDF()` so surfaces with continuous
  BSDFs remain connectible regardless of the sampled continuation lobe.
  BDPT/PT is now 1.0000 across all gap values. Full writeup:
  [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) §15 debt 23.

  That fix left VCM reading **+3.0 %** over PT on the same scene, which
  turned out to be a SECOND, independent, and NOT fabric-specific bug
  (§15 debt 24, also fixed 2026-09-04): `VCMIntegrator::EvaluateNEEImpl`
  zeroed **both** MIS alternatives at a DELTA light. Zeroing `wLight` is
  correct — BSDF sampling cannot land on a Dirac position by chance —
  but `wCamera` carries the *light-side* strategies (the t=1
  light-tracing splat, interior connections, merges), and those do not
  land on the light vertex, they **start** there and sample an emission
  **direction**, whose solid-angle density is perfectly ordinary
  (`PointLight` 1/4π, `SpotLight` the cone density). NEE therefore took
  weight 1 while the splat independently contributed its own
  `1/(1+wLight)` share of the very same path. Instrumented mean MIS
  weight per strategy on the gap-0.1 curtain: before, NEE 1.00000 + VM
  0.00815 + splat 0.02963 = **1.0378**; after, 0.96312 + 0.00816 +
  0.02963 = **1.0009**. A plain Lambertian quad under an omni light
  showed the identical +2.99 %, and any AREA light showed none, which is
  what proves it was never about the weave. Fixed by computing
  `camFactor` as the emission-direction AREA density
  `emissionDirPdfSA · cosAtEye / dist²` — algebraically identical to the
  SmallVCM spelling for non-delta lights (the `pdfPosition` and
  `cosAtLight` factors cancel), but free of the two placeholder
  quantities (`directPdfW := 1`, `cosAtLight := 1`) that made the old
  spelling unusable for delta lights. VCM/PT is now 1.000 ± 3e-4 on
  every delta-lit topology measured, with vertex merging on or off;
  `VCMStrategyBalanceTest` topology A moved 1.0082 → 0.9975;
  `EnvLightBalanceTest` stays 116/116 with no band moved.

  **Two findings surfaced by the same round, and a third that closing
  the first unmasked** (§15 debts 25, 26 and 27).
  Debt 25 — a CLOSED solid whose material is a thin-transmissive weave
  read BDPT/VCM ≈ **0.17×** PT while free-standing weave planes read
  1.000 — is **CLOSED 2026-09-05**: PT was the broken reference, not
  BDPT/VCM.  `box_geometry` reported the ray origin's own face as a
  ~1e-12 self-root to PT's unadvanced NEE shadow rays, which either
  discarded the whole box (light leaked through the closed box, PT 3.5×
  too bright) or self-occluded (light inside, PT 0.44×); BDPT/VCM advance
  their shadow rays 1e-6 and were right all along.  Fixed in
  `BoxGeometry::DropSelfHitRoot` (commit 40e78b69; BDPT/PT 0.28 → 0.97,
  the six-face `clippedplane` twin of the same box reads 0.99).  The
  fix unmasked **debt 27** (OPEN): with `gap > 0` and the light OUTSIDE
  a two-layer weave, PT reads UNDER BDPT/VCM by 1.28–1.30× at gap 0.1 and
  1.55× at gap 0.3 on box and free-standing planes alike, because the
  path light → straight through the far layer's delta gap → near layer's
  continuum lobe is reachable by light tracing but not by PT's binary
  NEE — a PT strategy gap the auto-router has no rule for yet.  Debt 26
  — the legacy `pixelpel_rasterizer` reading **0.0416** on a gapped weave
  in front of an area light where the modern `pathtracing_pel_rasterizer`
  reads **0.1024** at equal spp (BDPT 0.1026, VCM 0.1024) — is
  **RESOLVED 2026-09-05, and it was never a rasterizer defect.**  The
  legacy rasterizer executes the scene's `standard_shader` chain
  literally, and that scene declared `DefaultDirectLighting` alone.
  Neither `DirectLightingShaderOp` nor the `DefaultEmission` op
  `Job::AddStandardShader` auto-prepends declares `RequireSPF()`, so
  `StandardShader::Shade` never calls `ISPF::Scatter` and NO continuation
  ray of any kind is cast — the weave's delta gap lobe is not merely
  unfollowed, it is never sampled, which is also why `max_recursion` is
  inert.  Adding one op, `DefaultRefraction`, moves the legacy number to
  0.10148, i.e. **0.991** of the modern PT; the missing term is a top-hat
  of height exactly `gap × L_emitter` (0.22 % agreement in the interior)
  over the emitter's silhouette.  It is not weave-specific: a plain
  `dielectric_material` pane in the same scene renders **0.000000** under
  the direct-lighting-only chain and 0.61119 with `DefaultRefraction`
  added (modern PT 0.61116).  That is the legacy shader-op contract —
  a `DefaultDirectLighting`-only chain is a *direct lighting* render by
  construction — and the shipped legacy corpus already honours it
  (`scenes/FeatureBased/Caustics/pool_caustics.RISEscene` pairs
  `DefaultDirectLighting` with `DefaultRefraction`).
  **Consequence for measurement**: never use a legacy `pixelpel_*`
  rasterizer as a TRANSPORT reference unless the scene's shader chain
  covers every transport mode under test — even with `DefaultRefraction`
  added, the chain above is still 0.980 of the modern PT at gap 0, purely
  from indirect bounces it does not follow.
  `tests/BDPTStrategyBalanceTest.cpp`, which used `pixelpel_rasterizer`
  as its PT reference, now uses `pathtracing_pel_rasterizer` and gained
  the area-lit gapped-weave topology (topology F, BDPT/PT 0.9948) that
  the old reference could not host; every pre-existing topology agrees
  more tightly than before and no tolerance was loosened.

- **Debt 28 (RESOLVED 2026-09-11) — BDPT/VCM blow-ups on three shipped
  signal showcases.  Root cause: the t==1 light-tracing connection
  never sampled the camera's entrance APERTURE.  NOT
  signal-attributable.**

  Found by the showcase layer of
  `tests/SignalIntegratorConsistencyTest.cpp` (the money test of
  [SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md](SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md)
  §6.2) and reproduced through the CLI on master `185b0d5f`.

  **Root cause.**  `BDPTCameraUtilities::ImportanceThinLens` returned
  PBRT-v4's thin-lens importance

      We = d² / (A_lens · W · H · cos⁴θ)

  and the `1/A_lens` in it exists for exactly one reason: the t==1
  connection is supposed to SAMPLE a point on the aperture with
  density `1/A_lens`, and the estimator's `1/pdf` cancels it (PBRT-v4
  §16.1, `PerspectiveCamera::SampleWi`, whose pdf is
  `dist²/(cosθ·A_lens)`).  No caller sampled.  `RasterizeThinLens`
  projected through the lens CENTRE, and every t==1 site —
  `BDPTIntegrator.cpp`'s live `t == 1` branch and the dead `t == 0`
  one, plus `VCMIntegrator.cpp`'s `SplatLightSubpathToCameraImpl` —
  connected to `camera.GetLocation()` and multiplied by that
  importance anyway.  Each splat therefore came out

      1 / (cosθ · A_lens)

  too bright: **2.5 × 10⁵** for tidal_stones' 50 mm f/22 lens in a
  metres scene, 2.7 × 10⁶ for a 15.1 mm f/22 one.  What reached the
  image was that factor times the strategy's MIS weight, which is why
  the symptom ranged from 1.02× (shelf_bunny BDPT) to 4558×
  (shelf_bunny VCM), and why raising `fstop` to 1e5 or swapping in a
  `pinhole_camera` made it vanish.  The correlation is exact: the
  three blown-up showcases are precisely the three that carry a
  `thinlens_camera`; `plank_closeup`, the one that did not blow up,
  is the one with a `pinhole_camera`.

  **The fix** (PBRT-v4 §16.1, adapted to RISE's convention that
  `Importance` folds in the aperture cosine and `1/p_A`):

  1. `ThinLensCamera` grew the finite-aperture surface the integrators
     need — `GetApertureWorldArea` (blade shape and anamorphic squeeze
     included; the `pixelAR` pre-stretch cancels), `SampleLensPoint`
     (the SAME `SampleAperture` call `GenerateRay` makes, so
     light-traced bokeh is eye-traced bokeh), `LensPointToWorld`,
     `GetImagePlanePixelDensity`, and `RasterFromLensPoint`, the exact
     inverse of `ComputeWorldDirection`: intersect the (lens point →
     world point) ray with the plane of focus, then project that
     focus-plane point back through the lens CENTRE onto the sensor.
  2. Every t==1 site draws an aperture point
     (`BDPTCameraUtilities::SampleAperture`), connects the light vertex
     to THAT point (shadow ray, direction, distance, transmittance),
     and rasterizes THROUGH it (`RasterizeThrough`).  That is what
     gives the light-traced layer the depth of field the eye-traced
     layer has.
  3. `ImportanceThinLens` becomes `We · cosθ · A_lens =
     d²/(pixelAR · W · H · cos³θ)` — **the aperture area cancels
     completely**, and the result is algebraically the pinhole's, as
     it must be: a lens trades depth of field for noise, not exposure.
     The pinhole limit is therefore continuous with no special case,
     which retired a `reinterpret_cast<const PinholeCamera*>` on a
     `ThinLensCamera` (undefined behaviour that also read a pinhole's
     fov-baked `mxTrans` off a thin lens's scene-unit one).
     `PdfDirectionThinLens` picked up the same missing `1/pixelAR`.
  4. `RasterizeThinLens` is re-expressed as `RasterFromLensPoint` at
     the origin, so the lens-centre projection now inverts lens SHIFT
     and focal-plane TILT instead of ignoring them.
  5. BDPT's eye-subpath camera vertex 0 sits at `cameraRay.origin`
     when the aperture is finite (see §7.1 below), not at
     `GetLocation()`.

  **Before / after**, 160×120 / 16 spp, `oidn_denoise FALSE`,
  `pixel_filter box`, EXR `Rec709RGB_Linear`, the scene's rasterizer
  chunk swapped and everything else as shipped.  Means are the R
  channel.  Rows marked `ts` keep tidal's `transparent_shadows TRUE`,
  which only the PT chunk accepts — see the residual note below.

  | scene | integrator | pre-fix | post-fix | PT | post-fix ratio |
  |-------|-----------|---------|----------|-----|----------------|
  | `tidal_stones` f/22, ts | BDPT | 45.4603 | 0.0689 | 0.1344 | 0.51× |
  | `tidal_stones` f/22, ts | VCM  | 222.4264 | 0.1422 | 0.1344 | 1.06× |
  | `tidal_stones` f/2.8, ts | BDPT | 1.2359 | 0.0688 | 0.1345 | 0.51× |
  | `tidal_stones` f/2.8, ts | VCM  | 3.7425 | 0.1420 | 0.1345 | 1.06× |
  | `tidal_stones` f/22, no ts | BDPT | — | 0.0687 | 0.0675 | **1.02×** |
  | `tidal_stones` f/22, no ts | VCM  | — | 0.1425 | 0.0675 | 2.11× |
  | `shelf_bunny` f/16 | BDPT | 0.9334 | 0.9336 | 0.9347 | **1.00×** |
  | `shelf_bunny` f/16 | VCM  | 4193.37 | 0.9332 | 0.9347 | **1.00×** |
  | `pavilion_colonnade` f/16 | BDPT | 0.2770 | 0.2770 | 0.2725 | 1.02× |
  | `pavilion_colonnade` f/16 | VCM  | 217.24 | 0.2772 | 0.2725 | 1.02× |

  A `pinhole_camera` control at tidal's framing is unmoved by the fix
  (PT 0.0456 / BDPT 0.0443 / VCM 0.0468 before and after, all inside
  run-to-run noise), and the f/2.8 rows now give the same ratios as
  the f/22 ones — the defect's `1/A_lens` signature is gone.

  **Tidal's remaining BDPT 0.51× is `transparent_shadows`, not an
  integrator disagreement.**  That parameter is accepted only by
  `pathtracing_pel_rasterizer`; tidal's water surface needs it (the
  scene's own header calls it MANDATORY) and the BDPT/VCM chunks have
  no equivalent, so the swapped-rasterizer comparison is not
  apples-to-apples.  Turn it off on the PT side and PT reads 0.0675
  against BDPT's 0.0687 — **1.02×**.  VCM reads 1.06× of the
  transparent-shadows PT because its merges recover the refracted
  light through the water that a PT without transparent shadows misses
  (2.11× against that weaker reference).  Nothing here is a camera or
  MIS defect; do not "fix" it.

  **`plank_closeup`'s VCM 0.55× is unrelated and still OPEN.**  That
  scene uses a `pinhole_camera`, so debt 28 never touched it; the
  deficit is the known env-IBL VCM bias class (CLAUDE.md's env-IBL
  entry, IMPROVEMENTS.md §12).

  **Depth of field really is consistent now, not just the mean.**  On
  a synthetic scene built to isolate it (a 100° thin lens at 6 m, the
  plane of focus 5.97 m in front of the receiver, a 4.8-pixel circle
  of confusion), the per-pixel distance between the defocused and the
  FOCUSED render of the same integrator is 5.2e-3 (PT), 5.5e-3 (BDPT)
  and 6.0e-3 (VCM), while the distance between integrators at the same
  focus is 2.9e-4 (defocused) and 5.9e-5 (focused) — focus state
  dominates integrator identity by 18–100×, and each integrator's blur
  signature is the same size as PT's.  A centre-rasterized splat layer
  would show up as BDPT/VCM sitting much closer to the FOCUSED image.

  **Other cameras audited.**  Pinhole: unchanged (`SampleAperture`
  reports a point aperture at `GetLocation()`, `RasterizeThrough`
  falls through to `Rasterize`, neither importance branch was
  touched).  Orthographic: it is a delta-DIRECTION camera, so its t==1
  strategy is skipped outright and none of this reaches it; its
  per-pixel ray-origin offset is deliberately still not reflected in
  the camera path vertex.  Fisheye: **clean** — no aperture of
  non-zero area, `Rasterize` and `Importance` read the same direction
  through the same inverse matrix, and its importance is per SOLID
  ANGLE rather than per unit lens area, so there is no `cosθ · A_lens`
  fold to get wrong.  Pinned by
  `tests/CameraImportanceTest.cpp::TestFisheyeFilmResponse`, which
  integrates a unit-radiance sphere to 1.000166 against the closed
  form 1.  One narrower, PRE-EXISTING and UNFIXED issue was found
  there: the fisheye's pixel solid angle `scale²/(W·H·cosAngle)` is
  measured in the camera's pre-stretch local frame while `mxTrans`
  applies `Stretch(pixelAR,1,1)` to the direction, so at
  `pixelAR != 1` the world-space solid angle per pixel differs by a
  direction-dependent Jacobian.  It is not debt 28's missing aperture
  sample (the thin lens carried the constant-factor version of the
  same thing, which this arc did fix); no in-tree scene pairs a
  fisheye with non-square pixels, and a correct fix needs the full
  Jacobian of `normalize ∘ Stretch`, not a constant.

  **Guards.**  `tests/CameraImportanceTest.cpp` (new, 956 closed-form
  checks, no renders, ~1 s) pins the camera-side algebra: the aperture
  area IS one over the sampling density, the inverse projection
  round-trips ray generation to 6e-14 px, the circle of confusion is
  exactly zero on the plane of focus and exactly the analytic radius
  off it, importance and pdf equal the matched pinhole's at every
  f-stop, and a uniformly radiating plane filling the frustum
  integrates to a film response of exactly 1.  End to end,
  `BDPTStrategyBalanceTest` topologies G/H and
  `VCMStrategyBalanceTest` topologies D/E add f/22-focused and
  f/2.8-defocused thin-lens rows; red-proofed on the pre-fix tree at
  BDPT 1449× / 24.6× and VCM 404 511× / 6701× over PT, now 0.974 /
  0.987 and 0.979 / 0.989.  Those topologies are deliberately NOT a
  copy of the suites' pinhole ones: BDPT's t==1 MIS weight is set by
  `π·d²·r²/D²` (camera-side over light-side area density at the light
  vertex), which the pinhole topologies' 30°/3.5 m/4 m geometry puts
  near 1.5 × 10⁴ — t==1 weight ~5e-9, at which a 2.5 × 10⁵× inflated
  splat moves the mean by 1e-3 and the bug hides completely (measured
  BDPT/PT = 1.0010).

## 8. Cross-references

- Per-parameter reference for each rasterizer chunk (and the
  `direct_clamp`, RR, max-bounce parameters shared across them):
  [src/Library/Parsers/README.md](../src/Library/Parsers/README.md)
- VCM design and Veach-transparency handling: [VCM.md](VCM.md)
- SMS solver and constraint formulations: [SMS.md](SMS.md)
- MLT decision history: [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md)
- Path guiding (OpenPGL) integration:
  [src/Library/Utilities/PathGuidingField.h](../src/Library/Utilities/PathGuidingField.h)
- Optimal MIS: [src/Library/Utilities/OptimalMISAccumulator.h](../src/Library/Utilities/OptimalMISAccumulator.h),
  Kondapaneni et al. 2019.
- OIDN integration audit and the FilteredFilm bypass invariant:
  [OIDN.md](OIDN.md), [ARCHITECTURE.md](ARCHITECTURE.md)
- Interactive viewport architecture and platform parity:
  [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md)
- Light selection and MIS interactions:
  [LIGHTS.md](LIGHTS.md), specifically §5.4
- Materials and BSDFs: [MATERIALS.md](MATERIALS.md)
- BDPT/VCM MIS-balance failure modes:
  [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)
- MIS heuristic choice per integrator (power vs balance, why
  RISE's BDPT/VCM asymmetry matches PBRT/Mitsuba/SmallVCM):
  [MIS_HEURISTICS.md](MIS_HEURISTICS.md)
