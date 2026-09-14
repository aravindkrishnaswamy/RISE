# DL-81 — padded Sobol' collapsed every bounce-to-bounce decision pair

Closed 2026-09-14 on branch `debt-sobol`.
Ledger row: `docs/DEBT_LEDGER.md` → DL-81.
Code: [`src/Library/Sampling/SobolSequence.h`](../src/Library/Sampling/SobolSequence.h),
[`src/Library/Cameras/CameraUtilities.h`](../src/Library/Cameras/CameraUtilities.h).
Tests: `tests/SobolDimensionParityTest.cpp` (sampler level),
`tests/SobolSelectionChannelBiasTest.cpp` (render level),
`tests/SobolDimensionBudgetTest.cpp` (aperture placement).

---

## 1. The defect

`SobolSequence::Sample` computed

```cpp
uint32_t sobolDim = dimension & 1;          // padded Sobol
uint32_t v        = Sobol( sampleIndex, sobolDim );
uint32_t dimSeed  = HashCombine( seed, dimension );
v = OwenScramble( v, dimSeed );
```

Only Sobol' dimensions 0 and 1 were ever generated. Every other requested
dimension reused one of those two and was distinguished **only by its Owen
scramble seed**.

That is not what padding means. Padding — Burley, *Practical Hash-based Owen
Scrambling* (JCGT 2020), and PBRT-v4's `PaddedSobolSampler` — also **permutes
the sample index** per dimension group. The permutation is the part that
decorrelates. Without it the two draws are `Owen_a(v_i)` and `Owen_b(v_i)` for
the *same* base value `v_i`, and Owen scrambling is defined digit by digit: the
leading digit of the output is the leading digit of `v_i` XOR one
per-dimension bit. Two same-parity dimensions were therefore locked together at
the coarsest digit **no matter what their scramble seeds were**.

`SobolSampler::kStreamStride` is 32 — even — and `StartStream( 16 + depth )`
puts eye bounce `d`'s stream at dimension `512 + 32d`. The k-th draw of every
bounce therefore had the same parity as the k-th draw of every other bounce, so
precisely the decisions that must be independent — Fresnel reflect/refract
choice, multi-lobe selection, Russian roulette at successive bounces — were the
locked ones.

### What the row got wrong

The ledger row recorded the collapse as a roughly 5 % departure in a 3-way
joint-occupancy probe, and said an independent scramble seed decorrelated the
pair. Direct measurement over 2^20 samples says otherwise — it is total, and
the seed does not help:

```
same parity   (512,544) same seed    worst 3x3 rel dev 1.24999   dyadic 2x2 x4 = [2.0000 0.0000 0.0000 2.0000]
diff parity   (512,545) same seed    worst 3x3 rel dev 0.00002   dyadic 2x2 x4 = [1.0000 1.0000 1.0000 1.0000]
same parity   (512,544) indep seed   worst 3x3 rel dev 1.81250   dyadic 2x2 x4 = [0.0000 2.0000 2.0000 0.0000]
```

Two of the four dyadic 2×2 boxes hold everything, at double density; the other
two are empty. The `indep seed` row is collapsed exactly as hard as the
`same seed` one — the relationship is between the digits, not between the seeds.

---

## 2. The fix

Every dimension now draws from its **own** Sobol' dimension.

The index permutation is not available in this class: it has to permute the
sample *set*, so it needs the samples-per-pixel count, which a progressive
renderer does not have at draw time. Genuinely distinct Sobol' dimensions need
no such parameter.

Direction numbers are **built at first use**, not tabulated:

* primitive polynomials over GF(2) enumerated in the canonical Sobol' order (by
  degree, then by increasing `a`, where the polynomial is
  `x^d + a_1 x^(d-1) + … + a_(d-1) x + 1`), each verified by checking that `x`
  has multiplicative order exactly `2^d - 1` in `GF(2)[x]/(p)`;
* initial direction numbers `m_1 … m_d` from a fixed hash of `(dimension, i)`,
  admissible (odd, `< 2^i`, so `m_1 = 1` always);
