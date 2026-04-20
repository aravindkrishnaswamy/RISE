# MMLT (Multiplexed Metropolis Light Transport) in RISE

This document describes the MMLT integrator (Hachisuka et al., *Multiplexed Metropolis Light Transport*, ACM TOG 33(4), 2014), an adaptation of PSSMLT that pools Markov chains per path-length depth so per-strategy luminance — not the summed luminance over all strategies — drives chain density.

## Scope

- **MMLTRasterizer** in [src/Library/Rendering/MMLTRasterizer.{h,cpp}](../src/Library/Rendering/MMLTRasterizer.h), parallel to **MLTRasterizer** (PSSMLT).  Both rasterizers coexist; MMLT does not replace PSSMLT.
- Pel (RGB) only.  No spectral MMLT yet.
- Pinhole and ThinLens cameras.  Pinhole is unaffected by `lensSample` (falls through `GenerateRayWithLensSample`); ThinLens uses it for continuous aperture mutations exactly as PSSMLT does.
- No SMS interop.  Phase 6 design notes (below) outline a future SMS-pool extension if SDS scenes need it.
- No OpenPGL guiding.  MMLT explicitly bypasses `EvaluateAllStrategies`'s OpenPGL branches — path-guiding statistics from MMLT samples are highly correlated and would corrupt training.
- No directional / ambient light support — MMLT's per-depth bootstrap cannot route through `EvaluateAllStrategies`'s zero-exitance fallback branch.  PBRT v3 MMLT documents the same restriction.

## Why MMLT exists alongside PSSMLT

PSSMLT runs ONE Markov chain whose density adapts to the **summed** luminance over all `(s,t)` connection strategies for each path it proposes.  On SDS scenes, dim strategies (e.g., diffuse-wall paths) get visited rarely because the chain spends most of its time on the bright caustic strategy that dominates the sum.  Quantitative measurements on the [scenes/FeatureBased/MLT/mlt_veach_egg.RISEscene](../scenes/FeatureBased/MLT/mlt_veach_egg.RISEscene) test scene: 5× more PSSMLT samples reduce noise by only ~30% (vs the ~56% i.i.d. `1/√N` would predict).  The dominant residual variance is chain autocorrelation from being trapped in dim-strategy starvation.

MMLT addresses this by partitioning the chain budget into **one pool per path-length depth** `d = s + t - 2`.  Each chain in pool `d` only ever evaluates strategies `(s,t)` with `s+t = d+2`, and its density adapts to the per-DEPTH luminance `b_d`.  Diffuse-wall contributions at depth 6 get exactly the budget they earn from their share of the total path-space integral, independent of how bright the depth-3 caustic is.

Cornell-box validation (single-strategy Phase 5 build): MMLT mean luminance within 1–2% of PSSMLT, image RMSE 6.97 vs noise floor √2·σ ≈ 18.8 (well below systematic-bias threshold).  Per-depth additivity check passes: Σ(force_depth=0..7) = multi-depth render within Monte Carlo noise on every channel.

## Architecture

### Rasterizer flow

`RasterizeScene` runs as six steps:

