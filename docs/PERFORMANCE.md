# RISE Parallel-Efficiency Architecture

This document describes the shared rasterizer optimisations that let
BDPT, VCM, MLT, and the path tracer run efficiently on 10–40 core
systems.  See `baseline_renders/benchmarks.txt` for the numerical
results.

## TL;DR for future agents — thread priority is LOAD-BEARING

RISE used to set every render thread to `QOS_CLASS_UTILITY` on macOS
(and `nice(10)` on Linux, `THREAD_PRIORITY_BELOW_NORMAL` on Windows)
to be a "good citizen".  **This silently capped parallel efficiency
at ~25 % on Apple Silicon** because UTILITY threads get pinned to
E-cores and run on frequency-throttled P-cores.  A 10-core machine
was delivering ~2.5× throughput.

**The current policy** (see "Thread priority policy" section below
for full details) is:
- **Production default (topology-aware):** every P-core gets a render
  worker, every E-core except **one** also gets a worker.  The
  reserved E-core keeps UI / daemons responsive.  macOS workers use
  `QOS_CLASS_USER_INITIATED`; Linux/Windows use CPU-affinity pinning.
- **Benchmarks:** set `render_thread_reserve_count 0` to give every
  core a worker.  `./bench.sh` at the repo root does this
  automatically.  Without the override, Apple Silicon's E-core
  reservation makes the machine appear ~10 % slower than it really is.
- **Legacy background mode:** `force_all_threads_low_priority true`
  is an explicit opt-in that applies `QOS_CLASS_UTILITY` (macOS),
  `nice(10)` (Linux), or `THREAD_PRIORITY_BELOW_NORMAL` (Windows) to
  every render thread AND makes `ParallelFor` skip caller
  participation so one thread doesn't slip back to normal priority.
  2–4× slower but coexists cleanly with heavy user foreground work.

**Never "helpfully" put all threads back at lower priority without
understanding this trade-off.**  If you do, benchmarks will silently
regress 2–4× and every future optimisation will appear not to help.

## Summary

On a 10-core M1 Max:

| Scene | Original baseline | After sprint | Parallelism |
|---|---|---|---|
| `bench_bdpt` (256² Cornell) | 46 s | **14 s** | 8.4× / 10c |
| `bench_vcm` (256² + glass sphere) | 30 s | **14 s** | 8.1× / 10c |
| `bench_pt` (256² Cornell + blocks) | 9 s | **5 s** | 7.2× / 10c |
| `pt_torus_chain_atrium` (768² complex) | 134 s | **38 s** | 8.1× / 10c |
| `bdpt_alchemists_sanctum` (768×512, 14-depth) | 818 s | **~240 s** | ~7× / 10c |
| `mlt_caustic_chain` (512² with caustics) | 460 s | **~140 s** | ~7× / 10c |

The dominant single change was the thread-priority fix (2–4× on most
scenes).  The earlier architectural work (thread pool, adaptive tiles,
thread-local splat buffer, parallel KD-tree, pooled scratch) compounds
on top.

## Thread priority policy (most important section)

### Why this matters

On Apple Silicon, ARM big.LITTLE phones, and Intel Alder Lake+ CPUs,
cores come in two flavours:
- **Performance (P) cores**: high clock, wide OoO, large cache.
- **Efficiency (E) cores**: low clock, simple pipeline, small cache.
  Typical throughput ~1/3 of a P-core.

The previous RISE implementation set every thread to
`QOS_CLASS_UTILITY` on macOS "to be a good citizen." UTILITY pins
threads to E-cores and throttles any P-cores they land on.  A 10-core
M1 Max delivered only ~2.5× aggregate throughput instead of 7–9×.

### The current policy (topology-aware)

Implemented in `src/Library/Utilities/CPUTopology.{h,cpp}` and
applied by `GlobalThreadPool()`:

**Every P-core gets a render worker.  Every E-core except
`render_thread_reserve_count` also gets a worker.  1 E-core is left
to the OS by default so UI / daemons / system calls stay responsive.**

