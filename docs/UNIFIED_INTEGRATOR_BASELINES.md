# Unified Integrator Baselines — Phase 1 Empirical Scene→Integrator Matrix

**Status:** Phase-1 measurement output (per [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md) §4).
**Produced:** 2026-06-03.
**Foundation:** current production (disc-area) baseline — the documented ~11-28 %
VCM env-only strict residual (Sessions 9-13,
[VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md))
is **annotated, not blocked on** — PT is the env-IBL reference.
**Read first:** [skills/variance-measurement.md](skills/variance-measurement.md)
(the measurement protocol this executes), [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md)
(the §2 decision tree this matrix tests with data).

This document answers the Phase-1 question: **which integrator delivers the
lowest variance per unit wall-clock, on which scene class** — so the Phase-3
decision gate ([UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md)
§6.3) rests on data, not intuition. It does **not** pre-empt that decision; it
produces the measurement and states what it implies.

---

## 1. Methodology

### 1.1 What is measured

Per (scene, integrator), K=16 independent EXR trials (K=4 only where noted),
rendered **strictly sequentially** (RISE takes all cores; concurrent renders
corrupt wall-clock). Each trial is the **core** integrator — no path guiding
(so PT/BDPT/VCM are compared on equal footing; VCM cannot do PGL at all), no
adaptive sampling, no OIDN denoise, `pixel_filter box` forced on every variant
(removes the Session-13 filter confounder; identical filter across integrators).

Metrics per cell (from `bin/tools/HDRVarianceTest`, linear-space EXR):

- **mean / median / P99 / max per-pixel σ²** (inter-run variance, K trials).
- **relative noise σ/μ** (brightness-normalized).
- **RMSE vs reference** (each integrator's trials vs the scene's single high-SPP
  reference — captures bias + variance, i.e. distance to truth).
- **mean rasterization time T** (the `Total Rasterization Time` line, integrator
  time only; OIDN off so it is pure render).
- **mean luminance** (spatial mean — the bias signal for cross-integrator
  energy agreement).

### 1.2 The wall-clock-normalized figure of merit: σ²·T

The user-chosen metric is **lowest variance at fixed wall-clock**. Under Monte-Carlo
variance σ² ∝ 1/N and render time T ∝ N, the product **σ²·T is SPP-invariant** —
it is the variance you would obtain at unit wall-time, i.e. Veach efficiency
(lower = better). This was **empirically confirmed** in the pilot (§2): PT
jewel_vault at 32 spp gave σ²·T ≈ 19.6, at 128 spp ≈ 17.8 (equal within timing
noise). So σ²·T lets us compare integrators **even when they run at different SPP
or wall-clock** — exactly what cross-integrator comparison needs (BDPT/VCM cost
more per sample than PT).

The cross-integrator **ROI** in each scene is σ²·T(integrator) / σ²·T(best) — a
ratio ≥1 stating "this integrator is Nx worse than the per-scene best at fixed
wall time." The headline ROI uses **mean σ²·T**; the matrix also reports **median**
and **P99 σ²** per cell, because the tail (firefly) behaviour routinely diverges
from the mean (path-cost) — e.g. VCM often has the worst mean σ² yet the best P99
(its merging suppresses the firefly tail that PT cannot).

Because σ²·T is SPP-invariant, trial SPP was tuned **per scene** purely for
render-time feasibility (8–64 spp); this does not bias the comparison. The same
SPP is used across integrators within a scene.

### 1.3 Reference policy (for RMSE-vs-truth and bias)

- **PT is the unbiased reference wherever PT reaches the transport** —
  diffuse, glossy, env-IBL, volume, many-light. PT trials vs PT-ref → pure
  variance; BDPT/VCM trials vs PT-ref → variance + any bias (e.g. the env-IBL
  residual, VCM finite-N bias).
- **For caustic / refractive-chain scenes PT and BDPT cannot reach the
  transport**, so the reference is a **high-SPP VCM** render. PT/BDPT RMSE-vs-
  VCM-ref then quantifies their **transport deficit** (how much caustic energy
  they miss) — this is a feature of the measurement, annotated per cell, not a
  harness defect. VCM-vs-VCM-ref measures VCM's own variance (with the caveat
  that VCM is measured against itself).

### 1.4 Harness (all under the gitignored `var_test/`)

- `make_variant.py` — full rasterizer-chunk replacement to a canonical
  measurement chunk for the target integrator, **preserving** env-IBL physics
  params (`radiance_map/scale/background/orient`) and spectral-core params
  (`spectral_samples/nmbegin/nmend/num_wavelengths/hwss`); collapses outputs to
  a single 32-bit EXR (Rec709 linear); strips CRLF; overrides resolution.
- `measure.py` — corpus-driven orchestrator: sequential K-trial renders, timing
  capture, HDRVarianceTest in both modes, incremental resumable `results.json`.
- `corpus.py` — the scene manifest (§3).
- `vcm_bias.py` — the §4.3 VCM finite-N bias sweep (SPP × merge-radius).
- `analyze.py` — emits the markdown matrix below.
- Benchmark thread mode: `render_thread_reserve_count 0` via `RISE_OPTIONS_FILE`
  (every core gets a worker — Apple-Silicon E-core reservation off, per CLAUDE.md
  "Benchmarks").
- Measurement-tool build: added a dedicated `HDRVarianceTest` macOS Makefile
  target ([build/make/rise/Makefile](../build/make/rise/Makefile)), warning-free
  (removed a dead `sumMeanSq` variable in the tool source).

