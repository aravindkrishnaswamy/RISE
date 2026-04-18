---
name: performance-work-with-baselines
description: |
  Structured quantitative procedure for runtime or memory optimization.
  Use when: the user asks to speed up / optimize / reduce time / reduce
  memory / improve throughput, or any change framed as a performance
  improvement.  Forces a baseline BEFORE touching code, measurement
  discipline, single-variable changes, a correctness invariant check,
  and numeric reporting of the result.
---

# Performance Work With Baselines

## When To Use

- The user asks to make something faster, lower memory, or higher
  throughput.
- You propose an optimization and want to prove it helps.
- A change that is "mostly a refactor but I think it'll be faster" —
  the correctness invariant check is the critical part.
- Before accepting any performance claim about existing code ("X is
  slow") without a measurement backing it.

## When NOT To Use

- Correctness fixes that happen to change performance incidentally.
  The skill is for performance as the primary goal; correctness work
  uses [adversarial-code-review](adversarial-code-review.md) instead.
- Micro-micro optimizations inside a single hot function where the
  improvement is obvious from the diff and dwarfed by surrounding
  cost — measurement overhead is higher than the change.
- Exploration runs where you're just curious what the profile looks
  like — skip baselining and just profile.  Start this skill when you
  commit to making a change.

## Procedure

### 1. Establish the baseline BEFORE touching code

This is non-negotiable.  Without a baseline, "faster" is unprovable
and you cannot detect regressions from your own changes.

Pick and write down:

- **Scene(s)** — representative of the workload.  Avoid toys unless
  the optimization targets a toy case.  For RISE: pick a Cornell box
  for BDPT-style work, caustic scenes for light-transport work,
  volumetric scenes for media work, etc.
- **Command** — exact invocation, including `RISE_MEDIA_PATH`, any
  flags, the rendering trigger.  Pin it in a shell snippet.
- **Machine state** — no other heavy work running, on AC power if
  laptop, same CPU governor / QoS class.  Benchmarks also MUST set
  `render_thread_reserve_count 0` (see [AGENTS.md](../../AGENTS.md))
  so Apple Silicon's reserved E-core does not bias the measurement.
- **Sample size** — minimum 3 runs, ideally 5.  Single-run
  measurements are noise-dominated.
- **Metric** — wall time (`Total Rasterization Time` from the RISE
  log) for throughput; peak RSS for memory (`/usr/bin/time -l` on
  macOS); never eyeballed "feels faster."

Run the baseline, record mean and standard deviation.  If stddev is
more than ~10% of the mean, something is making measurements noisy —
diagnose that FIRST.  Close browsers, disable background tasks, pin
CPU governor, retry.

### 2. Profile before optimizing

Do not guess where time is spent.  The hotspot you expect is rarely
the hotspot that shows up in the profile.

Profiling options for RISE:

- Instrumented timing around suspected blocks using `Timer` from
  `src/Library/Utilities/RTime.h`.
- macOS `sample` command against a running render, or Xcode
  Instruments Time Profiler.
- Linux `perf record` / `perf report`.
- RISE's own profiling macros (`RISE_PROFILE_*`, see
  `src/Library/Utilities/Profiling.h`).

Record the top 3-5 cost centers.  Match them against your intuition.
If they disagree, trust the profile and re-plan.

### 3. Change ONE variable at a time

Multiple simultaneous optimizations make per-change attribution
impossible and hide regressions.  Sequence:

1. Propose one concrete change.
2. Apply it.
3. Re-measure (full protocol, same N runs).
4. Check the correctness invariant (step 5).
5. If the change passes both, commit / record the numbers, and
   move on to the next.

Tempting shortcut to avoid: batching three changes and measuring the
combined effect.  If the combined effect is a regression, you do not
know which change caused it.

### 4. Write the change and measure

Apply the change, rerun the baseline protocol exactly.  Do not relax
the sample size; 3 runs at minimum.  Record mean and stddev.

If stddev of the new mean or old mean is wide enough that the
difference between means is within 1σ, you do not have a significant
result.  Increase sample size or look for a different measurement
signal (e.g. a single subsystem timer instead of full wall time).

### 5. Verify the correctness invariant

