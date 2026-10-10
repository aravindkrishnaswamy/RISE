# DL-532: bounded linear keyframe scratch

This is a partial allocation reduction, not a zero-allocation implementation.
The exact built-in linear interpolator on exact `Parameter<T>` inputs uses
arithmetic-only stack scratch for the right operand and retains one owned
result clone. `Point3`, `Vector3`, `Scalar`, `RISEPel` and `Vector2` use the
original virtual arithmetic operations in their original order. Unknown
parameter subclasses and nonlinear/custom interpolators retain the old path.
Targets may still retain the final parameter using `addref`; the regression
checks that the values observed at each evaluation survive later evaluations
and Timeline destruction. The separate 4,999-value comparison uses the legacy
interpolator as its arithmetic oracle.

## Red and green

`KeyframeAllocationTest` evaluates 4,999 scalar interpolations against an
independent invocation of the legacy linear expression. The unchanged bounded test linked against master ca18fc82c Timeline:
29,994 allocations, zero bit mismatches, rc1. Production scratch: 9,998
allocations, zero bit mismatches, rc0. These are deterministic counters,
n=1, SD0. `--zero-allocation-pin` remains rc1 at 9,998 allocations. The
bounded check admits two allocations per evaluation, which master exceeds.

An ASan/UBSan build instruments the changed Timeline translation unit and
allocation test, linking other library objects without sanitizer instrumentation.
It passes with the same 9,998/0 counters. This is not a full-library sanitizer
claim.

## Cross-build identity and speed

`tools/KeyframePerformanceComparison.py` compares a preserved executable
built with the master-5bd23470d library with the changed executable (current
master 30f5a25e1 adds only the DL-475 diagnostic to that library state). It uses the existing
`TimeIndexedMotionBlurTest --child` harness with one rendering thread,
`render_thread_reserve_count 0`, and alternating A/B order.

- 48/48 exact hashes match: combo and ext_combo, all six PT/BDPT/VCM RGB and
  spectral rasterizers, four distinct ValueSalt values each, 8 spp.
- 8/8 benchmark hashes match: combo_big PT, 192x192, 8 spp, eight distinct
  paired ValueSalt values (9300..9307), alternating A/B order.
- Baseline seconds: mean 0.795098625, sample SD 0.032822217, n=8.
- Scratch seconds: mean 0.664259807, sample SD 0.018977108, n=8.
- Paired speed ratio: mean 1.196684837x, sample SD 0.021824075, n=8.

This speed measurement applies to that fixture and workload on the shared
machine. It does not establish a general renderer speedup. Raw local evidence
is `.claude/logs/codex9/532-isolated-current.json`; the committed comparison script
reproduces the protocol from two supplied executables.

## Residual and review

The final owned result still allocates a parameter and mutex. Nonlinear/custom
interpolators, active-animation map lookups and transform finalization remain
outside this slice. DL-532 remains open and the strict zero-allocation pin stays
red. Full named gates and the fresh review verdict are recorded in the ledger.

## Self-review

The main risks checked were changing floating-point operation order, handing a
borrowed result to a retaining target, writing into either input parameter, and
using scratch for an unknown parameter/interpolator subclass. The exact-type
guards retain the virtual fallback; arithmetic reads the inputs and writes only
the owned result or local right operand. The independent legacy oracle, retained
results test and cross-build pixel hashes cover those claims. No new Library
file or virtual interface was introduced.

## Final gates and timing isolation

Named builds return rc0 and have no warnings. TimeIndexedMotionBlurTest96/0,
AnimationRasterizerParityTest102/0, MotionBlurTimeAverageTest77/0; allocation
and retention checks rc0, strict zero-allocation pin rc1. ASan/UBSan instrument
the final Timeline/test TUs and pass, including retained-result lifetime.

The earlier ca18fc82c comparison also matched all56 hashes and measured
1.232274906x (sample SD .032109965, n8). A later latest-master timing batch
overlapped our own linker and was discarded. The primary isolated run above
started at Unix1791660527.329823, after our last build finished1791660323.4912763.
Our renders are serialized by an advisory file lock; no own builds or renders
overlapped these eight timing pairs. Other machine users remain uncontrolled;
these are interleaved fixture measurements, not a general speed guarantee.