* Sobol's recurrence for the remaining direction numbers.

`kNumDimensions` = **2311**, the smallest prime above `50 × kStreamStride`;
289 KiB; **5.0 ms** one-time construction. A dimension at or above 2311 wraps
modulo 2311; because the prime is coprime to the stride, a wrap can alias one
individual draw of a very deep bounce with one draw of a shallower one, never
two whole streams.

**Dimensions 0 and 1 are bit-identical to the previous `SobolDim0` /
`SobolDim1`** — dimension 0's direction numbers are the powers of two (van der
Corput) and dimension 1 is the first primitive polynomial, `x+1`, whose only
admissible initial number is `m_1 = 1`. The image plane's `Get2D()` therefore
still draws exactly the (0,2)-net it always did. Pinned by the parity test's
sections A and E2.

### Initial direction numbers must not all be 1

The obvious choice, `m_i = 1` throughout, was tried first and is **wrong in a
way that would have reproduced the original bug**: with unit initial numbers
every dimension of degree ≥ d shares the same first d direction numbers
(`2^31, 2^30, …`), so two dimensions of equal degree produce **bit-identical
values for every sample index below 2^d**. Degree 13 covers dimensions
481…1110 — the whole eye-bounce range — so at any production sample count
(2^13 = 8192) that choice collapses one degree class at a time. It measured as
clean only because the first probe ran at 2^20 samples.

### The aperture stream

`BDPTCameraUtilities::kApertureSamplerStream` was 8192, justified explicitly by
the padding: *"has no dimension capacity to exhaust … dimension 8192×32 =
262144, which is just another hash seed."* With a finite supply that premise is
gone: 262144 mod 2311 = **1001**, which is stream 31 slot 9 — eye bounce 15,
well inside a normal render. The constant is now **3322**, the smallest stream
above the worst-case walk stream (3121) whose dimension wraps to the *end* of
the table: 3322×32 mod 2311 = 2309, so the aperture's two dimensions are 2309
and 2310, and the first stream that can reach them is 72 — eye bounce 56.
`SobolDimensionBudgetTest` Test F asserts both properties.

---

## 3. What this does **not** fix, and why no direction numbers could

Some *pairs* of the 2311 dimensions are still poorly distributed against each
other at low sample counts. That is a counting fact, not a defect of the
initial direction numbers: over the first `2^M` samples only index bits
`0 … M-1` vary, so a dimension's leading generator row has just `2^(M-1)`
possible values there (`m_1 = 1` pins one bit). With 2311 dimensions and
M = 8 — 256 samples per pixel — there are 128 values to go round, so some pairs
**must** share a leading row, which is a collapsed 2×2 occupancy for that pair.

Measured on the shipped generator (`Sobol(2^j, d)` for j < M, all 2311
dimensions):

| samples/pixel | distinct leading rows | of possible | dimension pairs sharing one | largest bucket |
|---|---|---|---|---|
| 256   | 128  | 128  | 0.783 % | 32 |
| 512   | 256  | 256  | 0.393 % | 19 |
| 1024  | 506  | 512  | 0.198 % | 12 |
| 2048  | 916  | 1024 | 0.099 % |  9 |
| 4096  | 1387 | 2048 | 0.048 % |  6 |
| 8192  | 1752 | 4096 | 0.025 % |  4 |

A perfectly balanced assignment of 2311 dimensions into 128 buckets would give
0.738 % at 256 spp, so there is at most ~6 % of headroom on that particular
bound and Joe & Kuo's searched tables could not claim it either. The same argument one level down — a partial spread
of 2-dimensional subspaces of GF(2)^8 has at most `(2^8-1)/(2^2-1) = 85`
members — bounds how many dimensions can be pairwise 4×4-exact at 256 spp.

The pre-fix padding had the collapse on **100 %** of same-parity pairs, which
was every bounce-to-bounce pair, at **every** sample count. So the change is
100 % → 0.78 % at the worst sample count, and better from there.

