# L8 Interactive Render Path — Production Perf Impact Report

**Date**: 2026-05-11
**Baseline**: commit `97fb593a` (FilmIntrospection NaN guard — the last commit before the
L8 interactive-rendering rework began at `386c488a`)
**HEAD**: commit `0cb35194` (ViewportFrameStore log demote, end of L8 round-18 series)
**Question**: Did the L8 interactive-renderer work add measurable overhead to the
production rendering path (`bin/rise` CLI), and did it preserve correctness?

## TL;DR

- **Aggregate (sum-of-means across 7 scenes): +0.38%** — essentially flat.
- **Correctness invariant: holds.** Per-scene image RMSE between HEAD and baseline
  equals the HEAD-vs-HEAD-self MC noise floor (ratio 1.00 ± 0.02 for all stochastic
  scenes; bit-identical for the deterministic MLT bootstrap).
- **One regression**: `cornellbox_bdpt_spectral` (bdpt_spectral) +14.2%, statistically
  clean (min-HEAD > max-baseline). Documented as follow-up; not blocking.
- **Two opportunistic improvements**: `bench_bdpt` (-9.1%) and `bench_vcm` (-5.9%) —
  likely a side-effect of the `d68ace25` BidirectionalRasterizerBase scratch-image
  reallocation fix that landed alongside the L8 work.

## Methodology

Per [docs/skills/performance-work-with-baselines.md](skills/performance-work-with-baselines.md):

- **Binaries**: separate make builds of `97fb593a` (baseline) and `HEAD = 0cb35194`
  on the same Apple Silicon host, identical compile flags. Both binaries stashed at
  `/tmp/rise-baseline-bin` and `/tmp/rise-head-bin`.
- **Settings**: `render_thread_reserve_count 0` and `force_all_threads_low_priority false`
  via `RISE_OPTIONS_FILE` so every core gets a worker (no Apple-Silicon E-core
  reservation bias).
- **Protocol per cell**: one discarded warmup, then **K=5** trials, sequential.
- **No parallel renders** (per user policy — would saturate the CPU and pollute
  measurements).
- **Metric**: rasterizer-internal `Total Rasterization Time` (excludes scene-load
  and output-encode). Wall-clock — captured from the log.
- **Stats**: arithmetic mean, sample standard deviation (Bessel-corrected, N-1).
- **Correctness check**: re-render every scene once on each binary, pixel-diff with
  PIL/NumPy (MAE, RMSE, max|Δ|, PSNR). Separately re-render on HEAD twice to
  establish the intrinsic MC-noise floor for ratio comparison.

## Scene matrix

Seven scenes covering six distinct rasterizer types:

| Scene | Rasterizer | Notes |
|---|---|---|
| `scenes/Tests/Geometry/shapes.RISEscene` | `pixelpel_rasterizer` | Sample scene, direct illum |
| `scenes/Tests/Bench/bench_pt_bigmesh.RISEscene` | `pathtracing_pel_rasterizer` | Mesh-heavy PT bench |
| `scenes/Tests/Bench/bench_pt.RISEscene` | `pathtracing_pel_rasterizer` | Standard PT bench |
| `scenes/Tests/MLT/cornellbox_mlt_fast.RISEscene` | `mlt_rasterizer` | Cornell box, 50 mut/pix, 256² |
| `scenes/Tests/Bench/bench_bdpt.RISEscene` | `bdpt_pel_rasterizer` | BDPT bench |
| `scenes/Tests/Bench/bench_vcm.RISEscene` | `vcm_pel_rasterizer` | VCM bench |
| `scenes/Tests/BDPT/cornellbox_bdpt_spectral.RISEscene` | `bdpt_spectral_rasterizer` | Spectral BDPT |

## Results — wall-clock per cell

K=5 trials per cell. Mean ± sample stddev, in milliseconds.

| Scene | Rasterizer | Baseline (ms) ± σ | HEAD (ms) ± σ | Δ% | σ/μ HEAD |
|---|---|---|---|---|---|
| shapes | pixelpel | 1332.0 ± 99.1 | 1444.0 ± 20.1 | **+8.41%** | 1.4% |
| bench_pt_bigmesh | pathtracing_pel (heavy mesh) | 3411.0 ± 94.3 | 3518.2 ± 83.5 | +3.14% | 2.4% |
| bench_pt | pathtracing_pel | 6186.8 ± 85.3 | 6229.2 ± 123.7 | +0.69% | 2.0% |
| cornellbox_mlt_fast | mlt | 16222.2 ± 465.0 | 15678.4 ± 468.8 | -3.35% | 3.0% |
| bench_bdpt | bdpt_pel | 17757.8 ± 965.3 | 16140.8 ± 1057.9 | **-9.11%** | 6.6% |
| bench_vcm | vcm_pel | 17801.6 ± 515.1 | 16743.8 ± 685.0 | **-5.94%** | 4.1% |
| cornellbox_bdpt_spectral | bdpt_spectral | 23056.6 ± 726.9 | 26339.4 ± 755.2 | **+14.24%** | 2.9% |