1. **Active-depth determination** — `forceDepth >= 0` restricts to one depth (debug); `forceDepth < 0` (default) enumerates all `d ∈ [0, maxLightDepth + maxEyeDepth]` whose `MMLTSampler::CountStrategiesForDepth` is positive.
2. **Per-depth bootstrap** — for each active depth, run `nBootstrap` independent samples bound to that depth via the `BootstrapAtDepth` helper.  Each sample evaluates ALL valid `(s,t)` strategies at the depth and SUMS the MIS-weighted luminances (PBRT v4 multi-strategy pattern; see "Multi-strategy bootstrap" below).  `bd[d] = mean(per_sample_sum) * numPixels`.
3. **Per-depth CDF** — `BuildCDF` produces a normalized CDF over each depth's bootstrap luminances for chain-init importance sampling.
4. **Chain budget allocation** — `AllocateChainsPerDepth(bd, totalChains)` distributes the chain budget proportionally: `chainsPerDepth[d] = max(1, floor(N * bd[d] / Σbd))` for nonzero `bd[d]`, with trim-to-budget logic when over-allocated and a depth-drop loop (with warning log) when the active-depth count exceeds the chain budget.
5. **Render** — round-based progressive loop (mirrors `MLTRasterizer`'s pattern).  Each round runs `mutationsPerChainPerRound` mutations on every chain via work-stealing across the flat chain list.  Per-depth films persist across rounds; only the resolved output image is freshly allocated per round.  Adaptive resize after round 0 corrects for the bootstrap's systematically optimistic per-mutation cost estimate.
6. **Resolve** — at each round boundary (and on cancellation), resolve all per-depth `SplatFilm`s additively into a fresh image scaled by `mutationsDonePerChain / mutationsPerChain`.  Final round writes to file via `OutputImage`; intermediate rounds emit `OutputIntermediateImage` for GUI preview.

### MMLTSampler

[src/Library/Utilities/MMLTSampler.{h,cpp}](../src/Library/Utilities/MMLTSampler.h) extends `PSSMLTSampler` with three reserved primary-sample-vector streams beyond PSSMLT's 49:

- Stream 48: film position (same as PSSMLT)
- Stream 49: `(s,t)` strategy selection (NEW — uniform discrete map via `floor(u * nStrategies)`)
- Stream 50: lens position (NEW — split out of stream 48 so MMLT's discrete strategy choice is independent of small-step film mutations)

The 51-stream layout requires `kNumStreams = 51` for `MMLTSampler`.  PSSMLT keeps `kDefaultNumStreams = 49` so its primary sample vector layout — and therefore its bit-stable rendered output — is unchanged.  The split was implemented by promoting `PSSMLTSampler::kNumStreams` from `static const int` to a per-instance member with a `kDefaultNumStreams` fallback for the public ctor.

### Splat math (single-strategy chain)

Per chain mutation at depth `d`:

```
PickStrategyST → (s, t, nStrategies)             // uniform discrete
weighted = cr.contribution * cr.misWeight * nStrategies
           // multiply by nStrategies to undo the 1/nStrategies
           // selection PDF — PBRT v3 MLTIntegrator::L pattern
luminance = scalar_luminance(weighted)
splat_p = weighted * (accept * b_d / (luminance * N_d))
```

Where `N_d = chainsPerDepth[d] * mutationsPerChain` and `accept = min(1, lum_proposed / lum_current)` is the standard MH acceptance ratio.  Veach's expected-value technique splats both proposed and current with weights `accept` and `1 - accept`.

The full image is `Σ_d (per-depth film at depth d resolved with sampleCount = fraction-of-mutations-done)`.

## Multi-strategy bootstrap (Phase 6)

The bootstrap is the key place where MMLT's per-depth structure can break down: if a single bootstrap sample randomly picks `(s,t)` and that one strategy contributes 0 (which is common for SDS path families where most strategies are unreachable), the entire sample contributes 0.  Phase 4 saw `bd[4..8] = 0` on the Veach Egg because the rare successful `(s,t)` at each deep depth was almost never the random pick.

Phase 6 fixes this by running the bootstrap with the **same RNG consumption pattern** as the chain (`PickStrategyST → film → lens → BDPT subpaths`, so chain init still reproduces the bootstrap path on iteration 1) but evaluating **ALL valid `(s,t)` at the depth and summing** the MIS-weighted contributions instead of just the picked one.  `b_d` becomes the sum's mean times `numPixels`, which is a much lower-variance estimator of the same per-depth integral.

Empirical impact on Veach Egg (200 mut/pixel, 1024 chains):

| Quantity | PSSMLT (b reference) | Phase 5 MMLT (single-strategy bootstrap) | Phase 6 MMLT (multi-strategy bootstrap) |
|---|---|---|---|
| Total b | 95320 | 81328 (−14.7%) | **91620 (−3.9%)** |
| `bd[4..8]` | (sum across all depths) | 0 each | 0 each (genuinely unreachable, see below) |

The remaining `bd[4..8] = 0` is **not** a bootstrap-coverage gap — it's a geometric reality.  At depth 4, the only potentially-contributing strategies are `(4,2)` (light vertex 3) and `(5,1)` (light vertex 4), both requiring the light path to escape both glass shells AND land on a diffuse wall at exactly that vertex index.  PSSMLT's `b = 95320` is dominated by depths 2 and 3; deeper depths contribute very little to PSSMLT either.  Multi-strategy bootstrap closes the MMLT vs PSSMLT gap from 14.7% to 3.9% which is essentially MC noise.

Cornell-box validation: per-depth `bd` distribution unchanged within MC noise; back-wall stddev R tightened from 17.45 to 16.63 (consistent with lower-variance `b_d` estimator).

## Configuration

In a scene file:

```
mmlt_rasterizer
{
    max_eye_depth        4
    max_light_depth      4
    bootstrap_samples    100000
    chains               1024
    mutations_per_pixel  200
    large_step_prob      0.3
    # force_depth        N    # debug: restrict to one path-length depth
}
```

Defaults match `mlt_rasterizer` where applicable.  `force_depth` is a debug knob that pins every chain to one depth — useful for isolating per-depth contribution but produces an incomplete image.  Not for production renders.

## Memory scaling

Per-depth films are allocated only for depths with `chainsPerDepth[d] > 0`.  In the worst case (`maxLightDepth = maxEyeDepth = 12`), up to `25` films can live concurrently.  Each `SplatFilm` holds `width × height × sizeof(SplatPixel)` plus one `RMutex` per scanline.  At 256×256 with `~25` active depths the overhead is ~12 MB total — negligible.  At 1024×1024: ~800 MB.  At 4K (3840×2160): ~40 GB — **infeasible without revising the per-depth-film design**.

If high-resolution MMLT becomes a goal, the per-depth films could be replaced with a single shared `SplatFilm` whose splat weight uses the chain's own `bd / Nd` at deposit time (mathematically equivalent up to floating-point ordering).  This trades the documented per-depth additivity simplicity for memory.

## Critical files

- [src/Library/Rendering/MMLTRasterizer.{h,cpp}](../src/Library/Rendering/MMLTRasterizer.h) — rasterizer, chain orchestration, multi-strategy bootstrap
- [src/Library/Utilities/MMLTSampler.{h,cpp}](../src/Library/Utilities/MMLTSampler.h) — sampler, strategy selection, PDF accounting
- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) `ConnectAndEvaluateForMMLT` — single-strategy entry point routed through the same `ConnectAndEvaluate` body PSSMLT uses
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) `MMLTRasterizerAsciiChunkParser`
- [src/Library/Job.cpp](../src/Library/Job.cpp) `Job::SetMMLTRasterizer`
- [src/Library/RISE_API.cpp](../src/Library/RISE_API.cpp) `RISE_API_CreateMMLTRasterizerWithFilter`
- [tests/MMLTStrategySelectionTest.cpp](../tests/MMLTStrategySelectionTest.cpp) — strategy enumeration unit tests (66,298 assertions)