A greedy per-dimension search over hash salts, maximising the 2D resolution
against the dimensions a renderer is most likely to pair each one with, was
implemented and **discarded**: it cleaned the stride-32 pairs it targeted but
broke film-vs-bounce pairs that had been fine, i.e. it moved the defect rather
than removing it — which the counting bound above says it must. It also cost
50 ms of one-time construction. Adopting Joe & Kuo's `new-joe-kuo-6.21201`
tables would improve the higher dimensions' t-values; they are external data
this build does not carry.

---

## 4. Measurements

### 4a. Render fixture — `SobolSelectionChannelBiasTest`, 1024 spp

A lossless (`tau 1.0`, `scattering 1000000`) 2-deep dielectric box, white
Lambertian luminaire (exitance 1) directly behind, pinhole on axis at fov 10,
24×24, `pathtracing_pel_rasterizer`, `oidn_denoise FALSE`, `pixel_filter box`.
The index triple gives **channels 0 and 1 the identical index**, so `R == G`
needs no closed form at all.

Closed form per channel: `L·T²/(1-R²)`, `L = 1/π`, `R = R0(n) = ((n-1)/(n+1))²`,
`T = 1-R` — `T²` for the straight-through path times the geometric series over
the paths that internally reflect off both faces and exit, which at normal
incidence all land in the same pixel. (The DL-81 row's closed form omitted the
series. It is worth 0.81 % on the shared index, and it changes the reading of
the row's own control: the pre-fix non-dispersive control was not 0.26 % *above*
the right answer, it was 0.52 % *below* it.)

| row | | before | after | closed form |
|---|---|---|---|---|
| A  dielectric, dispersive | R | 0.280992 | 0.265585 | 0.266053 |
| | G | 0.250685 | 0.266349 | 0.266053 |
| | B | 0.241632 | 0.244792 | 0.245270 |
| | **R/G − 1** | **+12.09 %** | **−0.287 %** | 0 |
| B  dielectric, non-dispersive control | R | 0.264661 (−0.523 %) | 0.266111 (+0.022 %) | 0.266053 |
| C  perfectrefractor, dispersive | R | 0.263745 | 0.265114 | 0.266053 |
| | G | 0.252972 | 0.266981 | 0.266053 |
| | B | 0.227891 | 0.246584 | 0.245270 |
| | **R/G − 1** | **+4.259 %** | **−0.699 %** | 0 |
| D  polished, dispersive mirror coat | R/G − 1 | +0.006 % | −0.012 % | 0 |

Counters: **11 passed / 8 failed → 19 passed / 0 failed.**

The old numbers are a **bias**, not a truncation error: row A's R/G departure
was 11.94 % at 256 spp and 12.09 % at 1024 spp — it did not converge. The new
numbers do:

| spp | R/G − 1 | R dev | G dev | B dev |
|---|---|---|---|---|
| 64   | +2.05 % | −1.07 % | −3.06 % | +0.12 % |
| 256  | +1.15 % | +1.06 % | −0.10 % | −0.25 % |
| 1024 | −0.29 % | −0.18 % | +0.11 % | −0.19 % |
| 4096 | −0.22 % | −0.15 % | +0.07 % | −0.12 % |

All three channels are inside ±0.15 % of the closed form by 4096 spp, against
the non-dispersive control's own +0.02 %.

Row D is a **consistency pin**, not a red-proof: a mirror coat at normal
incidence makes one stochastic lobe selection, and the defect needs two
selections at successive bounces to express, so it was green before and after.

### 4b. Joint occupancy — `SobolDimensionParityTest`

2^20 samples, dimensions 512+k (eye bounce 0) against 544+k (eye bounce 1):

| pair | before: worst 3×3 dev | dyadic 2×2 ×4 | after: worst 3×3 dev | dyadic 2×2 ×4 |
|---|---|---|---|---|
| (512,544) same seed | 1.24999 | [2, 0, 0, 2] | 0.00012 | [1, 1, 1, 1] |
| (512,545) diff parity | 0.00002 | [1, 1, 1, 1] | 0.00027 | [1, 1, 1, 1] |
| (512,544) indep seed | 1.81250 | [0, 2, 2, 0] | 0.00014 | [1, 1, 1, 1] |