**Aggregate**: sum-of-means baseline = 85.77 s, HEAD = 86.09 s, **Δ = +0.38%**.

Significance check on the standout regression — `cornellbox_bdpt_spectral`:
- Baseline trials: 22459, 22461, 22761, 23480, 24122 (range 22459–24122)
- HEAD trials: 25488, 26070, 26132, 26475, 27532 (range 25488–27532)
- Min-HEAD (25488) > max-baseline (24122). No trial overlap. Effect is real, not noise.

## Correctness invariant

Each scene rendered once on each binary, then HEAD rendered a second time to measure
the intrinsic MC-noise floor for that scene's RNG. The L8 changes alter the timing of
when `EndTile`/`BeginTile` brackets fire and add an `IsCancelled()` virtual call per
intra-block tick, but they do NOT change the rendering math — so any RMSE that
exceeds the MC floor would indicate algorithmic regression.

| Scene | HEAD vs HEAD self (MC floor) | HEAD vs Baseline | Ratio | Verdict |
|---|---|---|---|---|
| `bench_pt` | RMSE 2.95 | RMSE 2.94 | 1.00 | within MC noise |
| `cornellbox_bdpt_spectral` | RMSE 12.99 | RMSE 12.98 | 1.00 | within MC noise |
| `shapes` | RMSE 5.83 | RMSE 5.71 | 0.98 | within MC noise |
| `cornellbox_mlt_fast` | RMSE 0.00 | RMSE 0.00 | n/a | **bit-identical** |

For every stochastic scene the cross-binary RMSE matches the same-binary RMSE to
within 2% — the differences are pure Monte-Carlo non-determinism from non-fixed RNG
seeds and parallel-tile scheduling, not algorithmic divergence. For MLT (which uses
a deterministically-seeded PSSMLT bootstrap) the output is byte-for-byte identical.

The full per-scene PNG diff table (across all 7 scenes) is in `docs/L8_INTERACTIVE_PERF_REPORT_RAW.md`.

## Where the L8 production-path overhead lives

Of the 12 L8 commits since `97fb593a`, four touched the production render path
(the others are bridge-only / Mac-GUI-only / SceneEditController-only):

1. **`d68ace25`** — `BidirectionalRasterizerBase`: reallocate `pScratchImage` on
   dim change. Correctness fix (was crashing on resize); may also avoid a
   carried-over stale-image branch on first render of certain scenes — plausible
   cause of the BDPT/VCM speedup.
2. **`7dc00d28`** — `PixelBasedRasterizerHelper::SPRasterizeSingleBlock`:
   time-based intra-block flush. Every 100 ms inside a block: EndTile + BeginTile
   on every covered FrameStore tile so the bridge poll can pick up partial pixels.
3. **`dc2639f2`** — `PixelBasedRasterizerHelper::SPRasterizeSingleBlock`:
   intra-block `IsCancelled()` query (virtual dispatch via `IProgressCallback`).
4. **`cc51e54b`** — `PixelBasedRasterizerHelper`: re-enable the split-bracket
   `DrawToggles` observer fires (was disabled at the start of the L8 round-13
   work; this is the re-enable).

The intra-block flush/cancel checks (2, 3) are gated by a `chrono::steady_clock`
elapsed-time test; in a tight per-pixel loop they add a `clock_gettime` (~20 ns on
Apple Silicon) plus a virtual call per pixel iteration when the timer hasn't
elapsed, plus an EndTile/BeginTile cycle every 100 ms when it does.

For most scenes this is amortized below noise. The spectral BDPT path appears to be
the worst case — likely because its per-pixel work hits multiple FrameStore splat
tiles via `SplatFilm`, so each intra-block flush triggers a wider set of tile-mutex
acquire/release pairs.

## Recommendations

1. **Ship the L8 work** — aggregate impact is 0.38% with correctness preserved
   across all 7 scenes and 6 rasterizer types.
