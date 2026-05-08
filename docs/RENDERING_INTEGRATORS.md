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
| `pathtracing_spectral_rasterizer` | pure integrator | Spectral. NM and HWSS modes both available. |

### BDPT (bidirectional path tracing)

| Chunk | Pipeline | Notes |
|---|---|---|
| `bdpt_pel_rasterizer` | pure integrator | RGB. Generates eye + light subpaths, connects all (s,t) strategy pairs, MIS-weights via the **power heuristic (β=2)** — see [MIS_HEURISTICS.md](MIS_HEURISTICS.md). Strong on glossy interreflection and indirect specular. |
| `bdpt_spectral_rasterizer` | pure integrator | Spectral analogue.  Wires the full path-guiding parameter set (parser switched from a hand-rolled subset to `AddPathGuidingParams` on 2026-05-07; see [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.7). |

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
[`CreateAllChunkParsers()`](../src/Library/Parsers/AsciiSceneParser.cpp);
the table in [Parsers/README.md](../src/Library/Parsers/README.md)
"Helper Templates" is the source of truth. ✓ = supported, ✗ = not
wired, partial = subset.

| Chunk | Path guiding | Adaptive sampling | SMS | Optimal MIS | OIDN denoise |
|---|---|---|---|---|---|
| `pixelpel_rasterizer` | ✓ | ✓ | (via shader-op) | ✓ | ✓ |
| `pixelintegratingspectral_rasterizer` ¹ | ✗ | ✗ | (via shader-op) | ✗ | (limited) |
| `pathtracing_pel_rasterizer` | ✓ | ✓ | ✓ | ✓ | ✓ (full filtered-film bypass) |
| `pathtracing_spectral_rasterizer` | ✗ | ✓ | ✓ | ✗ | (limited) |
| `bdpt_pel_rasterizer` | ✓ | ✓ | ✗ | ✗ | ✓ |
| `bdpt_spectral_rasterizer` | ✓ | ✗ | ✗ | ✗ | (limited) |
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

## 7. Cross-references

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