Sweeping all 32 slots of stream 16 against stream 17:

| | before | after |
|---|---|---|
| N = 256, worst 2×2 dyadic dev | 1.00000 | 1.00000 (one slot) |
| N = 256, slots with a 2×2 collapse | **32 / 32** | **1 / 32** |
| N = 4096, worst 2×2 dyadic dev | 1.00000 | 0.00000 |
| N = 4096, worst 4×4 dyadic dev | 3.00000 | 0.00000 |
| N = 4096, slots with a 2×2 collapse | **32 / 32** | **0 / 32** |

The one remaining collapsed slot at 256 spp is the counting bound of §3 showing
up: ~0.78 % of all dimension pairs must collapse there, which is ~0.25 of these
32.

Counters: **17 passed / 18 failed → 35 passed / 0 failed.**

### 4c. Convergence and variance — cornell box, PT

`scenes/Tests/Samplers/cornell_pt_16spp_sobol.RISEscene` at 128×128,
`oidn_denoise FALSE`, `pixel_filter box`, file outputs stripped, image captured
in memory (no PNG quantisation). Ground truth is an **independent** render:
the fixed build, `blue_noise_sampler TRUE` (ZSobol), 8192 spp — a different
sample-index mapping, so it is not a prefix of either candidate sequence.
RMSE is over linear RGB; each figure is the mean of 4 runs.

| spp | RMSE vs truth, before | RMSE vs truth, after |
|---|---|---|
| 16   | 0.09514 | 0.09539 |
| 64   | 0.04907 | 0.05353 |
| 256  | 0.03672 | **0.02078** |
| 4096 | 0.03327 | **0.00588** |

Convergence rate between successive rows: before `N^-0.48` then `N^-0.21`;
after `N^-0.42` then `N^-0.68`. **The old sampler stops converging** — from 256
to 4096 spp, a 16× sample increase, its RMSE moves only 0.0367 → 0.0333,
because it has converged to the wrong image. The two builds' own 4096-spp
renders differ by RMSE 0.0335, essentially all of which is the old build's
distance from truth. Scene mean: truth 0.473113, after 0.473115 (+0.0004 %),
before 0.473494 (+0.080 %).

The new sampler is ~9 % worse at 64 spp and 1.77× better at 256. The low-spp
crossover is the §3 counting bound (a minority of dimension pairs is still
poorly distributed at 2^6–2^8 samples) and it is not a bias: by 256 spp the
ordering has reversed and it keeps widening.

### 4d. Cost

Isolated per-draw cost of `SobolSequence::Sample`, 2×10^6 draws over 2000
dimensions (a deliberately cache-hostile sweep), best of 3:

| | before | after |
|---|---|---|
| ns per draw | 1.81 | 7.06 |

End-to-end, the same cornell-box PT render, ns per sample, best of 3–4 runs on
a machine with concurrent load:

| spp | before | after | delta |
|---|---|---|---|
| 16  | 481.6 | 511.4 | +6.2 % |
| 64  | 492.3 | 517.1 | +5.0 % |
| 256 | 481.5 | 519.7 | +7.9 % |

So roughly **+5 – 8 % wall clock per sample**, against 1.77× lower RMSE at
256 spp — a net win on equal-time as well as equal-sample terms. One-time table
construction is 5.0 ms per process, on first use.

---

## 5. What changed for users

* **Every `SobolSampler` / `ZSobolSampler` render's noise pattern changes.**
  That is every `pathtracing_*`, `bdpt_*`, `vcm_*`, `pixelpel_*` and
  `pixelspectral_*` rasterizer. Images are not bit-comparable with pre-fix
  renders.
* **Means change only where selection correlation was biasing them.** The
  affected shape is *repeated stochastic selection at successive bounces*:
  RGB dispersion in `DielectricSPF` / `PolishedSPF` / `PerfectRefractorSPF`
  (the confirmed instance, up to 12 % per channel), and Fresnel
  reflect/refract choice, multi-lobe selection and Russian roulette wherever
  they stack across bounces. A render with one selection event per path — the
  row-D mirror coat — is unchanged to 0.01 %.