2. **Don't gate the intra-block flush** — investigation in the follow-up below
   confirmed the flush is a contributor but a targeted gate hurts on aggregate.
3. **Keep the BDPT/VCM speedups** — the `pScratchImage` realloc fix is principled
   correctness work and any incidental perf gain is welcome.

## Follow-up investigation (2026-05-11): spectral-BDPT regression

After landing the L8 work I revisited the `cornellbox_bdpt_spectral` +14% number
because it's the largest single-scene regression. The investigation found three
things, all of which argue *against* a code change:

**1. The "+14%" magnitude is half measurement noise.**  Two K=5 sweeps and one
K=8 cycled sweep across two days produced spectral-BDPT regressions of +14%,
+11%, and +7.5% — same machine, same baseline / HEAD binaries, same warmup
discipline. The regression is real (Welch t = 5-6 against baseline) but its
magnitude swings ~2× from run to run, indicating significant thermal /
scheduler / allocator state contribution.

**2. The intra-block flush is a contributor but not the whole story.**
Disabling the flush via two paths — both `kFlushInterval = 1 hour` and a clean
virtual-gate override (`ShouldIntraBlockFlush()` defaulting `false` for
production, `true` for `InteractivePelRasterizer`) — recovered 30-60% of the
single-scene regression in some runs, ~0% in others. The recovery never lands
above the noise floor reliably.

**3. The clean fix hurts on aggregate.**  Running the gating fix through the
full 7-scene K=5 sweep:

| | base | HEAD | fix |
|---|---|---|---|
| Aggregate sum-of-means | 87.30 s | 86.00 s (**−1.49%**) | 88.22 s (+1.05%) |

HEAD is actually 1.5% **faster** than baseline on aggregate. The gating fix —
which removes the flush mechanism on production rasterizers — gives back the
BDPT/VCM/MLT speedups (which seem to be entangled with the L8 changes via
`-flto` cross-TU inlining decisions) while only weakly recovering the
spectral-BDPT outlier. Net: +2.5% slower than HEAD aggregate. **The fix is a
step backwards** and was reverted.

**Profile evidence**: `sample $PID 16 -mayDie` against HEAD and baseline runs
shows the top hotspots are unchanged in identity (BVH traversal, BDPT
integrator, BDPTVertex / IORStack churn through `_xzm_xzone_malloc_tiny`).
What's elevated on HEAD is `mach_absolute_time` (called from the macOS
allocator's per-free telemetry — i.e. driven by malloc volume, not by my
intra-block `chrono::now()`). The malloc volume increase has no obvious
single-line cause in the L8 diff and is likely a cross-TU LTO inlining shift in
how `BDPTIntegrator::EvaluateAllStrategiesNM` allocates per-strategy vertex
stacks. Untangling LTO is well outside the budget of a "fix one scene's
regression" task.

**Conclusion**: the L8 work is net-positive for performance. The spectral-BDPT
regression is a known outlier within the aggregate envelope and is not worth
chasing via a targeted gating fix — the cost of the fix exceeds the gain.

A future investigation could try `-fno-lto` on `BidirectionalRasterizerBase.cpp`
and `PixelBasedRasterizerHelper.cpp` to isolate inlining vs. algorithmic
contributions, but the effort-to-reward ratio is poor for a sub-2-second
absolute swing on one scene at our current measurement noise floor.

## Artifacts

- Raw CSVs: `/tmp/perfsweep_results.csv`, `/tmp/triple_results.csv`,
  `/tmp/full_fix_results.csv`
- Sweep logs: `/tmp/perfsweep.log`, `/tmp/triple_bench.log`, `/tmp/full_fix_bench.log`
- Per-scene PNG snapshots: `/tmp/diff_baseline/`, `/tmp/diff_head/`, `/tmp/diff_head2/`
- Profile samples: `/tmp/sample_head.txt`, `/tmp/sample_base.txt`, `/tmp/sample_fix.txt`
- Harnesses: `/tmp/perfsweep.sh`, `/tmp/render_for_diff.sh`, `/tmp/quad_bench.sh`,
  `/tmp/triple_bench.sh`, `/tmp/full_fix_bench.sh`
- Analyzers: `/tmp/perfanalyze.py`, `/tmp/imgdiff.py`, `/tmp/imgdiff2.py`

These live in `/tmp` and are NOT checked in — they're scratch from this measurement
run. The report itself (this file) IS checked in for reproducibility.