- On M1 Max (8P + 2E): **9 render workers**, 1 E-core for system.
- On M1 Pro (6P + 2E): **7 workers**, 1 E-core free.
- On Intel i9 13900K (8P + 16E): **23 workers**, 1 E-core free.
- On homogeneous systems (no E-cores detected): reserve
  `render_thread_reserve_count` (default 1) cores from the total.

Render workers:
- **macOS**: set `QOS_CLASS_USER_INITIATED` on each worker — tells
  the scheduler to prefer P-cores and NOT throttle their clock.  This
  is the class below USER_INTERACTIVE and above DEFAULT.  It is NOT
  UTILITY (the trap) and NOT USER_INTERACTIVE (that preempts UI).
- **Linux**: `sched_setaffinity` pins each worker to the union of
  P-cores and (E-cores − reserved).  Kernel scheduler handles
  placement within the mask.
- **Windows**: `SetThreadAffinityMask` with the same mask.  The
  reserved E-cores are visible to Windows' own scheduler for system
  work.

### Benchmark mode

For accurate parallel-efficiency measurement, set
`render_thread_reserve_count 0` to give every core a worker.  The
`./bench.sh` harness at the repo root does this automatically.

### Legacy "render in the background" mode

`force_all_threads_low_priority true` still exists and applies the
old UTILITY class to every worker.  Useful only when the user wants
to render while heavily using the machine for other work and accepts
the 2–4× throughput cost.

### Configuration

| Option | Default | Meaning |
|---|---|---|
| `render_thread_reserve_count` | `1` | E-cores to leave for the OS |
| `force_all_threads_low_priority` | `false` | Legacy UTILITY for every worker |
| `force_number_of_threads` | `0` | Override worker count explicitly |
| `maximum_thread_count` | unlimited | Clamp topology-derived worker count |

All in `RISE_OPTIONS_FILE` (`key value`, one per line).

### Platform topology detection

| Platform | API used |
|---|---|
| macOS | `sysctlbyname("hw.perflevel0.logicalcpu"/"hw.perflevel1.logicalcpu")` |
| Linux | `/sys/devices/system/cpu/cpu*/cpu_capacity` (highest value = P) |
| Windows | `GetLogicalProcessorInformationEx(RelationProcessorCore)` → `EfficiencyClass` |

If detection fails (older kernel, Intel Mac before perflevel sysctls,
etc.) all cores are treated as P and the reserve counts against the
total.

## Core components

### [ThreadPool](../src/Library/Utilities/ThreadPool.h)

Persistent worker pool created once via a Meyers' singleton
(`GlobalThreadPool()`).  Workers park on a `std::condition_variable`
between tasks.  All rasterizers now submit work via
`pool.ParallelFor(N, body)` instead of calling `pthread_create` per
progressive pass.  For VCM at 64 iterations this saves ~30 ms of
thread-lifecycle overhead per render and eliminates a scaling wall
that would have bitten at 20+ cores.

The pool constructor takes two arguments — total worker count and
a CPU-affinity mask (list of CPU IDs workers should pin to on Linux
/ Windows; empty on macOS which uses QoS class instead).
`GlobalThreadPool()` derives both from `ComputeRenderPoolSize()` and
`GetRenderAffinityMask()`, which respect the options documented above.

`ParallelFor(n, body)` submits `n` tasks and blocks the caller until
all complete.  In normal mode the caller participates by draining
queued tasks while waiting, so an `n == 1` dispatch never deadlocks
and recursive ParallelFor calls from pool workers are safe.

In the legacy "render in the background" mode
(`force_all_threads_low_priority true`) the caller does NOT
participate — letting it would leave one render thread at normal
priority, silently defeating the user's opt-in.  **Trade-off**:
recursive `ParallelFor` from within a pool worker becomes unsafe on
saturated / small pools in this mode (the waiting worker no longer
steals work).  Current render call sites don't recurse, so this is a
contract restriction documented in `ThreadPool.h`, not a live bug.