* **Thin-lens renders** additionally move because the aperture stream changed
  (see §2).
* No scene-file, API or ABI change. No new source file.

## 6. Sibling audit

| candidate | verdict |
|---|---|
| `ZSobolSampler` | **Same defect, fixed by the same change** — it derives from `SobolSampler` and only remaps the sample index. |
| The legacy `PixelBasedPelRasterizer` / `PixelBasedSpectralIntegratingRasterizer` samplers | **Same defect, fixed by the same change** — both construct `SobolSampler` / `ZSobolSampler` directly. |
| BDPT / VCM / MLT light- and eye-subpath streams | **Same defect, fixed by the same change** — all go through `SobolSampler::StartStream`. |
| `SobolSampling2D` | **Refuted.** Draws dimensions 0 and 1 only, which are bit-identical before and after. |
| `PSSMLTSampler` (primary sample space) | **Refuted — structurally different.** `idx = streamIndex + kNumStreams * sampleIndex` indexes a vector of independent uniforms; there is no base-dimension reuse to collapse. Its own known stream-aliasing overrun (`kNumStreams` = 49 vs BDPT reaching stream 48 at eye depth 32) is a separate, already-recorded debt, unaffected by this change. |
| `IndependentSampler` | **Refuted.** A plain RNG; no dimensions. |
| Photon tracers (`GlobalPel`, `GlobalSpectral`, `CausticPel`, `CausticSpectral`, `TranslucentPel`, `ShadowPhotonTracer`) | **Refuted.** Every one wraps a `RandomNumberGenerator` in `IndependentSampler`; none consumes a Sobol dimension. |
| `BDPTCameraUtilities::kApertureSamplerStream` | **Downstream consumer, fixed here** — it relied on the padding's unbounded dimension capacity. See §2. |
| `PolishedSPF` ~:187, `PerfectRefractorSPF` ~:227 per-channel dispersion loops | **Healthy after the fix**, verified by closed-form fixture rows C and D. Both are downstream victims, not independent defects: their loops emit per-channel rays correctly, and the channel-dependent error came entirely from the selection draws. |

---

## 7. Gate, including the three bands this moved

Run on the fix, in the `debt-sobol` worktree. Clean rebuild, zero warnings.

| suite | counter |
|---|---|
| `SobolDimensionParityTest` | 35 passed / 0 failed (was 17 / 18) |
| `SobolSelectionChannelBiasTest` | 19 passed / 0 failed (was 11 / 8) |
| `SobolDimensionBudgetTest` | all passed |
| `TranslucentSamplerDimensionCountTest` | passed |
| `ZSobolSamplerTest` | all passed |
| `PSSMLTStreamAliasingTest` | all passed |
| `SSSRadianceScalingTest` | 574017 guards / 0 failed |
| `AgentProposeRenderTest` | 559 passed / 0 failed |
| `MISWeightsTest` | 59 / 0 |
| `PTGuidedSelectProbTest` | passed |
| `RayCasterEnvEscapeMISTest` | 91 / 0 |
| `GGXWhiteFurnaceTest` | passed |
| `LayeredWhiteFurnaceTest` | 0 of 57 configurations failed |
| `RefractiveRadianceScalingTest` | 38 / 0 |
| `CstDeriveGoldenTest` | passed |
| `SourceHygieneTest` | 165 passed / 0 failed (scanned 338 test files) |
| `FabricRenderTest` | 57 / 0 |
| `HairRenderTest` | 26 / 0 |
| `VCMStrategyBalanceTest` | 55 / 0 |
| `BDPTStrategyBalanceTest` | 65 passed / **1 failed** |
| `EnvLightBalanceTest` | 114 passed / **2 failed** |

(The task list also named `PTGuidingMISPartitionTest`; no such file exists in
`tests/`. `PTGuidedSelectProbTest`, `MISWeightsTest` and
`RayCasterEnvEscapeMISTest` were run in its place.)