Every performance change must prove it does not alter output beyond a
documented tolerance.

- **Pure refactor** (no algorithmic change): output must be
  bit-identical, or within 1 ULP, vs the baseline image.
- **Algorithmic change** (different sampling pattern, different order
  of operations in floating-point): document a tolerance — e.g.
  "RMSE ≤ 0.5% across the image, no visible caustic differences on
  scene S" — and prove it.
- **Memory-only change** (allocation strategy, layout): no
  computational effect, bit-identical is required.

Render the SAME scene before and after, pixel-diff.  A fast wrong
answer is not an optimization.

For RISE, use `compare` from ImageMagick, or write a small Python
script using PIL to compute per-channel mean abs diff, mean squared
error, and max diff.  Include the numbers in the final report.

### 6. Report with numbers

Vague reports reliably become "I think it got faster but I don't
remember how much."  Use a fixed template:

```
Change: <one-line summary>
Scene: <path>
Baseline: <mean>s ± <stddev>s over <N> runs
After:    <mean>s ± <stddev>s over <N> runs
Speedup:  <X>.<Y>x (or <P>% faster)
Correctness: bit-identical / RMSE <V> / <other>
Notes: <anything surprising>
```

### 7. Know when to stop

Budget the work before starting.  "Make the caustics test 2× faster"
is a stop condition; "make it faster" is not.  Diminishing returns
kick in fast: first change is usually the big one; subsequent
changes often contribute single-digit percentages each.

Stop when:

- You hit the target.
- The remaining top hotspot is structural (algorithm change beyond
  the budget) rather than tactical.
- The per-change improvement drops below the measurement noise
  floor.

## Anti-patterns

### "It feels faster"

Subjective speed is a known-bad perf signal.  Always measure.

### Single-run comparison

One run before + one run after = noise ≥ signal.  Minimum 3 runs each
side.

### Baseline measured on a different machine / different load

If the baseline ran on a clean laptop and the "after" runs on a busy
laptop with Slack open, you measured Slack, not your change.  Keep
the machine state matched across measurements.

### Skipping the profile

"I know what's slow, I'll just optimize this hot function" is how
people speed up a 3% line and declare a 30% win.  Profile first.

### Skipping the correctness invariant

A perf win that broke the image is a regression, not a win.  Check
every time.

### Batching changes

Two changes, one measurement → you cannot attribute.  Split them.

### No budget / stop rule

Open-ended perf work blows schedules.  Write the target down, and
stop when hit.

## Concrete Example (Hypothetical, Following The Skill)

Suppose someone proposes: "replace the per-pixel filter footprint
weights buffer with a stack-allocated array for the common Mitchell
footprint" (the kind of local optimization that shows up in
`SplatFilm::SplatFiltered`).

Following the skill:

1. Baseline: `cornellbox_bdpt_caustics.RISEscene`, 5 runs, mean
   28.9s ± 0.4s.
2. Profile: 4% of wall time in `SplatFilm::SplatFiltered`, mostly
   `std::vector<Scalar>` allocation inside the two-pass loop.
3. Change: use `Scalar weights[64]` stack buffer, heap fallback only
   for footprints > 64.
4. Measure: 5 runs, mean 28.0s ± 0.3s.
5. Correctness: bit-identical output image (confirmed via `compare
   -metric PSNR`).
6. Report:
   ```
   Change: stack-allocate SplatFiltered weight buffer for common case
   Scene: scenes/Tests/BDPT/cornellbox_bdpt_caustics.RISEscene
   Baseline: 28.9s ± 0.4s over 5 runs
   After:    28.0s ± 0.3s over 5 runs
   Speedup:  1.03x (3.1%)
   Correctness: bit-identical (PSNR ∞)
   ```

A 3% gain is marginal.  Per the stop rule, if the target was "2×", do
not count this as progress — look for a bigger hotspot.  If the
target was "every percent counts in a long-running production
render," record and keep it.

## Stop Rule

The skill's work is done when one of:

- The user-stated target is hit and reported.
- The measurement shows no significant improvement and you have the
  numbers to show why further work is not warranted.
- A correctness regression surfaces and the change is reverted (in
  which case the skill has done its protective job — the baseline
  saved the image).