## Known limitations

1. **Deep-depth coverage on extreme SDS scenes** — even with multi-strategy bootstrap, depths whose only contributing strategies require both light-shell escapes AND specific diffuse-vertex placements may show `bd[d] = 0` if those configurations are below the bootstrap noise floor.  MMLT shares this limitation with PBRT v3/v4 MMLT.  PSSMLT's summed-strategy bootstrap masks this because the sum is averaged across many depths, but the actual contribution to the rendered image is the same.
2. **Single-strategy chain variance** — MMLT chains pick one `(s,t)` per mutation, which has higher per-mutation variance than PSSMLT's all-strategies-summed evaluation.  At equal mutation budgets, MMLT can be slightly noisier on well-mixed scenes (Cornell: ~15% higher stddev).  The convergence advantage shows up on SDS scenes where the per-depth chain density adaptation pays off.
3. **No directional / ambient lights** — see Scope above.
4. **No OpenPGL path guiding** — MMLT samples are highly correlated and would corrupt training.  PSSMLT remains the choice for path-guiding workflows.
5. **No spectral variant** — `MLTSpectralRasterizer` exists but `MMLTSpectralRasterizer` does not.  Future work if needed.

## Phasing reference

Implementation phases (commit history):

- **Phase 1**: `BDPTIntegrator::ConnectAndEvaluateForMMLT` single-strategy entry point.
- **Phase 2**: `MMLTSampler` with reserved streams 49 (s,t) and 50 (lens), composition over `PSSMLTSampler`.
- **Phase 3**: `MMLTRasterizer` skeleton with `force_depth` single-depth mode.
- **Phase 4**: Per-depth chain pools, per-depth bootstrap, per-depth `SplatFilm` summation.
- **Phase 5**: Round-based progressive rendering with adaptive round resize.
- **Phase 6**: Multi-strategy bootstrap, documentation, polish.
