# RISE Parallel-Efficiency Architecture

This document describes the shared rasterizer optimisations that let
BDPT, VCM, and the path tracer run efficiently on 10–40 core systems.
See `baseline_renders/benchmarks.txt` for the numerical results.

## Summary

On a 10-core M1 Max running a canonical 256² Cornell BDPT benchmark,
end-to-end wall time dropped from 46 s to 27 s (−41 %) and average
parallelism from 2.9× to 4.6×.  The same infrastructure is designed
to scale to 20 and 40 core systems without further code changes.

## Core components

### [ThreadPool](../src/Library/Utilities/ThreadPool.h)

Persistent worker pool created once via a Meyers' singleton
(`GlobalThreadPool()`).  Workers park on a `std::condition_variable`
between tasks.  All rasterizers now submit work via
`pool.ParallelFor(N, body)` instead of calling `pthread_create` per
progressive pass.  For VCM at 64 iterations this saves ~30 ms of
thread-lifecycle overhead per render and eliminates a scaling wall
that would have bitten at 20+ cores.

### [AdaptiveTileSizer](../src/Library/Rendering/AdaptiveTileSizer.h)

Replaces hard-coded 32×32 tiles with `ComputeTileSize(w, h, threads,
target=8, min=8, max=64)`.  Keeps `tilesPerThread ≥ 8` so
work-stealing always has slack.  Rounds to a multiple of 8 for SIMD
friendliness.

At 10 threads on 256² this gives 24×24 tiles ≈ 12 tiles/thread;
previously 32×32 gave only 6.4.  At 20 threads on the same image it
picks 16×16 (16 tiles/thread).  At 40 threads it clamps to the
8-pixel minimum.

### [PreviewScheduler](../src/Library/Rendering/PreviewScheduler.h)

Decouples progressive-preview cadence from iteration cadence.  Default
target is 7.5 s wall-time between previews.  Without this, a VCM
render at K=1 on a small scene ran `progFilm.Resolve()` + file I/O
25×/sec, serialising all workers on the main-thread barrier.

Guarantees:
- First call always returns true (users see something immediately).
- The final pass always forces a preview.
- Convergence check runs with preview so early-exit latency is at
  most `targetInterval`.

### [ThreadLocalSplatBuffer](../src/Library/Rendering/ThreadLocalSplatBuffer.h)

Per-worker sparse splat accumulator.  Replaces the per-splat
`rowMutex->lock()` with per-row batched commits.  Workflow:

1. Worker calls `SplatFilm::Splat(x, y, c)` as before.
2. Splat internally lazy-binds a `thread_local` buffer to the film
   and appends a record with zero synchronisation.
3. Auto-flushes into the film at 65 536 records (≈3 MB).
4. Worker flushes on exit (`FlushCallingThreadSplatBuffer` hook at
   the end of `DoWork`).

Contention drops from `O(splats)` mutex cycles to `O(unique rows per
tile)`.  On VCM where the light-subpath splat rate can hit 10/sample,
this is the difference between serial saturation and full parallel
throughput.

### [Parallel KD-tree build](../src/Library/Shaders/VCMLightVertexStore.h)

`LightVertexStore::BuildKDTreeParallel()` recursively splits subtree
construction across the global thread pool.  Below a cutoff
(`max(4096, N / (8 × numWorkers))`) it drops to the existing serial
`BalanceSegment`.  Produces a query-equivalent tree (not
byte-identical in non-median slots, but identical results because
`LocateAllInRadiusSq` only reads vertices via the tree traversal).

Relieves a ~100–200 ms / iteration serial bottleneck on 2 M-vertex
stores that otherwise capped VCM's parallel scaling at ~5× regardless
of core count (Amdahl's Law).

### Per-thread scratch vectors

`VCMIntegrator`'s `candidates` vector, `VCMPelRasterizer`'s per-pixel
subpath vectors, and `BDPTPelRasterizer`'s light/eye subpath vectors
are now `static thread_local std::vector<>` reused across pixels and
samples.  `clear()` + on-demand `reserve()` keep memory bounded while
eliminating per-sample libmalloc arena contention.

This was the single biggest VCM win in the sprint (−14 % wall).

## Rejected optimisation: ProgressiveFilm `alignas(64)`

Padding `ProgressivePixel` from 56 B to 64 B to eliminate false
sharing between adjacent pixels was investigated and reverted.  On
the 256² benchmark VCM wall went 27 s → 47 s and PT 9 s → 14 s — the
L2 working-set overshoot cost more than the false-sharing fixed.

Morton-ordered tile dispatch naturally keeps adjacent-pixel writes on
the same thread, so false sharing was rarer than the static analysis
suggested.

## Measuring parallel efficiency

On macOS, `/usr/bin/time -l` reports user/sys CPU only for the main
thread — worthless for multithreaded code.  Use zsh's `time` builtin
instead:

```sh
TIMEFMT='%*E %*U %*S'
time (printf "render\nquit\n" | ./bin/rise scene.RISEscene > /dev/null 2>&1)
#                        ↑ wall      ↑ user     ↑ sys (all summed across threads)
```

Parallel efficiency = `(user + sys) / (wall × threads)`.  A value of
1.0 would be perfect; 0.5 is "running on 5 cores".  Pre-sprint
baselines were 0.2–0.4; targets for 20 cores are ≥ 0.7.

Canonical benchmark scenes live in `scenes/Tests/Bench/`.

## Tuning guide

- **Preview too slow / too frequent?** `PreviewScheduler` target is
  currently hardcoded at 7.5 s in `PixelBasedRasterizerHelper.cpp`
  and `BDPTRasterizerBase.cpp`.  Expose as a scene-file option if
  needed.
- **Memory too high?**  `ProgressiveFilm` memory scales with
  resolution × 56 B/pixel.  Largest store is `LightVertexStore` at
  VCM's 1080p × 10 light-depth ≈ 1.6 GB.  Cap via
  `maxStoredLightVertices`.
- **Want smaller/larger tiles?**  Override `ComputeTileSize`'s
  `targetTilesPerThread` (default 8) if your scene's per-pixel cost
  is unusually uneven — denser scenes with complex materials may
  benefit from more tile granularity.