Resolution was downscaled to ~320 px wide (aspect preserved) for K-trial
feasibility; per-pixel variance is resolution-independent in expectation, so this
does not bias the metric. Machine: Apple Silicon (Darwin arm64), this session.

---

## 2. Pilot validation (the de-risking gate)

Before the full corpus, the complete pipeline was validated end-to-end on one
scene (`jewel_vault`, the variance-measurement skill's canonical benchmark) at
K=4, plus a dedicated SPP-scaling check at K=16. **Gate result: PASS** — with one
important methodological finding.

| Gate check | Result |
|---|---|
| EXR reads clean (no HDR-RLE 1e-24 garbage) | ✓ pixel sums physical, center pixels sane |
| Integrators agree on mean luminance | ✓ PT 10.6754 / BDPT 10.6773 / VCM 10.6423 (≤0.3 %) |
| mean σ² decreases ∝ 1/N with SPP | ✓ 32→128 spp: 11.14→2.65 = **4.20×** (textbook) |
| P99 σ² decreases with SPP | ✓ 178.7→22.3 = 8.0× (tail suppression, faster than 1/N — fine) |
| σ²·T SPP-invariant (efficiency metric sound) | ✓ 19.6 (32 spp) vs 17.8 (128 spp) |
| RMSE-vs-ref finite + consistent across trials | ✓ PT 3.71–4.03, BDPT 4.42–4.78, VCM 8.47–9.10 |
| Wall-times stable | ✓ PT 1.73–1.79 s, BDPT 7.54–7.71 s, VCM 11.1–11.6 s |

**Methodological finding — median σ² is fragile on near-black-heavy scenes.**
On `jewel_vault`, median σ² did **not** drop with SPP (8.2e-4→1.2e-3) and was
2× K-sensitive. Root cause (not a pipeline flaw): the scene has a huge population
of near-black pixels (deep alcove, sparse 3+ bounce indirect — the scene's whole
point); the median sits exactly at the "barely-lit vs never-lit" boundary, and as
SPP rises more pixels cross from σ²≈0 to σ²>0, so the median *rises*. The **mean
and P99 scale correctly** and are the SPP-invariant basis for σ²·T; **median is
reported but not used for efficiency conclusions on near-black-heavy scenes.**

---

## 3. Corpus

Production-weighted (FeatureBased showcase) plus the cleanest matched-triplet
Test scenes for the bias measurements. 18 scenes spanning the transport-difficulty
space. (`sponza_new` was dropped — its source imports a glTF from a hardcoded
absent Windows path; the architectural-diffuse class is still covered by
`cloister`, `jewel_vault`, and the mixed scenes.)

| scene | class | integrators | spp | ref | res |
|-------|-------|-------------|-----|-----|-----|
| `jewel_vault` | diffuse-indirect | PT/BDPT/VCM | 32 | PT@512 | 256x192 |
| `cloister` | diffuse-glossy outdoor | PT/BDPT/VCM | 8 | PT@256 | 320x240 |
| `ggx_showcase` | glossy metal | PT/BDPT/VCM | 16 | PT@256 | 320x320 |
| `gi_spheres` | glossy GI | PT/BDPT/VCM | 8 | PT@256 | 320x320 |
| `pool_caustics` | caustic (water) | PT/BDPT/VCM | 16 | VCM@256 | 320x240 |
| `glass_pavilion` | caustic (dielectric) | PT/BDPT/VCM | 16 | VCM@256 | 320x240 |
| `diamond_teapot` | caustic (dispersive-ish, VCM-native) | PT/BDPT/VCM | 8 | VCM@128 | 224x320 |
| `torus_chain` | refractive chain | PT/BDPT/VCM | 8 | VCM@128 | 320x320 |
| `alchemists` | mixed showcase | PT/BDPT/VCM | 16 | PT@256 | 320x214 |
| `sculptors_studio` | mixed showcase | PT/BDPT/VCM | 16 | PT@256 | 320x240 |
| `showroom` | mixed showcase (product viz) | PT/BDPT/VCM | 16 | PT@256 | 320x240 |
| `homogeneous_fog` | volumetric (homogeneous) | PT/BDPT/VCM | 8 | PT@256 | 256x256 |
| `env_fog` | volumetric env-through-fog (VCM transmittance gap) | PT/BDPT/VCM | 32 | PT@512 | 128x128 |
| `prism_dispersion` | spectral dispersion (prism) | PT/BDPT | 16 | PT@256 | 256x256 |
| `spectral_caustic` | spectral dispersive caustic | PT/BDPT/VCM | 16 | VCM@256 | 256x256 |
| `env_only` | env-IBL only (env is the illuminant) | PT/BDPT/VCM | 32 | PT@512 | 320x160 |
| `env_mesh` | env-IBL + mesh emitter | PT/BDPT/VCM | 64 | PT@1024 | 128x128 |
| `corridor_100lights` | many-light | PT/BDPT/VCM | 16 | PT@256 | 256x256 |

---

## 4. Results matrix

Each cell: mean rasterization time `t`, the four σ² statistics, **σ²·T** (wall-clock-normalized variance — the headline efficiency metric, lower = better) with its cross-integrator **ROI** vs the per-scene best, **RMSE→ref** (distance to truth), mean luminance `lum`, relative noise `σ/μ`, and a `conv` flag (✓ = luminance within 20% of the reference integrator, i.e. the cell actually solves the scene; ✗ = biased/incomplete; `Inf⚠` = mean σ² overflowed on degenerate-pdf fireflies). **σ²·T ROI is only computed among `conv ✓` cells** — an integrator that renders a dark image has low variance for the wrong reason.

#### `jewel_vault` — diffuse-indirect
_ref: high-SPP PT. Slot-window Cornell vault, dominant 3+ bounce indirect. PT home/hard-indirect._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 2.00 | 1.101e+01 | 8.259e-04 | 1.839e+02 | 2.203e+01 | **best** | 3.886 | 10.6765 | 31.1% | ✓ |
| BDPT | 8.63 | 1.252e+01 | 2.066e-04 | 1.705e+01 | 1.081e+02 | 4.9x | 4.530 | 10.6764 | 33.1% | ✓ |
| VCM | 11.95 | 7.188e+01 | 2.952e-04 | 5.500e-01 | 8.589e+02 | 39.0x | 8.657 | 10.6429 | 79.7% | ✓ |

**Luminance bias vs PT:** BDPT -0.0%, VCM -0.3%

#### `cloister` — diffuse-glossy outdoor
_ref: high-SPP PT. Outdoor cloister, more uniform lighting; lower-variance diffuse baseline._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.18 | 3.468e-02 | 2.587e-03 | 1.188e+00 | 4.096e-02 | **best** | 0.297 | 0.6631 | 28.1% | ✓ |
| BDPT | 5.51 | 1.815e-02 | 2.969e-04 | 2.804e-01 | 9.995e-02 | 2.4x | 0.233 | 0.6702 | 20.1% | ✓ |
| VCM | 7.95 | 4.361e-02 | 8.806e-04 | 1.026e-02 | 3.465e-01 | 8.5x | 0.230 | 0.6575 | 31.8% | ✓ |

**Luminance bias vs PT:** BDPT +1.1%, VCM -0.8%

#### `ggx_showcase` — glossy metal
_ref: high-SPP PT. GGX metal roughness sweep; glossy interreflection._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.13 | 1.962e-02 | 3.674e-03 | 3.878e-01 | 2.222e-02 | **best** | 0.226 | 1.1129 | 12.6% | ✓ |
| BDPT | 5.99 | 9.037e-03 | 6.178e-04 | 2.253e-01 | 5.410e-02 | 2.4x | 0.185 | 1.1167 | 8.5% | ✓ |
| VCM | 6.60 | 8.640e-03 | 1.831e-04 | 6.046e-02 | 5.701e-02 | 2.6x | 0.159 | 1.1151 | 8.3% | ✓ |

**Luminance bias vs PT:** BDPT +0.3%, VCM +0.2%

#### `gi_spheres` — glossy GI
_ref: high-SPP PT. Glossy sphere GI (native pixelpel). Glossy interreflection._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 2.14 | 1.290e-02 | 2.532e-04 | 6.354e-01 | 2.760e-02 | 56.2x | 0.368 | 0.3907 | 29.1% | ✓ |
| BDPT | 3.41 | 1.440e-04 | 2.737e-05 | 3.498e-03 | 4.913e-04 | **best** | 0.121 | 0.3914 | 3.1% | ✓ |
| VCM | 7.93 | 7.901e-05 | 2.168e-05 | 1.770e-03 | 6.263e-04 | 1.3x | 0.114 | 0.3908 | 2.3% | ✓ |

**Luminance bias vs PT:** BDPT +0.2%, VCM +0.0%

#### `pool_caustics` — caustic (water)
_ref: high-SPP VCM. Water-surface caustics. PT/BDPT cannot reach -> VCM ref; RMSE = deficit._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 3.85 | 3.617e-06 | 9.779e-10 | 8.509e-05 | 1.393e-05 |  | 0.218 | 0.0345 | 5.5% | ✗ |
| BDPT | 1.66 | 2.405e-02 | 5.402e-09 | 5.983e-02 | 3.996e-02 |  | 0.355 | 0.0711 | 218.4% | ✗ |
| VCM | 5.62 | 5.332e-03 | 2.156e-05 | 8.488e-02 | 2.999e-02 | **best** | 0.086 | 0.1533 | 47.6% | ✓ |

**Transport reach vs VCM-ref (energy):** PT -77.5%, BDPT -53.7%

#### `glass_pavilion` — caustic (dielectric)
_ref: high-SPP VCM. Dielectric pavilion caustics. VCM ref._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.68 | 4.168e+02 | 4.100e-04 | 9.763e-01 | 6.984e+02 |  | 103.172 | 0.3347 | 6099.3% | ✗ |
| BDPT | 4.68 | Inf⚠ | 1.003e-04 | 6.244e+00 | Inf⚠ |  | n/a | n/a | n/a | Inf⚠ |
| VCM | 6.29 | Inf⚠ | 2.335e-03 | 1.108e+03 | Inf⚠ |  | n/a | n/a | n/a | Inf⚠ |



#### `diamond_teapot` — caustic (dispersive-ish, VCM-native)
_ref: high-SPP VCM. Diamond teapot pour, VCM-native showcase. VCM ref._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 3.94 | 6.058e-01 | 1.110e-05 | 1.571e+00 | 2.389e+00 |  | 1.821 | 0.4348 | 179.0% | ✗ |
| BDPT | 6.21 | 1.169e-01 | 9.419e-07 | 8.186e-01 | 7.256e-01 |  | 1.621 | 0.4806 | 71.1% | ✗ |
| VCM | 9.69 | 3.415e-01 | 4.372e-04 | 3.258e+00 | 3.308e+00 | **best** | 0.744 | 0.8517 | 68.6% | ✓ |

**Transport reach vs VCM-ref (energy):** PT -48.9%, BDPT -43.6%

#### `torus_chain` — refractive chain
_ref: high-SPP VCM. Glass torus chain atrium; refractive transport. VCM ref._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 2.04 | 6.870e-01 | 4.666e-03 | 1.564e+01 | 1.404e+00 | **best** | 1.497 | 0.8815 | 94.0% | ✓ |
| BDPT | 6.14 | 5.892e-01 | 9.517e-04 | 1.523e+01 | 3.615e+00 | 2.6x | 1.463 | 0.8651 | 88.7% | ✓ |
| VCM | 9.59 | 5.340e-01 | 7.971e-04 | 1.412e+01 | 5.121e+00 | 3.6x | 0.757 | 0.8179 | 89.3% | ✓ |

**Transport reach vs VCM-ref (energy):** PT +7.8%, BDPT +5.8%

#### `alchemists` — mixed showcase
_ref: high-SPP PT. Alchemist's sanctum, mixed transport (pt/bdpt pair exists)._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.84 | 3.090e+01 | 4.542e-02 | 1.892e+02 | 5.673e+01 | 2.5x | 5.750 | 4.2280 | 131.5% | ✓ |
| BDPT | 6.25 | 3.648e+00 | 4.524e-06 | 1.596e-01 | 2.281e+01 | **best** | 3.206 | 3.7775 | 50.6% | ✓ |
| VCM | 8.17 | 1.535e+01 | 1.939e-05 | 6.718e-02 | 1.254e+02 | 5.5x | 4.931 | 3.5233 | 111.2% | ✓ |

**Luminance bias vs PT:** BDPT -10.7%, VCM -16.7%

#### `sculptors_studio` — mixed showcase
_ref: high-SPP PT. Sculptor's studio, mixed diffuse+glossy showcase._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.71 | 1.057e+02 | 1.987e-08 | 2.683e-01 | 7.544e+01 | 302.3x | 15.098 | 0.1914 | 5372.7% | ✓ |
| BDPT | 1.71 | 2.357e-07 | 9.550e-13 | 1.995e-07 | 4.020e-07 |  | 10.968 | 0.0023 | 21.4% | ✗ |
| VCM | 1.85 | 1.350e-01 | 2.371e-08 | 7.097e-02 | 2.496e-01 | **best** | 10.971 | 0.1742 | 211.0% | ✓ |

**Luminance bias vs PT:** BDPT -98.8%, VCM -9.0%

#### `showroom` — mixed showcase (product viz)
_ref: high-SPP PT. Product-viz showroom, mixed transport._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.59 | 4.746e-01 | 6.258e-08 | 2.682e+00 | 2.811e-01 | **best** | 1.131 | 0.6040 | 114.1% | ✓ |
| BDPT | 0.84 | 9.681e-01 | 9.437e-08 | 1.309e+00 | 8.177e-01 | 2.9x | 1.931 | 0.5492 | 179.2% | ✓ |
| VCM | 1.81 | 2.021e+00 | 3.111e-06 | 2.861e+00 | 3.652e+00 | 13.0x | 2.464 | 0.5664 | 251.0% | ✓ |

**Luminance bias vs PT:** BDPT -9.1%, VCM -6.2%

#### `homogeneous_fog` — volumetric (homogeneous)
_ref: high-SPP PT. Homogeneous fog medium. PT reference._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.25 | 2.360e-01 | 1.835e-03 | 2.178e+01 | 5.912e-02 | **best** | 0.493 | 0.2221 | 218.7% | ✓ |
| BDPT | 9.37 | 7.111e-03 | 8.616e-08 | 5.098e-04 | 6.663e-02 | 1.1x | 0.202 | 0.2263 | 37.3% | ✓ |
| VCM | 2.94 | 7.690e-03 | 6.457e-09 | 9.390e-05 | 2.261e-02 |  | 0.206 | 0.1543 | 56.8% | ✗ |

**Luminance bias vs PT:** BDPT +1.9%, VCM -30.5%

#### `env_fog` — volumetric env-through-fog (VCM transmittance gap)
_ref: high-SPP PT. Env-IBL through bounded fog. Isolates VCM VCMIsVisible binary-occlusion gap. Matched triplet exists._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.05 | 3.268e-03 | 3.133e-03 | 7.178e-03 | 1.649e-04 | **best** | 0.059 | 0.1918 | 29.8% | ✓ |
| BDPT | 0.21 | 2.073e-03 | 1.935e-03 | 4.873e-03 | 4.435e-04 | 2.7x | 0.064 | 0.2006 | 22.7% | ✓ |
| VCM | 0.24 | 1.929e-03 | 1.802e-03 | 4.467e-03 | 4.634e-04 | 2.8x | 0.074 | 0.2088 | 21.0% | ✓ |

**Luminance bias vs PT:** BDPT +4.6%, VCM +8.9%

#### `prism_dispersion` — spectral dispersion (prism)
_ref: high-SPP PT. HWSS prism dispersion. Spectral PT/BDPT. PT-spectral reference._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.37 | 1.973e-01 | 5.348e-03 | 1.654e+01 | 2.701e-01 | **best** | 0.641 | 1.2429 | 35.7% | ✓ |
| BDPT | 6.07 | 4.129e-01 | 1.028e-02 | 2.102e+01 | 2.507e+00 |  | 2.126 | 0.7983 | 80.5% | ✗ |

**Luminance bias vs PT:** BDPT -35.8%

#### `spectral_caustic` — spectral dispersive caustic
_ref: high-SPP VCM. Spectral dispersive caustic. VCM-spectral ref (note: VCM-spectral merge luminance-proxy gap)._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.66 | 4.297e+01 | 2.692e-02 | 5.546e+02 | 2.835e+01 |  | 6.846 | 1.5990 | 410.0% | ✗ |
| BDPT | 0.95 | 4.216e+01 | 1.889e-02 | 5.301e+02 | 4.011e+01 |  | 6.773 | 1.6025 | 405.2% | ✗ |
| VCM | 1.98 | 4.011e+01 | 1.148e-02 | 8.195e+00 | 7.925e+01 | **best** | 6.546 | 1.3288 | 476.6% | ✓ |

**Transport reach vs VCM-ref (energy):** PT +20.3%, BDPT +20.6%

#### `env_only` — env-IBL only (env is the illuminant)
_ref: high-SPP PT. Env-only NEE test; env is the sole illuminant. Canonical env-IBL deficit (PT ref handles env correctly)._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 0.34 | 2.148e-01 | 4.657e-08 | 2.310e+00 | 7.291e-02 | **best** | 1.484 | 0.4906 | 94.5% | ✓ |
| BDPT | 1.21 | 1.043e-01 | 3.080e-08 | 1.035e+00 | 1.260e-01 | 1.7x | 1.368 | 0.5220 | 61.9% | ✓ |
| VCM | 1.44 | 3.720e-01 | 1.192e-07 | 3.226e+00 | 5.374e-01 |  | 1.904 | 0.7995 | 76.3% | ✗ |

**Luminance bias vs PT:** BDPT +6.4%, VCM +63.0%

#### `env_mesh` — env-IBL + mesh emitter
_ref: high-SPP PT. Env + mesh emitter mixed. Documented VCM/BDPT env strict residual. PT reference (handles env correctly)._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.04 | 3.613e-04 | 8.105e-07 | 4.269e-05 | 3.742e-04 | 4.0x | 0.027 | 0.1183 | 16.1% | ✓ |
| BDPT | 1.59 | 5.812e-05 | 4.094e-05 | 4.445e-04 | 9.266e-05 | **best** | 0.037 | 0.1173 | 6.5% | ✓ |
| VCM | 1.82 | 4.947e-05 | 3.563e-09 | 1.928e-07 | 9.005e-05 |  | 0.081 | 0.0438 | 16.1% | ✗ |

**Luminance bias vs PT:** BDPT -0.8%, VCM -63.0%

#### `corridor_100lights` — many-light
_ref: high-SPP PT. 100-light corridor, light-BVH stress. PT reference._

| ig | t(s) | mean σ² | median σ² | P99 σ² | **σ²·T** | ROI | RMSE→ref | lum | σ/μ | conv |
|----|------|---------|-----------|--------|----------|-----|----------|-----|-----|------|
| PT | 1.07 | 1.873e-01 | 3.323e-02 | 4.127e+00 | 2.005e-01 | **best** | 1.606 | 6.5462 | 6.6% | ✓ |
| BDPT | 7.98 | 8.514e+00 | 5.345e-04 | 7.386e+00 | 6.791e+01 | 338.6x | 19.278 | 6.6755 | 43.7% | ✓ |
| VCM | 7.77 | 2.666e+01 | 1.317e-03 | 1.904e+02 | 2.073e+02 |  | 27.126 | 11.5466 | 44.7% | ✗ |

**Luminance bias vs PT:** BDPT +2.0%, VCM +76.4%

---

## 5. Per-scene-class winner (refining RENDERING_INTEGRATORS.md §2 with data)

| scene | class | σ²·T winner (converged) | RMSE→truth winner | decisive |
|-------|-------|-------------------------|-------------------|----------|
| `jewel_vault` | diffuse-indirect | **PT** | PT | σ²·T |
| `cloister` | diffuse-glossy outdoor | **PT** | VCM | σ²·T |
| `ggx_showcase` | glossy metal | **PT** | VCM | σ²·T |
| `gi_spheres` | glossy GI | **BDPT** | VCM | σ²·T |
| `pool_caustics` | caustic (water) | VCM | **VCM** | RMSE |
| `glass_pavilion` | caustic (dielectric) | — | **PT** | RMSE |
| `diamond_teapot` | caustic (dispersive-ish, VCM-native) | VCM | **VCM** | RMSE |
| `torus_chain` | refractive chain | PT | **VCM** | RMSE |
| `alchemists` | mixed showcase | **BDPT** | BDPT | σ²·T |
| `sculptors_studio` | mixed showcase | **VCM** | — *(ref noisy)* | σ²·T |
| `showroom` | mixed showcase (product viz) | **PT** | PT | σ²·T |
| `homogeneous_fog` | volumetric (homogeneous) | PT | **BDPT** | RMSE |
| `env_fog` | volumetric env-through-fog (VCM transmittance gap) | PT | **PT** | RMSE |
| `prism_dispersion` | spectral dispersion (prism) | **PT** | PT | σ²·T |
| `spectral_caustic` | spectral dispersive caustic | VCM | **VCM** | RMSE |
| `env_only` | env-IBL only (env is the illuminant) | PT | **BDPT** | RMSE |
| `env_mesh` | env-IBL + mesh emitter | BDPT | **PT** | RMSE |
| `corridor_100lights` | many-light | **PT** | PT | σ²·T |

**Reading the winners.** The decisive metric is **σ²·T** where all integrators converge to the same image (diffuse / glossy / mixed / many-light / prism), and **RMSE→truth** where luminance bias dominates (caustic / refractive / volumetric / env-IBL) — because there σ²·T would reward an integrator for *missing* the hard transport (a dark image is a low-variance image). **RMSE caveat:** the RMSE→truth column is only trustworthy when the reference integrator is itself converged *and* is a different integrator than the cell. Where the PT reference itself fireflies (`sculptors_studio`, PT σ/μ 5373 %) the column is marked '—' (a near-black BDPT render's low RMSE is an artifact, not a win); and where a cell is scored against its *own* high-SPP reference (VCM on the VCM-ref caustic/refractive scenes) its RMSE win is partly circular — so on a near-converged scene like `torus_chain` (all three within 8 % luminance) the σ²·T winner (PT) is the more meaningful read.

- **σ²·T-decisive classes → PT wins almost everywhere.** On `jewel_vault`, `cloister`, `ggx_showcase`, `showroom`, `corridor_100lights`, `prism_dispersion`, PT's per-sample cheapness (3-7× faster than BDPT/VCM) beats BDPT/VCM's lower *raw* variance. Note the inversion: on `cloister`/`ggx_showcase` BDPT and VCM have **lower mean σ² and lower RMSE** than PT, yet **lose** σ²·T because they cost 3-6× more — the wall-clock-normalized metric the user chose specifically exposes this.
- **BDPT wins the strong-indirect / glossy-interreflection scenes** `gi_spheres` (56× better σ²·T than PT) and `alchemists` — exactly the regime RENDERING_INTEGRATORS.md §5.2 predicts for BDPT, now confirmed with a 56× efficiency margin where the indirect is hard enough that BDPT's connections pay for themselves.
- **RMSE-decisive caustic/refractive → VCM wins** (`pool_caustics`, `diamond_teapot`, `torus_chain`, `spectral_caustic`) because it is the **only** integrator that reaches the transport: PT/BDPT under-deliver 44-78% of the energy (the 'transport reach' line per cell). `glass_pavilion` is pathological for *all three* (Inf fireflies — see §6/§7).
- **Volumetric / env-IBL → PT or BDPT win on RMSE, not VCM** — because VCM carries large luminance bias there (−30% fog, ±63% env; §6). This is the most important correctness caveat in the matrix.

---

## 6. Bias premium — VCM-vs-PT/BDPT, and env-IBL at production resolution

### 6.1 VCM finite-N bias (the §4.3 'bias premium' question)

On a well-behaved caustic (`pool_caustics`), VCM's finite-N bias is **negligible** — mean luminance moves only **+0.44% from 16→256 spp** (64→256 is +0.15%), i.e. VCM is already within half a percent of its converged answer at the trial SPP. The merge **radius** is the real bias/variance knob: 0.5×→2× the auto radius cuts variance **16×** (8.4e-3 → 5.1e-4) at the cost of **+2.9%** luminance (blur/bias). So on caustics VCM reaches, the bias premium is small and the radius-vs-variance tradeoff is the lever — not finite-N bias.

**`pool_caustics`** — auto merge-radius = 0.0281

SPP sweep (auto radius) — finite-N luminance convergence:

| spp | luminance | bias vs 16× | mean σ² |
|-----|-----------|-------------|---------|
| 16 | 0.15330 | +0.44% | 5.246e-03 |
| 64 | 0.15287 | +0.15% | 2.048e-03 |
| 256 | 0.15263 | +0.00% | 6.889e-04 |

Radius sweep (at 4× base spp) — bias/variance tradeoff:

| radius | luminance | mean σ² |
|--------|-----------|---------|
| 0.5× (0.01405) | 0.15157 | 8.423e-03 |
| 1.0× (0.0281) | 0.15287 | 2.090e-03 |
| 2.0× (0.0562) | 0.15597 | 5.072e-04 |

**`glass_pavilion`** — auto merge-radius = 0.037871

SPP sweep (auto radius) — finite-N luminance convergence:

| spp | luminance | bias vs 16× | mean σ² |
|-----|-----------|-------------|---------|
| 16 | n/a | n/a | n/a |
| 64 | n/a | n/a | n/a |
| 256 | n/a | n/a | n/a |

Radius sweep (at 4× base spp) — bias/variance tradeoff:

| radius | luminance | mean σ² |
|--------|-----------|---------|
| 0.5× (0.018936) | n/a | n/a |
| 1.0× (0.037871) | n/a | n/a |
| 2.0× (0.075743) | n/a | n/a |

`glass_pavilion` returns `n/a` throughout — its BDPT/VCM renders contain literal `Inf` pixels (a degenerate-pdf caustic-connection firefly), so the sweep is unusable; itself a finding (§7).

### 6.2 Env-IBL bias at production resolution — the large, scene-dependent VCM gap

The documented env-IBL residual (Sessions 9-13, `EnvLightBalanceTest` synthetic ~22%) shows up at production resolution as a **much larger and sign-inconsistent VCM luminance bias vs the PT reference**:

| scene | env regime | BDPT vs PT | VCM vs PT |
|-------|-----------|-----------|----------|
| `env_only` | env is sole illuminant (+background) | **+6.4%** | **+63.0%** |
| `env_mesh` | bright blocked env drives select-prob→1 | −0.8% | **−63.0%** |
| `env_fog` | env through bounded fog | +4.6% | **+8.9%** |
| `homogeneous_fog` | volume, no env | +1.9% | **−30.5%** |
| `corridor_100lights` | many-light, no env | +2.0% | **+76.4%** |

**VCM's env/volume luminance is unreliable: −63% to +76% across the corpus**, sign-flipping by scene (over-counts env_only/corridor, under-counts env_mesh/fog). BDPT stays within ±6.4% on every env/volume scene. This is the production-resolution confirmation that (a) the env-IBL SA-MIS refactor and (b) VCM media-aware connection transmittance (`VCMIsVisible` → transmittance walk) are real VCM correctness debts — **far larger in magnitude than the 22% the synthetic test suggested** outside the uniform-env Lambertian box. `homogeneous_fog` VCM −30.5% isolates the transmittance gap with no env at all. (The two `−63%` figures are opposite-sign — over vs under — so they are distinct phenomena that happen to share a magnitude.)

**Spectral note:** on `prism_dispersion`, spectral-BDPT delivers **−35.8%** vs spectral-PT — a large spectral-BDPT luminance gap on the hero-wavelength prism, a distinct spectral-BDPT finding (possibly HWSS-related) worth a separate look.

---

## 7. Highest-leverage scenes (every integrator noisy)

| scene | best-solving ig | its σ/μ | its σ²·T |
|-------|-----------------|---------|----------|
| `glass_pavilion` | PT | 6099.3% | 6.984e+02 |
| `spectral_caustic` | VCM | 476.6% | 7.925e+01 |
| `sculptors_studio` | VCM | 211.0% | 2.496e-01 |
| `showroom` | PT | 114.1% | 2.811e-01 |
| `torus_chain` | VCM | 89.3% | 5.121e+00 |
| `diamond_teapot` | VCM | 68.6% | 3.308e+00 |
| `env_only` | BDPT | 61.9% | 1.260e-01 |
| `alchemists` | BDPT | 50.6% | 2.281e+01 |
| `pool_caustics` | VCM | 47.6% | 2.999e-02 |
| `homogeneous_fog` | BDPT | 37.3% | 6.663e-02 |
| `prism_dispersion` | PT | 35.7% | 2.701e-01 |
| `jewel_vault` | PT | 31.1% | 2.203e+01 |
| `env_fog` | PT | 29.8% | 1.649e-04 |
| `cloister` | PT | 28.1% | 4.096e-02 |
| `env_mesh` | PT | 16.1% | 3.742e-04 |
| `ggx_showcase` | PT | 12.6% | 2.222e-02 |
| `corridor_100lights` | PT | 6.6% | 2.005e-01 |
| `gi_spheres` | BDPT | 3.1% | 4.913e-04 |

These are the scenes where **even the best-solving integrator is noisy** (high σ/μ) — where no current RISE integrator converges efficiently, the **highest-leverage targets for a new technique** (§6 of the analysis plan):

- **`glass_pavilion` (Inf), `spectral_caustic` (477%), `torus_chain` (89%), `diamond_teapot` (69%), `pool_caustics` (48%)** — caustic / refractive / dispersive. Even VCM (the winner) is noisy. These point at **Specular Polynomials** and **per-wavelength VCM photons** (spectral_caustic). **`glass_pavilion` `Inf` UPDATE (2026-06-03): the "degenerate-pdf firefly" was a MISDIAGNOSIS — root-caused to an FP16 EXR-write overflow of legitimate (finite) heavy fireflies; the integrators are correct. FIXED at the writer layer (`bpp 32` → 32-bit FLOAT EXR), fix in working tree, pending commit. See [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md) §1. A re-measure now yields large-but-finite variance (the scene is still a legitimate high-noise target for a new technique).**
- **`sculptors_studio` (211%)** — spot-light-dominated. **UPDATE (2026-06-03): the BDPT near-black (lum 0.0023) was NOT a delta-light failure but a delta-DIRECTION camera failure — the orthographic camera's phantom t=1 light-tracing strategy inverted the BDPT MIS (perspective camera recovers fully). FIXED (orthographic camera treated as delta-direction, t=1 skipped + excluded from MIS); BDPT now 0.178 ≈ PT 0.188. Fix in working tree, pending commit. See [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md) §2.**
- **`env_only` (62%), `alchemists` (51%)** — hard env-NEE and mixed transport; candidates for path guiding / ReSTIR-PT.

---

## 8. Coverage notes (honest accounting)

| scene | pt | bdpt | vcm | ref | K |
|-------|----|----|-----|-----|---|
| `jewel_vault` | ✓ | ✓ | ✓ | ✓ | 16 |
| `cloister` | ✓ | ✓ | ✓ | ✓ | 16 |
| `ggx_showcase` | ✓ | ✓ | ✓ | ✓ | 16 |
| `gi_spheres` | ✓ | ✓ | ✓ | ✓ | 16 |
| `pool_caustics` | ✓ | ✓ | ✓ | ✓ | 16 |
| `glass_pavilion` | ✓ | Inf⚠ | Inf⚠ | ✓ | 16 |
| `diamond_teapot` | ✓ | ✓ | ✓ | ✓ | 16 |
| `torus_chain` | ✓ | ✓ | ✓ | ✓ | 16 |
| `alchemists` | ✓ | ✓ | ✓ | ✓ | 16 |
| `sculptors_studio` | ✓ | ✓ | ✓ | ✓ | 16 |
| `showroom` | ✓ | ✓ | ✓ | ✓ | 16 |
| `homogeneous_fog` | ✓ | ✓ | ✓ | ✓ | 16 |
| `env_fog` | ✓ | ✓ | ✓ | ✓ | 16 |
| `prism_dispersion` | ✓ | ✓ | n/a | ✓ | 16 |
| `spectral_caustic` | ✓ | ✓ | ✓ | ✓ | 16 |
| `env_only` | ✓ | ✓ | ✓ | ✓ | 16 |
| `env_mesh` | ✓ | ✓ | ✓ | ✓ | 16 |
| `corridor_100lights` | ✓ | ✓ | ✓ | ✓ | 16 |

- **18 scenes × {PT,BDPT,VCM} at K=16 EXR trials each** (53 integrator cells + 18 references), all renders strictly sequential, benchmark thread mode (`render_thread_reserve_count 0`). **No K=4 fallback was needed** — the full corpus completed at K=16.
- **`glass_pavilion` BDPT/VCM = `Inf⚠`**: mean σ² overflowed on degenerate-pdf caustic fireflies (a real RISE firefly — §7, not fixed per measurement-only scope). Robust median/P99 still reported; PT finite but σ/μ=6099%.
- **`prism_dispersion` VCM = n/a by design**: PT/BDPT-spectral only (spectral-VCM merge uses a luminance proxy on a Pel-only photon store — known gap, SPECTRAL_PARITY_AUDIT §3).
- **`sponza_new` not run**: source imports a glTF from a hardcoded absent Windows path; architectural-diffuse still covered by `cloister` + `jewel_vault`.
- **VCM-bias sweep (§4.3)**: `pool_caustics` complete; `glass_pavilion` `n/a` (Inf fireflies).
- **Resolution downscaled ~320px wide, SPP tuned per scene (8-64)** for K-trial feasibility — both unbiased for per-pixel variance and for SPP-invariant σ²·T (validated §2). Absolute σ²·T is **not** comparable *across* scenes (different SPP/res/content), only *within* a scene across integrators (the ROI column).
- **PT+SMS and MLT not measured** (scope: PT/BDPT/VCM 'at minimum'). PT+SMS on the caustic scenes is the obvious next extension.
- **VCM-spectral dispersion-loss bias (analysis §4.3) not isolated cleanly.** It calls for a dispersive caustic measured against a *per-wavelength PT* reference — but PT cannot reach the dispersive-caustic transport, so a PT reference is missing the caustic energy (no clean truth exists). `spectral_caustic` is therefore measured against a VCM-spectral reference (which itself uses the luminance-proxy merge), and PT/BDPT-spectral show **+20%** reach vs that ref. The luminance-proxy dispersion-loss is a known correctness gap (SPECTRAL_PARITY_AUDIT §3) that needs a per-wavelength-photon VCM build to measure against, not just a reference choice.

---

## 9. What this implies for the Phase-3 decision (not a decision)

This is measurement, not the Phase-3 decision. What the data **implies** for the §6.3 candidate end-states:

1. **VCM-as-default (Candidate B) is contraindicated by the wall-clock metric.** VCM loses σ²·T on *every* class where all integrators converge (diffuse, glossy, mixed, many-light, prism) — typically 3-40× — from its photon-pass + kd-tree + merge cost, and carries **−63% to +76% luminance bias** on env/volume scenes. It *wins* only on caustic/refractive transport, and there because it is the *only* integrator that reaches it, not because it is efficient.

2. **The data favors a hybrid (Candidate C): PT/BDPT default + VCM caustic fallback.** PT wins the wall-clock race on the bulk of production classes; BDPT wins strong-indirect/glossy (`gi_spheres` 56×, `alchemists`); VCM is reserved for the caustic/refractive regime where PT/BDPT miss 44-78% of the energy. PT+BDPT together cover everything except caustics efficiently and *without* VCM's env/volume bias.

3. **The env-IBL SA-MIS refactor (5.2.1) + VCM media-aware transmittance (5.2.2) are empirically the highest-value VCM correctness work** — and matter most precisely *if* VCM is kept. The production-resolution env/volume bias (±30-76%) dwarfs the synthetic 22%, so any end-state making VCM a default must close them first. A **BDPT-centric (Candidate A)** path is least exposed: BDPT's env/volume bias is ≤6.4%.

4. **Highest-leverage scenes (§7) are all caustic/SDS/spectral-caustic** — where a *new* technique (Specular Polynomials, per-wavelength VCM photons, ReSTIR-PT) would pay off, since even the best current integrator is noisy there. The three by-product anomalies have since been investigated (2026-06-03, [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md)): (a) `glass_pavilion` "Inf" = an **FP16 EXR-write overflow** of legitimate fireflies (not an integrator 1/0) — **FIXED** (`bpp 32` → FLOAT EXR), pending commit; (b) `sculptors_studio` BDPT near-black = an **orthographic delta-direction camera** inverting the BDPT MIS — **FIXED** (delta-direction camera handling), pending commit; (c) `prism_dispersion` spectral-BDPT −36 % = the **deep HWSS spectral-bundle bias** (hwss=false closes it; PT is fine; ~17 % general + ~19 % dispersion-specific) — **documented & deferred** (multi-week). All fixes are uncommitted for review.

The matrix feeds the §6.3 decision gate; it does not pre-empt it.
