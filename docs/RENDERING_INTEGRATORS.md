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

## 6.1 The camera path vertex under a finite aperture

How BDPT and VCM treat the camera end of a path when the camera is a
`thinlens_camera` rather than a pinhole.  Written up because debt 28
(below) made it a live question and the answer is easy to get wrong in
the "obvious" direction.

**Three kinds of camera vertex, and only two of them matter to MIS.**

| camera | position | direction | t==1 strategy |
|--------|----------|-----------|---------------|
| `pinhole_camera`, `fisheye_camera` | a point | finite density | valid |
| `thinlens_camera` | a region of AREA | finite density | valid |
| `orthographic_camera` | a point per pixel | **Dirac delta** | **skipped** (in BDPT since the IsDeltaDirection fix; in VCM since 2026-09-11 — see the orthographic note in debt 28 below) |

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
  NEE — a PT strategy gap the auto-router has no rule for yet.

  **New debt-27-class data point (debt 28 review round 2, supervisor
  ruling on `SignalIntegratorConsistencyTest`'s masked layer):**
  `tidal_stones` — a scene with an omni light refracted through
  dielectric water onto submerged stones — reads BDPT **1.02×** PT on
  the WHOLE image, comfortably inside every existing band, but **≈2.7×**
  PT (2.78–3.01× across two runs) on just the masked ~13.5 % darkest
  pixels (the caustic-lit region under the water); VCM reads **2.05×**
  PT whole-image and **≈57.5–58×** PT on that same masked set.  Same
  mechanism as the weave case: a delta light's refraction through the
  dielectric water reaches those pixels by light-tracing (BDPT's t<n
  strategies, VCM's connections/merges) but not by PT's NEE, which can
  only reach a delta light via a straight, unoccluded shadow ray — so
  the whole-image ratio, dominated by the 86.5 % of unmasked pixels
  where both integrators agree, hides a large localized gap the masked
  layer was specifically built to catch.  This is what makes PT an
  invalid reference for `SignalIntegratorConsistencyTest`'s tidal masked
  row (see that test's header and `RunLayer2Showcase`'s
  reference-completeness gate): BDPT's and VCM's own masked E/B
  self-ratios disagree with EACH OTHER by an amount that itself
  straddles a 20 % band (−0.1996 to −0.2354 across runs) — evidence the
  57×-vs-2.7× reference-incompleteness gap is not just "PT is missing
  energy uniformly," but that BDPT and VCM recover DIFFERENT amounts of
  that unreachable transport from each other, an open question this
  entry does not resolve — tracked as its own item, **debt 30** below,
  since it is a BDPT-vs-VCM disagreement rather than a PT strategy gap.
  *(Debt 30 is now RESOLVED — see its entry: the disagreement turned out
  to be BDPT's structural S-D-S gap, so "BDPT and VCM recover different
  amounts" is right, and the reason is that BDPT recovers none of it.)*
  Debt 26
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
     when the aperture is finite (see §6.1 above), not at
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

  A `pinhole_camera` control at tidal's framing (`fov 39.6` =
  2·atan(18/50), same placement, the PT chunk keeping
  `transparent_shadows TRUE`) is unmoved by the fix — PT 0.134 / BDPT
  0.079 / VCM 0.141 before and after, the supervisor's pre-fix run and
  reviewer C's post-fix reproduction agreeing to three figures (an
  earlier draft of this sentence quoted numbers from a different framing
  and was corrected in review) — and the f/2.8 rows now give the same
  ratios as the f/22 ones — the defect's `1/A_lens` signature is gone.

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
  plane of focus 5.97 m in front of the receiver, a 2.4-pixel circle
  of confusion — 1.2 px radius; an earlier draft said 4.8 px, from an
  `(S1 − f)` that reduces to `S1` once the pixel pitch is taken at the
  ACTUAL image distance `v = f·S1/(S1 − f)`), the per-pixel distance between the defocused and the
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
  strategy is skipped outright and none of debt 28's aperture work
  reaches it; its per-pixel ray-origin offset is deliberately still not
  reflected in the camera path vertex.  **But "skipped outright" was
  only true of BDPT.**  Writing the orthographic mirror of
  BDPTStrategyBalanceTest's topology D into `VCMStrategyBalanceTest`
  (topology G, 2026-09-11) found VCM's support broken two ways:
  `SplatLightSubpathToCameraImpl` had no `IsDeltaDirection` guard at
  all, and the camera vertex's `emissionPdfW` — which `InitCamera`
  turns into `dVCM = N / cameraPdfW`, the MIS mass reserved for the
  t==1 strategy — carried `PdfDirection`'s orthographic return
  `1/A_image`, an AREA density where the recurrence wants a
  solid-angle one.  For a delta-direction camera that strategy does not
  exist, so the value must be **0** (the mirror of the `isDelta` flag
  BDPT already sets on the same vertex, and what SmallVCM does on the
  light side for a delta light).  Measured VCM mean / PT mean on
  topology G: 0.077 with both defects, 0.0007 with the guard alone
  (worse — the phantom splat had been carrying nearly all of what
  little energy VCM produced), 1.082 with `dVCM = 0` alone (that 8.2 %
  excess IS the phantom splat at full weight, the guard's own
  red-proof), **1.005 with both**.  **The red-proof's 8.2 % sat only
  0.2 percentage points outside the test's then-shared 8 % mean band
  (A2 P2-1, debt 28 review round 2) — honestly too close to call a
  reliable red-proof on its own.**  A 5-run re-measurement of that
  exact red-proof state (guard disabled, `dVCM = 0` fix kept) gave
  8.098 %, 8.176 %, 8.075 %, 8.155 %, 8.238 % (mean 8.148 %, sample
  stddev ≈0.065 pp, relative spread <1 %) — tight enough that the 0.2 pp
  margin was a real coincidence of magnitude, not measurement noise
  that could occasionally push the row to a false pass.  `topology G`
  now asserts against its own tighter band, 4 % mean (same p99/max as
  `kStrictTolerances`) — the shipped 1.005 sits comfortably inside it
  while the red-proof now misses by ~4 pp instead of 0.2.  BDPT is bit-unchanged: the zeroing
  touches only the VCM post-pass field, while BDPT's own walk keeps
  using the local `pdfCamDir`.  Fisheye: **clean** — no aperture of
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

  **NEW, OPEN, and a direct consequence of this fix —
  `SignalIntegratorConsistencyTest`'s tidal MASKED BDPT row became
  assertable, and it FAILED until round 3's reference-completeness gate
  excluded it (it is now a counted REFERENCE INCOMPLETE skip — BDPT's
  masked neutral-variant ratio to PT is ≈ 3×, so PT cannot referee it —
  and the BDPT-vs-VCM gap behind it is debt 30, now RESOLVED: BDPT has no
  strategy for tidal's S-D-S caustic at all).**  Before the fix that
  row never ran:
  tidal's whole-image BDPT/PT was 338×, outside the blow-up gate's
  [0.5×, 2×], so both the whole-image and the masked ratio-of-ratios
  were SKIPPED.  Post-fix the whole-image ratio is 1.019 (PT 0.05619,
  BDPT 0.05724 at the test's 160×120 / reduced-spp configuration, with
  no `transparent_shadows` on any of the three), the gate no longer
  fires for BDPT, and the masked ratio-of-ratios runs for the first
  time — against a 0.20 band.

  It first looked like a flake: at a fixed K = 12 sub-renders it
  failed roughly four runs in six, with the instability apparently in
  **R_B**, the neutral-signal variant (2.60–2.94 across the six seed
  bases recorded — and a later run reached 3.30, so treat that span as
  a lower bound on what R_B does, not a range).  It is not a flake.
  Reworking the estimator (2026-09-11) settled it:

  - **The spread was a common-mode measurement artifact.**  The
    estimator is `(maskedE/maskedB) · (maskedPT_B/maskedPT_E)`, so the
    PT pair enters as a single MULTIPLICATIVE factor shared by every
    sub-render.  Re-rendering only BDPT/VCM averaged away the
    integrator-side noise and left the PT-side noise fully intact —
    within-run SE 0.017 against a run-to-run sd of ~0.06, 3.5× larger.
    Each sub-render now draws its own PT denominator pair.  That moved
    every OTHER row toward zero (plank BDPT −0.074 → +0.000, plank VCM
    −0.052 → +0.008, pavilion BDPT −0.039 → −0.011, pavilion VCM
    −0.036 → −0.009): the shared denominator had been biasing the
    whole suite negative.
  - **K is now adaptive**, running to a standard-error target of a
    quarter of the band with a 48 cap, and a row that reaches the cap
    without the precision reports `INSUFFICIENT PRECISION` and counts
    as a skip rather than a pass or a fail.  At the default seed base
    every row settles at the K = 12 minimum.
  - **Tidal then reads −0.2443 with SE 0.0179** (−0.2432 / 0.0173 on
    the previous run) — about **2.5 standard errors outside the 0.20
    band** per run (`(0.2443 − 0.20)/0.0179`; the figure "13.6" an
    earlier draft quoted here is |mean|/SE, the distance from ZERO), and
    the real argument is reproducibility: five further fresh runs landed
    in −0.252…−0.273, none passing.

  So the row is a real BDPT-vs-PT disagreement, not a noisy one.  The
  masked set lands on pixels roughly 30× darker than the frame average
  (masked PT mean 0.0017 against a whole-image 0.056, ≈ 13 % of pixels
  — 12.96–13.07 % measured) and BDPT reads **2–3× PT** there on BOTH
  variants (masked R_E 2.11, R_B ≈ 2.8–3.1, 2.60–3.30 observed); the
  ratio-of-ratios does not cancel it because
  the two differ by 24 %.  Whether that 2–3× is a debt-27-class PT
  strategy gap through the water or something else has NOT been
  diagnosed.  *(Diagnosed 2026-09-12 under debt 30: it is a strategy gap,
  and it is BDPT's as much as PT's — neither can reach the S-D-S caustic,
  so on the masked pixels BDPT is numerically PT-without-
  transparent-shadows and only VCM's merges see the transport.  The
  numbers quoted in this paragraph predate the eta^2 fix.)*  **The band was not widened** — round 2 left the row
  failing with a stated precision; round 3 then classified it by the
  symmetric reference-completeness rule (§6.2 of
  SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md) as a counted skip, because
  neither PT nor VCM agrees with BDPT on those pixels even on the neutral
  variant, and opened debt 30 for the disagreement itself.  Tidal's VCM row
  still skips at 2.049× (just over the gate's 2.0), which is the same
  transparent-shadows/merge-recovery effect the table above
  quantifies.  `shelf_bunny` and `pavilion_colonnade`, which were also
  blow-up skips before, are now fully asserted and clean — so the fix
  took the suite from four blow-up skips to one.

  **Guards.**  `tests/CameraImportanceTest.cpp` (new, 960 closed-form
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
  0.987 and 0.979 / 0.989.  A six-bladed f/2.8 row was added to both
  (BDPT topology I, VCM topology F) to run the polygonal aperture
  branch end to end — it is a path-coverage guard rather than a shape
  one, because the aperture area cancels out of `Importance` and the
  shape only decides which pixel a splat lands in (measured: forcing
  the connection side onto the disk branch while the eye rays stay
  hexagonal leaves BDPT/PT at 0.969 and VCM/PT at 0.997, both inside
  the band; the shape/density guarantee is `CameraImportanceTest`'s
  Test 1 instead).  `VCMStrategyBalanceTest` also gained topology G,
  the orthographic mirror of BDPT's topology D, which found that VCM's
  delta-direction-camera support was broken two ways — see the
  orthographic entry below.  Those topologies are deliberately NOT a
  copy of the suites' pinhole ones: BDPT's t==1 MIS weight is set by
  `π·d²·r²/D²` (camera-side over light-side area density at the light
  vertex), which the pinhole topologies' 30°/3.5 m/4 m geometry puts
  near 1.5 × 10⁴ — t==1 weight ~5e-9, at which a 2.5 × 10⁵× inflated
  splat moves the mean by 1e-3 and the bug hides completely (measured
  BDPT/PT = 1.0010).

- **Debt 29 (OPEN, A2 P1-1, debt 28 review round 2): the eye walk
  overruns PSSMLT's 49-lane layout at deep eye/volume depths, and
  debt 28's aperture draw only widened an existing hole.**
  `PSSMLTSampler` multiplexes lanes as `idx = stream + kNumStreams *
  sampleIndex` with `kNumStreams == 49` — every integrator stream from
  0 to 48 gets its own private lane space, but a stream index >= 49
  does not get a fresh lane, it ALIASES an existing one.  The MLT
  rasterizers' `EvaluateSample`/`EvaluateSampleSpectral` deliberately
  live on stream 48 (film + lens + the debt-28 aperture draw,
  contiguous), which is safe as long as nothing else ever reaches
  stream 48 or higher.  But `BDPTIntegrator`'s eye-subpath walk calls
  `sampler.StartStream( 16u + depth )`
  ([BDPTIntegrator.cpp:1732](../src/Library/Shaders/BDPTIntegrator.cpp)),
  which reaches stream 48 ITSELF at eye depth 32 and keeps going past
  it — with `maxVolumeBounce` at its 64 default, ordinary deep-volume
  scenes reach that depth.  Past depth 32 the eye walk's own sampling
  dimensions alias the MLT film/lens/aperture block's lanes (and each
  other, at depths 49 apart).  This is PRE-EXISTING — it predates debt
  28 entirely, since the film/lens block alone already occupied stream
  48 with 4 lanes — debt 28's aperture draw only extends the occupied
  span on stream 48 from 4 lanes to 6 (RGB) or 6+`nSpectralSamples`
  (spectral), which does not change WHETHER the eye walk can reach
  stream 48, only how many of that stream's lanes are spoken for when
  it does.  Not fixed here: the fix needs either raising `kNumStreams`
  well past any reachable eye/volume depth (Sobol-sampler style, see
  `kApertureSamplerStream`'s 8192) or reworking the eye walk's stream
  assignment to stay bounded — both are out of scope for a
  camera-aperture arc.  See `CameraUtilities.h`'s
  `APERTURE_CURRENT_STREAM` doc and
  `tests/PSSMLTStreamAliasingTest.cpp`'s C2/C4 for where this was
  found; neither test currently probes depth >= 32 under PSSMLT, so
  this overrun has no red-proof guard yet.

- **Debt 30 (RESOLVED 2026-09-12) — it was TWO things, and only one of
  them was a bug.  (1) The "BDPT and VCM disagree ~20x on
  `tidal_stones`' caustic-lit pixels" is BDPT's STRUCTURAL S-D-S gap,
  not a weighting error in either integrator.  (2) Underneath it sat a
  real, separate defect that affected every integrator and every
  refractive scene in the tree: RISE applied NO eta^2 basic-radiance
  scaling at any dielectric interface.  Fixed; guard
  `tests/RefractiveRadianceScalingTest.cpp`.  Full write-up:
  [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md).**

  **Finding 1 — the 20x is structural.**  Every caustic-lit stone pixel
  on `tidal_stones` is `E -> water top (S, delta) -> stone (D) -> water
  top (S, delta) -> L (delta)`.  BDPT has no strategy for it: s=1 NEE
  from the stone is blocked by the opaque water surface, t=1
  (stone -> camera) is blocked the same way, and every other split lands
  on a delta vertex.  VCM reaches it by merging at the stone.  Measured
  on the pinhole variant, 160x120, 64 spp, `pixel_filter box`,
  `oidn_denoise FALSE` (whole-image mean, and the ratio of the SUBMERGED
  image quadrants to transparent-shadow PT):

  | integrator | mean, pre-fix | mean, post-fix | submerged quadrants vs PT-ts, pre | post |
  |---|---|---|---|---|
  | PT `transparent_shadows TRUE` | 0.109177 | 0.090168 | 1 | 1 |
  | PT `transparent_shadows FALSE` | 0.063864 | 0.063867 | 0.003–0.10 | 0.005–0.16 |
  | BDPT | 0.064695 | 0.064459 | 0.011–0.10 | 0.012–0.16 |
  | VCM | 0.113430 | 0.092001 | 0.87–1.24 | 0.96–1.15 |

  On tidal, **BDPT is numerically "PT without transparent shadows"** and
  VCM tracks transparent-shadow PT — before AND after the fix.  The
  ledger's `1 : 2.8 : 58` is BDPT/PT ~ 0 on the masked pixels, i.e. a
  transport-coverage difference of exactly the kind
  [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)
  says not to mistake for an MIS bug.  Neither integrator is wrong here;
  the earlier speculation about a VCM merge over-count on S-D-S paths
  from a delta light, and about a BDPT under-count below the merging
  radius, is **withdrawn** — the numbers above are what a missing
  strategy looks like, not what a mis-weighted one looks like.  This
  half needed documentation, not code, and
  `SignalIntegratorConsistencyTest`'s tidal masked row remains a
  counted `NO COMPLETE REFERENCE` skip after the fix (post-fix
  neutral-variant masked means PT : BDPT : VCM = 1 : 2.01 : 33.6; the
  1 : 2.8 : 58 recorded in earlier rounds was measured before the fix,
  and VCM's multiple fell by n^2 while PT's masked mean — which carries
  no under-water transport either way — did not).

  **Finding 2 — the real defect.**  Radiance is not invariant across a
  smooth refractive interface; `L / n^2` is.  A RADIANCE-mode walk owes
  `(eta_before / eta_after)^2` at every medium change.  RISE applied it
  nowhere.  Reference-free measurement — a Lambertian luminaire
  (`exitance 1`, so `L = 1/pi = 0.318310`) 25 cm inside a water box,
  viewed from air at near-normal incidence:

  | ior | | PT | BDPT | VCM | pixelpel | physics `T*L/n^2` |
  |---|---|---|---|---|---|---|
  | — | dry | 0.318359 | 0.318359 | 0.318359 | 0.318359 | 0.318310 |
  | 1.33 | wet, pre-fix | 0.311816 | 0.311816 | 0.311816 | 0.312012 | **0.176338** |
  | 1.33 | wet, post-fix | 0.176310 | 0.176310 | 0.176310 | 0.176392 | 0.176338 |

  Pre-fix every integrator read exactly `T*L` — the Fresnel
  transmittance applied, the 1/n^2 missing (0.311816/0.176338 = 1.7683,
  close to but not the same number as n^2 = 1.33^2 = 1.7689 -- the
  ~0.03% gap is measurement quantization in the pre-fix render, not a
  different physical constant).

  **Why it hid.**  It CANCELS for PT and BDPT in the common case and
  does not cancel for anything whose light side carries flux.  An eye
  path that enters the medium (x 1/n^2) and exits it again toward the
  emitter (x n^2) nets x1; a merge, a photon gather, a light-tracing
  splat or a transparent-shadow NEE crosses the interface only once on
  the radiance side and came out n^2 bright.  That is what the ledger
  had recorded as "VCM 1.53x over PT on a submerged floor": on the slab
  probe (Lambertian floor inside a water box, tiny sphere emitter and
  camera both in air) VCM/PT went **1.554 -> 1.046** (0.004908/0.004690
  = 1.0465, rounds to 1.046) while PT and BDPT
  moved by less than their own run-to-run spread (0.004666 -> 0.004690
  and 0.004786 -> 0.004857).  With a delta omni instead, PT and BDPT
  are exactly 0 both before and after (structural), and the two
  estimators that CAN see it both dropped by n^2 and still agree:
  transparent-shadow PT 0.095650 -> 0.054075 and VCM 0.102916 ->
  0.056977.

  **The rule.**  Radiance-mode walks (camera-rooted: PT, the BDPT /
  VCM / MLT eye subpath, the legacy shader-op chain, the final gather)
  multiply by `(eta_before / eta_after)^2` on a medium change.
  Importance-mode walks (light subpaths, photon tracers, SMS photon
  seeds, detector-sphere rigs) and shadow rays get NO factor.  No
  `TransportMode` was threaded through `ISPF::Scatter` (~60
  implementations); the factor is applied at the CONSUMER from the two
  IOR stacks it already holds, via one inline
  `RISE::RadianceEtaScale` in `Utilities/IORStack.h`, and returns
  exactly 1 when the medium did not change.  `ManifoldSolver`'s SMS
  chain already applied the same factor in the same convention and was
  left alone.  §6.1 of the write-up is the full site table.

  **What changes in shipped scenes:** submerged / encased surfaces and
  emitters seen from outside get 1/n^2 (0.565x in water, 0.444x in
  glass); a camera inside a refractor sees air-side content at n^2.
  Nothing else moves.  `TidalStonesShowcaseTest` measures wet/dry
  RATIOS and stayed at 122/122; `EnvLightBalanceTest` topology J uses
  `ior 1.0` and stays bit-exact (116/116);
  `CausticPhotonMapNormalizationTest` puts its camera UNDER the water,
  so no interface sits on its eye side, and is unchanged (4/4) — which
  is also why
  [CAUSTIC_PHOTONMAP_NORMALIZATION.md](CAUSTIC_PHOTONMAP_NORMALIZATION.md)
  §11's measurements were blind to this factor and remain valid, and why
  §11.6's prohibition (on a MERGE-side rescale) is untouched.  See §13
  of that document.

  **Cost:** one multiply per refraction event.  `tidal_stones` PT-ts
  456/438/439 ms -> 461/445/447 ms; `plank_closeup`
  19547/20153/20135 ms -> 19837/20010/19962 ms.  Both inside the
  run-to-run spread.

  **Residual: SSS family eta^2, see REFRACTIVE_RADIANCE_SCALING.md §10.**
  The subsurface-scattering family (`SubSurfaceScatteringSPF`,
  `RandomWalkSSS`, `BSSRDFSampling::Sw`) never touches the IOR stack, so
  it is untouched by this fix and reads the factor as exactly 1
  unconditionally — an open question on whether that is correct (a
  telescoping closed form, matching this fix's own convention) or a gap
  (PBRT-v3's radiance-mode BSSRDF applies an explicit eta^2 divide).  Not
  diagnosed further this round; see that section for the two readings
  and the un-run observable that would settle it.

  **Incidental fix found in passing while auditing this area:**
  `TranslucentSPF::Scatter`'s entry-side per-channel Phong-N loop
  (`src/Library/Materials/TranslucentSPF.cpp` ~line 154, inside
  `TranslucentSPF::Scatter`) wrote `trans.kray[0] = p[0]` on every
  iteration instead of `trans.kray[i] = p[i]`, zeroing channels 1 and 2
  of that lobe for any `translucent_material` with a non-uniform
  per-channel Phong N; fixed to match the sibling exit-side loop, guard
  extended in `tests/TranslucentIORStackTest.cpp`.  Review round 2
  (2026-09-12) found and fixed the EXIT side's sibling of the same
  pattern (~line 230-231): `front.kray = 0; front.kray[i] = ...` reset
  every iteration, but `front` (the ray that actually leaves the object)
  is added to `scattered` only once, after the loop, so only the last
  channel (B) survived — see the C1 fix commit and the same test's
  updated coverage.

- **~~Debt 31: TranslucentSPF residuals from the debt-30 audit.~~ CLOSED
  2026-09-12 — items 1–4 closed individually below.** Further independent
  amplitude/mixture/state-generation debts remain DL-38/DL-41/DL-47; this does not assert complete
  translucent transport correctness.
  1. **~~Guided-direction IOR-stack leak.~~ CLOSED 2026-09-12 — `8a9bdb18`,
     `TranslucentIORStackTest: ALL TESTS PASSED`.** PT and BDPT now preserve
     an available selected exit post-stack for guided continuations crossing the geometric boundary and retain
     the input stack for opposite-side replacements. Training and eta
     consumers share that resolved state. Four failed assertions on the
     unfixed library establish the PT RGB/NM red proof; subsequent BDPT
     eye/light coverage was first run on the fixed library. Ordinary PT
     specular arrivals suppress guiding, so the fixture explicitly seeds
     a diffuse arrival and demands positive actual outward substitutions.
     BDPT eye RIS's retained-SPF-only case was disclosed under DL-43
     (CLOSED `a69c9ce6`, 2026-09-13 -- eye RIS now achieves actual
     outward guide-direction substitutions; see docs/DEBT_LEDGER.md).
     See [DL-03 closure and audit](DL03_GUIDED_IOR_CONTINUATION.md).
  2. **`ScatteredRayContainer` overflow leak in the per-channel loop —
     CLOSED, review round 3 (in the same commit range that fixed debt
     30's review round 3 items C1/C2).**  Auditing the shape described
     here for C1 found it understated the bug: the entry per-channel
     loop's leak is UNCONDITIONAL on the SUCCESS path, not
     overflow-only — every anisotropic-N entry `Scatter` call leaked
     two `IORStack`s regardless of container occupancy, because the
     loop never re-armed `delete_stack` before each new allocation
     (fixed by doing so). Auditing THAT fix in turn surfaced the
     narrower overflow-carryover variant this item originally
     described — an iteration whose `AddScatteredRay` call fails still
     owns its own stack post-fix, but a later iteration's `new
     IORStack` would silently overwrite that pointer without freeing it
     — which was closed in the same round by freeing any such
     carried-over stack before each reassignment. The exit-side loop
     never had either shape (it never assigns `ior_stack` inside a
     per-channel loop at all). See
     [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md)
     §10.3 for the closed writeup, and `TranslucentSPF.cpp`'s commit
     history for this round for the two fix commits.  (The second fix
     commit's message refers to a "four-way case table above"; no table
     was written into that message — the ownership trace it means is
     the comment block above the entry per-channel loop in
     `TranslucentSPF.cpp` (~lines 169-203), and review round 4
     re-traced every add-success/add-failure combination independently.)
  3. **~~RGB vs spectral exit-lobe weight divergence.~~ CLOSED 2026-09-12
     — `1239edf2`, `TranslucentSpectralParityTest: 676 checks, 0 failures`.**
     Both entry paths pay the primary-layer `tau`; each interior segment
     now pays only Beer extinction before its exit/backscatter split.
     The spectral parent weight incorrectly multiplied `tau` again,
     affecting BOTH children. The red-proof on `6486656e` (test commit
     `fc371041`) had 174 failures. This closes DL-01's lobe-weight
     divergence, not item 4's directional/Pdf contract or DL-38's
     separate BSDF/HWSS repricing gap. The actual scene parameter is
     `tau`, not `transmittance`. See
     [DL-01 closure](DL01_TRANSLUCENT_EXIT_WEIGHT.md).
  4. **~~RGB/NM exit density support/shape mismatch (DL-02).~~ CLOSED
     2026-09-12 — `a041e51d`, `TranslucentSpectralParityTest: 1918 checks,
     0 failures` (red: 324 failures).** Both pipes now sample and evaluate
     cosine density around +onb.w() for the diffuse exit. The inside-state
     evaluator had used the opposite hemisphere, and NM additionally
     sampled a Phong exit. Entry/backscatter Phong lobes are unchanged.
     Complete mixture/reverse densities remain DL-41; this closure does
     not assert integrator-wide MIS correctness. See
     [DL-02 closure](DL02_TRANSLUCENT_EXIT_DENSITY.md).
  Item 1 is recorded in
  [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md) §10.3;
  items 3 and 4 are recorded there as well.


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