The generic pixel rasterizer, BDPT, and MLT single-thread fallbacks
(reached when `force_number_of_threads 1` or `maximum_thread_count 1`
shrinks the pool, or when animation exposure disables MP dispatch)
run the render loop directly on the caller thread instead of going
through the pool.  When legacy low-priority mode is active, those
branches lower the caller's own priority via
`riseSetThreadLowPriority()` before starting the render loop, so the
"every render thread at reduced priority" contract holds there too.
The caller thread stays low-priority for the remainder of the
process (QoS class can only be lowered on macOS) — acceptable
because legacy mode is an explicit opt-in for the whole render
lifetime.

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

### MLT work-stealing chain dispatch

[MLTRasterizer.cpp](../src/Library/Rendering/MLTRasterizer.cpp) used
to statically partition chains across threads: chain `c` went to
thread `c / chainsPerThread`.  MLT chains have highly variable cost
(one chain may explore deep caustic paths with many rejections,
another may converge quickly), so the static partition meant fast
threads idled waiting for the slowest.

The current design hands out chains via `std::atomic<unsigned int>
fetch_add(1)`.  Workers scavenge extra chains whenever they finish
their current one.  The spectral variant follows the same pattern.

### Per-thread scratch vectors

`VCMIntegrator`'s `candidates` vector, `VCMPelRasterizer`'s per-pixel
subpath vectors, and `BDPTPelRasterizer`'s light/eye subpath vectors
are now `static thread_local std::vector<>` reused across pixels and
samples.  `clear()` + on-demand `reserve()` keep memory bounded while
eliminating per-sample libmalloc arena contention.

This was the single biggest VCM win in the sprint (−14 % wall).

## Investigated-and-rejected optimisations (keep notes for future agents)

### ProgressiveFilm `alignas(64)`
Padding `ProgressivePixel` from 56 B to 64 B to eliminate false
sharing between adjacent pixels was investigated and reverted.  On
the 256² benchmark VCM wall went 27 s → 47 s and PT 9 s → 14 s — the
L2 working-set overshoot cost more than the false-sharing fixed.
Morton-ordered tile dispatch naturally keeps adjacent-pixel writes on
the same thread, so false sharing was rarer than the static analysis
suggested.

### ProgressiveFilm struct-shrink (packing converged into wN high bit)
Tried compacting `ProgressivePixel` from 72 B to exactly 64 B (one
pixel per cache line, no padding) by moving the `bool converged`
field into the high bit of `wN`.  A/B benchmark showed `bench_bdpt`
regressed ~5 % (14.82 s → 15.52 s), `bench_vcm` flat, `bench_pt` +3 %.
The ~10 % memory savings didn't translate to wall-time gains because
the struct was already L2-resident for these scenes and Morton tile
ordering had eliminated most false sharing already.  Reverted.

### Splat-film lock sharding / column strips
Back-of-envelope: with `ThreadLocalSplatBuffer` already amortising
locks to one-per-row-per-tile, the total lock overhead for
`bench_bdpt` is ~1–10 ms over a 14 s render (< 1 %).  Sharding
would save a sub-1 % slice.  Not implemented — the
`ThreadLocalSplatBuffer` + `BatchCommit` pair is already the right
abstraction.

### VCM async light pass (double-buffered store) — deferred to 20+ cores
Measured on 10-core M1 Max: light pass = 36 ms/iter, eye pass = 180
ms/iter.  Light is 17 % of VCM time so the theoretical ceiling for
full async overlap is ~17 %.  On 10 cores the eye pass already
saturates ~8 worker-cores; taking even 1 core for background light
work slows the eye pass faster than the light pass is hidden (checked
math: eye-9c + light-1c = 324 ms > 216 ms serial).  On a 20-core
machine where the eye pass only uses ~15 cores, the 5 spare cores
absorb background light work and this WOULD pay off.  Implementation
deferred until 20-core validation is available.

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