Both suites are **fully green on the pre-fix library** (`EnvLightBalanceTest`
116 / 0, `BDPTStrategyBalanceTest` 66 / 0, measured on this branch with only
`SobolSequence.h` and `CameraUtilities.h` reverted). The three failures are
therefore caused by this change — and **none of them is a mean**. All three are
p99 tail statistics whose bands were calibrated against the old sampler's noise
shape. They have **not** been retuned.

**1. `BDPTStrategyBalanceTest` — "BDPT p99 within 60 % of PT: submerged
Lambertian floor, sphere emitter in air (eta^2 cancellation, debt 30)"**

| | PT p99 | BDPT p99 | relative diff |
|---|---|---|---|
| before | 0.0131186 | 0.0163977 | 25.0 % (pass) |
| after  | **0.00983884** | 0.0163978 | **66.7 %** (fail, band 60 %) |

BDPT's p99 is **identical to six digits**. What moved is PT's: its 99th
percentile fell 25 %, while its mean moved −0.12 % (0.00462557 → 0.00461997)
and its median is unchanged to six digits (0.00327966). The new sampler gives
PT a tighter noise tail on this scene, which widened a gap the band measures as
a difference.

**2. `EnvLightBalanceTest` — "VCM p99 ratio-to-PT within 5 % of measured bias:
env + mesh emitter"** (band centre 1.5223)

| | PT p99 | VCM p99 | ratio |
|---|---|---|---|
| before | 0.525590 | 0.800721 | 1.5235 (pass) |
| after  | 0.527257 | **1.02090** | **1.9363** (fail) |

VCM's **mean** moves +0.27 % (0.779844 → 0.781987) and its max +1.2 %
(1.05301 → 1.06568); PT's mean moves −0.034 %. Before, VCM's p99 sat at 76 % of
its own max — a narrow body with an isolated bright tail; after, at 96 % — a
smooth one. The bias statistic this row exists to pin (the *mean* ratio to PT)
is unaffected and still passes.

**3. `EnvLightBalanceTest` — "VCM p99 ratio-to-PT within 12 % of measured bias:
env-only Lambertian (spectral, hwss=true)"** (band centres 1.4135 / 1.4953 /
1.4293)

| | VCM p99 | VCM max | VCM mean | ratio to PT p99 |
|---|---|---|---|---|
| before | (0.891276, 0.889168, 0.892142) | (0.904246, 0.903399, 0.933261) | (0.60401, 0.602167, 0.597554) | 1.4243 / 1.5078 / 1.4029 (pass) |
| after  | (0.628072, 0.623409, 0.635372) | (0.639515, 0.636896, 0.651812) | (0.604729, 0.603268, 0.599054) | **1.0109 / 1.0400 / 1.0137** (fail) |

Here the tail **collapsed toward the body**: VCM's max fell 29 % while its mean
moved +0.12 %. That is fewer bright outliers at the same energy — a firefly
reduction — which broke a band calibrated on the old tail.

### Did the truth-referenced PT rows move toward or away from their closed forms?

Every one of them still passes. The only closed form whose value the output
exposes directly is the env-only Lambertian PT mean, which must equal **0.5**
(albedo × uniform env) within `kAbsBandRGB` = 1 %:

| | PT mean | departure from 0.5 |
|---|---|---|
| before | 0.500023 | +0.0046 % |
| after  | 0.499913 | −0.0174 % |

So on that row it moved marginally **away** — by 1.1 × 10⁻⁴ absolute, against a
band 57× wider than the resulting departure. The move is deterministic, not
noise (the renders are bit-identical across seed bases; re-running at seed base
2000 reproduces 0.499913 exactly), but it is two orders of magnitude inside the
check's own tolerance and carries no verdict about the fix. Every other
truth-referenced PT row — the spectral luminance = 0.5 checks, the PT light
increment over the env-only baseline, and the submerged-camera / delta
dielectric-shell closed form — passes in both builds; none printed a value,
which in this suite means none was outside its band.

**Recommendation, not taken here:** these three bands describe the noise shape
of the *old* sampler and should be re-derived, with their own measurement, by
whoever next touches those suites — not silently widened.
