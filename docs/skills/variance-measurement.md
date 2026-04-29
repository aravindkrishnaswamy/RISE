---
name: variance-measurement
description: |
  Structured procedure for proving (or disproving) that a rendering-side
  change actually reduces variance / RMSE — not just produces a
  different image.  Use when:  someone asks "does this fix help?", you
  just changed an integrator or sampler and want to quantify impact,
  reviewing a PR that claims convergence improvement, or writing a perf
  comparison for an OpenPGL / MIS / RR / sampling change.  Walks through
  K-trial measurement on EXR outputs, the HDRVarianceTest tool, master
  vs fix protocol with stash-rebuild-pop, and reading both pure variance
  and RMSE-vs-reference.  Avoids the common pitfalls (PNG quantization,
  HDRReader RLE bug, deterministic seeds masking variance, K too small).
---

# Variance Measurement

Quantify whether a rendering change actually reduces noise — separate
from anecdotal "looks cleaner" or eyeballing a single image.  The goal
is a single ratio: σ²_fix / σ²_master, with confidence that any
difference is real and not trial-to-trial noise.

## When To Use

- You changed an integrator, sampler, MIS weight, RR rule, or
  guiding-field plumbing and want to know if it actually helps.
- A PR claims "converges faster" without numeric backing.
- You're choosing between two approaches to the same problem (RIS vs
  one-sample MIS, learned MIS weights vs fixed alpha, etc.).
- You want a ground-truth-relative metric (RMSE vs high-SPP ref) — not
  just inter-run variance.

## When NOT To Use

- For binary correctness (pass/fail) — use a regression test, not
  variance.  Variance can pass an incorrect implementation.
- For visual checks (ringing, banding, hue shift) — those are
  structural artifacts, not variance.  Look at the diff image.
- When K=1 or K=2 are all you can afford — you don't have enough
  samples to make a claim either way.  Use ≥4, ideally ≥16.

## The protocol

### 1. Pick a scene with the right characteristics

For the change you're testing, the scene must (a) actually exercise
the code path you changed, and (b) have enough indirect / hard-to-
sample transport that variance differences will show up.  Curated
benchmarks:

- `scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene` — slot-
  window single-light Cornell vault, dominant indirect, Lambertian
  walls.  Best for guiding / NEE-direct measurements.
- `scenes/FeatureBased/BDPT/bdpt_jewel_vault.RISEscene` — same scene,
  BDPT pipeline.
- `scenes/FeatureBased/BDPT/pt_torus_chain_atrium.RISEscene` and the
  bdpt variant — refractive-heavy (glass torus chain), where
  parallax-aware VMM `eta` matters.
- `scenes/FeatureBased/BDPT/bdpt_cloister.RISEscene` — outdoor
  diffuse-glossy, more uniform lighting; lower variance baseline.

### 2. Build a low-SPP no-denoise EXR-output test scene

Strip three things from the upstream scene:

```awk
# In the rasterizer block:
samples 32           # PT: 32  / BDPT: 16   (low enough to fit K trials)
oidn_denoise FALSE   # explicit — denoise smooths over real variance
adaptive_max_samples 0  # disable adaptive — variance must be pure SPP

# In the file_rasterizeroutput block:
type EXR             # NOT HDR — see "Pitfalls" #2
```

Drop any `file_rasterizeroutput { type PNG ... }` blocks — PNG output
adds I/O cost and isn't used.  Strip CRLF first (`tr -d '\r'`) — RISE
scenes use Windows line endings on Windows, which trips up sed/awk on
patterns like `\tsamples\b`.

### 3. Render K trials per condition with the right binary

K=16 is the floor for honest stats, K=4 only for sanity.  The variance
source in RISE comes from non-deterministic OpenPGL training
(`pglFieldArgumentsSetDefaults(..., false, ...)` is what gets passed
in [PathGuidingField.cpp:66-72](../../src/Library/Utilities/PathGuidingField.cpp:66))
plus multithreaded sample collection.  Two consecutive runs with the
same scene file WILL differ — you don't need a seed knob.

```bash
# K=16 fix trials (fix CLI is currently built)
export RISE_MEDIA_PATH="$(pwd)/"
mkdir -p var_test/fix
for i in $(seq 0 15); do
  sed "s|var_test/fix/run0|var_test/fix/run${i}|g" /tmp/scene.RISEscene \
    > /tmp/scene_${i}.RISEscene
  printf "render\nquit\n" | ./bin/RISE-CLI.exe /tmp/scene_${i}.RISEscene \
    2>&1 | grep "Total Rasterization"
done
```

Then `git stash` the fix changes, rebuild Library + CLI:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild build/VS2022/Library/Library.vcxproj /p:Configuration=Release `
    /p:Platform=x64 /p:SolutionDir="C:\Dev\GitHub\RISE\build\VS2022\\" `
    /m /nologo /v:minimal
& $msbuild build/VS2022/RISE-CLI/RISE-CLI.vcxproj /p:Configuration=Release `
    /p:Platform=x64 /p:SolutionDir="C:\Dev\GitHub\RISE\build\VS2022\\" `
    /m /nologo /v:minimal /t:Rebuild
```

The `/p:SolutionDir=...` is mandatory when building the .vcxproj
directly (without the .sln) — without it, library search paths resolve
wrong and you get `LNK1104: cannot open file 'RISE.lib'`.

Render K=16 master trials with `var_test/master/runN.exr`, then
`git stash pop` and rebuild fix again.

### 4. Render a high-SPP reference

For RMSE-vs-truth measurements, render one reference at ≥4× the trial
SPP, with the same scene topology and same algorithm.  256 SPP for PT
trials at 32, 128 SPP for BDPT trials at 16.  Either binary works for
the reference — both converge to the same physical truth.  EXR output.

### 5. Compute variance with HDRVarianceTest

Two modes — run both for the full picture:

```bash
# Mode 1: pure inter-run variance (no ground truth needed)
./bin/tools/HDRVarianceTest.exe var_test/master/run{0..15}.exr
./bin/tools/HDRVarianceTest.exe var_test/fix/run{0..15}.exr

# Mode 2: RMSE vs reference (bias + variance)
./bin/tools/HDRVarianceTest.exe --ref var_test/ref/ref.exr \
    var_test/master/run{0..15}.exr
./bin/tools/HDRVarianceTest.exe --ref var_test/ref/ref.exr \
    var_test/fix/run{0..15}.exr
```

Source is at [tools/HDRVarianceTest.cpp](../../tools/HDRVarianceTest.cpp);
project at [build/VS2022/Tools/HDRVarianceTest.vcxproj](../../build/VS2022/Tools/HDRVarianceTest.vcxproj).
Build with the same `/p:SolutionDir=...` recipe.

### 6. Read the metrics

Mode 1 reports:

- **mean per-pixel σ²** — total noise.  Sensitive to fireflies.  Lower
  is better.
- **median per-pixel σ²** — robust noise on the typical pixel.  Tells
  you how the bulk of the image converges.
- **95th / 99th percentile per-pixel σ²** — tail behaviour.  Diverges
  from the mean if the fix is causing fireflies in a few pixels.
- **relative noise σ/μ** — normalized for image brightness, comparable
  across scenes.

Mode 2 reports:

- **mean RMSE across runs** — primary "convergence to truth" metric.
- **min / max per-run RMSE** — confidence range.

A claim of "fix improves variance" needs:

- Mean σ² lower for fix, AND
- Median σ² lower for fix (rules out "fix just suppressed a few
  fireflies"), AND
- Mean RMSE-vs-ref lower for fix (rules out "fix tightened consistency
  but at a different mean — bias").

Conversely, **fix variance > master variance is also a real signal**:
something in the fix is over-amplifying samples (likely an RR
compensation that's recovering with too small a survival probability,
or a guide-PDF in the denominator that's too small).  Fireflies in P99
are the most common signature.

### 7. Compute ROI vs unguided BDPT at fixed wall time

Variance ratios between two PGL configurations are useful but
incomplete.  The harder question is whether *any* PGL is better than
no PGL at fixed wall budget — because path guiding always pays a
training overhead, and that overhead has to come back through reduced
sampling variance to be worthwhile.

The protocol:

1. Render `pathguiding FALSE` at the same scene at K=16, get a
   baseline `(σ²₀, W₀)` pair.
2. For each PGL configuration with measured `(σ²_PGL, W_PGL)`,
   project unguided variance at the PGL config's wall time:
   `σ²_unguided(W_PGL) = σ²₀ × W₀ / W_PGL` (1/N scaling, where N is
   linear in wall time).
3. **ROI = σ²_unguided(W_PGL) / σ²_PGL.**  ROI > 1 means PGL beats
   unguided at the same wall budget; ROI < 1 means unguided wins.

Compute ROI separately for **mean σ²**, **P99 σ²**, and **max σ²** —
they often disagree.  Path guiding is fundamentally a tail-variance
tool: it reduces fireflies on hard-to-sample paths.  A scene where
PGL has mean ROI < 1 but P99 ROI > 1 is the *expected* shape, not a
failure.  A configuration that fixes mean σ² at the cost of P99 (or
vice versa) is *also* a real result, just not the one you wanted.

**Do not compare PGL configurations only against other PGL
configurations.**  An A-vs-B win between two guided modes can still
be a B-vs-unguided loss.  Both A and B might be losing the wall-time
race against `pathguiding FALSE` — the meaningful number is whether
*any* mode wins against off.

#### Historical: when RMSE-vs-reference was unreliable (bug, now fixed)

Multiple "reference" renders that *should* converge to the same
truth disagreed with each other at K=1: 128 SPP (offline-PGL), 128
SPP (online-PGL), and 1024 SPP (unguided) on `bdpt_jewel_vault` had
pairwise RMSE 6–14 and channel means differing by ~5%, with the
1024-SPP reference always darkest.  Root cause: the progressive
multi-pass code path fired a per-pixel convergence check
(`stdError/mean < 0.01`) even when adaptive sampling was disabled.
On heavy-tailed (firefly-prone) sample distributions, the check is
more likely to fire on a "lucky-low" 32-sample realization than on
a high one, freezing those pixels mid-render while pixels that hit
a firefly continue to refine.  Bias scales with pass count: ~1% per
pass, ~5.4% at 32 passes (1024 SPP / 32 spp-per-pass).

Fixed in commit `<tbd>` by gating the Welford accumulator,
convergence check, and `AddAdaptiveSamples` on the `adaptive` flag
alone (not `adaptive || pProgFilm`) in `BDPTPelRasterizer`,
`PathTracingPelRasterizer`, `PathTracingSpectralRasterizer`,
`PixelBasedPelRasterizer`, and `VCMPelRasterizer`.  After the fix,
default-progressive 1024-SPP unguided BDPT on `bdpt_jewel_vault`
agrees with 1-pass progressive at any SPP, and RMSE-vs-reference is
a reliable metric again.

**If you still see references disagree by more than per-trial σ:**
verify the build is post-fix, render a 1-pass progressive control
(`progressive_samples_per_pass = total samples`), and compare.
If the 1-pass control still differs from default-progressive,
re-open the convergence-bias investigation.

## Pitfalls and how to avoid them

1. **PNG outputs are 8-bit sRGB.** ImageDiffTest works on PNG and is
   useful for quick sanity, but the 1/255 quantization step in linear
   space (~1.5% at low values, much worse at very low values) hides
   small variance differences.  Always use EXR for the actual
   measurement.

2. **RISE's HDRReader has a bug in old-format RLE.** Lines 200–203 of
   [HDRReader.cpp](../../src/Library/RasterImages/HDRReader.cpp:200)
   write from the stale `buf` instead of the just-read `col`.  RISE
   scene output uses old-format RLE for HDR, so RISE can't reliably
   read its own .hdr files and pixel exponents come out garbage (1e-24
   instead of 1e-2).  Use `type EXR` not `type HDR` for variance work.
   This is a real RISE bug — fix it separately if you have cycles.

3. **`/p:SolutionDir=` is mandatory for direct .vcxproj builds.**
   Without it, `$(SolutionDir)..\..\bin\` resolves to
   `build/VS2022/<projdir>/../../bin/` instead of
   `build/VS2022/../../bin/`, the RISE.lib search misses, and you get
   `LNK1104`.  Pass
   `/p:SolutionDir="C:\Dev\GitHub\RISE\build\VS2022\\"` (note the
   double trailing backslash for proper escaping).

4. **Windows Defender real-time scan briefly locks freshly-written
   .lib files.**  If `LNK1104: cannot open file 'RISE.lib'` happens
   immediately after a successful Library build, give it 10s and
   retry.  Or run the Library build alone first, then the CLI build
   in a separate invocation — they don't need to be parallel.

5. **The `bin/tools/` exe needs DLLs alongside it.**  Windows DLL
   search starts in the .exe directory.  When a tool is built fresh,
   you may need to copy `OpenImageDenoise*.dll`, `libpng16.dll`,
   `OpenEXR-3_4.dll`, etc. from `bin/` to `bin/tools/`.  Or `cd
   bin/tools/` before invoking, with relative paths in the args.

6. **K=4 is statistical noise.**  Differences smaller than ~5% are not
   distinguishable at K=4.  Use K=16 (28 pairs) before claiming small
   wins.  If the K=4 difference is >20%, K=16 will likely confirm it
   without surprises.

7. **Don't disable OpenPGL non-determinism.**  The non-deterministic
   training is your variance source.  Setting deterministic mode
   collapses K runs to identical images and you measure nothing.

8. **OIDN denoise will mask variance.**  Always set `oidn_denoise
   FALSE` in the test scene.  Same for `adaptive_max_samples 0`
   (uniform SPP only) and `adaptive_threshold #disabled`.

9. **If guided wall time ≈ unguided wall time, guiding probably
   isn't running at all.**  Sanity-check before believing any
   variance ratio.  Training (`pathguiding_iterations × pathguiding_spp`
   passes plus the configured render SPP) should make guided
   substantially slower than unguided — typically 30%–500%
   depending on integrator.  When the times match within ~5%, look
   for an `#ifdef RISE_ENABLE_OPENPGL` that's silently disabled —
   the VS2022 Library.vcxproj historically did not include the
   define, so the entire OpenPGL integration was dead code on
   Windows even though the parser accepted `pathguiding TRUE`.
   The fix is to add `RISE_ENABLE_OPENPGL` to the
   `<PreprocessorDefinitions>` of `build/VS2022/Library/Library.vcxproj`
   (Release + Debug), add `C:\Dev\openpgl-install\include` to
   `<AdditionalIncludeDirectories>`, and add `openpgl.lib` +
   `C:\Dev\openpgl-install\lib\` to RISE-CLI's link line.  Verify
   with: `bin/RISE_Log.txt` should contain a `PathGuidingField::
   Initialized` line after a guided render.

10. **Build the OpenPGL dependency once via its CMake superbuild**
    (`cd /c/Dev/openpgl-0.7.1 && mkdir build && cd build && cmake -G
    "Visual Studio 18 2026" -A x64 -DCMAKE_INSTALL_PREFIX=C:/Dev/openpgl-install
    -DBUILD_OIDN=OFF -DBUILD_TOOLS=OFF ../superbuild && cmake --build .
    --config Release`).  The superbuild auto-fetches TBB.  Copy
    `C:/Dev/openpgl-install/bin/openpgl.dll` and `tbb12.dll` into
    `bin/` so the built RISE-CLI can find them at runtime.

11. **PGL training non-determinism dominates at low final-SPP — always
    measure against an unguided baseline before claiming guided X is
    better than guided Y.**  RISE's default config is 6 training
    iterations × 12 SPP each (= 72 SPP of pure training) + the
    configured final SPP.  At 16 final SPP on `bdpt_jewel_vault`,
    guided mean σ² is ~9.3 and unguided is ~3.3 — *PGL nearly triples
    the variance.*  Per-trial training non-determinism
    (`pglFieldArgumentsSetDefaults(..., false, ...)` in
    [PathGuidingField.cpp:66](../../src/Library/Utilities/PathGuidingField.cpp:66)
    means each K=16 run trains a different field) overwhelms whatever
    sampling-quality benefit the guide provides.  Bias is not the
    issue — channel means agree to ~0.6%.  This isn't a bug; it's
    "guiding doesn't pay off until final SPP × is large enough to
    amortize training jitter."  Before drawing conclusions about a
    PGL-internal change, render a `pathguiding FALSE` baseline at the
    same SPP and confirm guided actually beats unguided.  If it
    doesn't, you're measuring training jitter, not your fix.

## Worked example: OpenPGL fix evaluation — and a cautionary tale

Background: an OpenPGL integration fix changed (a) the
`rrAffectsDirectContribution` flag from false to true on both PT and
BDPT, eye and light paths, (b) the recorded
`russianRouletteSurvivalProbability`, and (c) the recorded `eta` and
`roughness` per path segment.  Question: does it measurably help?

**The original measurement got two things wrong** and is preserved here
because the failure mode is instructive.  Skip to the "what really
happened" section if you just want the punchline.

### What we measured first (invalid)

K=16 EXR variance on `bdpt_jewel_vault` (16 SPP, RIS, max-depth 8),
Windows VS2022 build:

| Metric                | "Master"  | "Fix"     | Δ        |
|-----------------------|-----------|-----------|----------|
| mean σ²               | 3.022     | 3.584     | +18.6%   |
| median σ²             | 9.68e-6   | 9.30e-6   | -3.9%    |
| 99th percentile σ²    | 0.090     | 0.149     | **+66%** |
| RMSE vs 128-SPP ref   | 4.592     | 4.562     | -0.67%   |

This led to a wrong-but-plausible story: the fix's BDPT light-path
`rrAffectsDirectContribution=true` was dividing the bare-emission Le
recorded at reversed-walk vertex 1 by an unrelated `p_rr`, blowing up
fireflies in the P99.  The story even matched the asymmetry between
median (slightly better) and P99 (much worse) — exactly what
RR-amplification fireflies look like.

### What really happened

The Windows `build/VS2022/Library/Library.vcxproj` was missing
`RISE_ENABLE_OPENPGL` from `<PreprocessorDefinitions>` (fixed in
commit 09be84b).  Every `#ifdef RISE_ENABLE_OPENPGL` block — which is
the entire path-guiding subsystem including both `AddPathSegments`
call sites — was compiled out.  All three "conditions" (master,
full fix, partial fix) were running pure unguided BDPT.  The +66% was
trial-to-trial threading variance for K=16 unguided BDPT on this
scene, not a fingerprint of any code we changed.

The wall-time tell was right there: ~12 s per render with
`pathguiding TRUE` in the scene file.  After fixing the build define,
the same scene takes ~62 s (6 training iterations + final pass).  A
5× slowdown when guiding turns on is not subtle — but if you don't
have a comparison number in front of you, it's easy to miss.  **See
pitfall #9 above; verify guided wall time looks slow before believing
any variance ratio.**

### What the proper measurement showed (PGL actually firing)

K=16 EXR variance on `bdpt_jewel_vault` (16 SPP, RIS, max-depth 8),
Windows build with `RISE_ENABLE_OPENPGL` set:

| Metric                | Master  | Full fix | Partial fix | Unguided |
|-----------------------|---------|----------|-------------|----------|
| mean σ²               | 9.320   | 9.950    | 9.461       | 3.316    |
| median σ²             | 9.03e-5 | 9.10e-5  | 9.05e-5     | 1.27e-5  |
| 99th percentile σ²    | 0.2575  | 0.2385   | 0.2543      | 0.1371   |
| max σ²                | 28140   | 36059    | 34928       | 21317    |
| RMSE vs 128-SPP ref   | 5.220   | 5.240    | 5.145       | 4.695    |

Two things to notice:

1. **The +66% P99 spike disappeared** — Full vs Master is now −7.4%
   on P99, partial is +1.4% on Master.  The hypothesis the original
   measurement seemed to support (BDPT light-path RR amplification)
   was a phantom.  The theoretical reasoning still favours
   `rrAffectsDirectContribution=false` on the BDPT light-path
   (vertex-1 Le has no RR history to compensate for), but it stays
   that way on principle, not measured P99 behaviour.
2. **PGL is currently *worse* than unguided on this scene.**  Guided
   mean σ² ~9.4 vs unguided 3.3.  RMSE-vs-ref ~5.2 vs 4.7.  This is
   training non-determinism dominating the variance budget at low
   final-SPP — see pitfall #11.

The interesting comparison stopped being master-vs-fix and became
guided-vs-unguided.  When you spend an hour rebuilding and re-measuring
to debunk one hypothesis, the bigger picture is often the *next*
hypothesis hiding in the data.

## Worked example: tier ladder for fixing low-SPP PGL

Continuing on `bdpt_jewel_vault` after the master/fix sweep above
showed PGL *losing* to unguided BDPT at low final SPP, four
incremental architectural changes were prototyped to see which
(if any) could close the gap.  The tiers map to user-controllable
config knobs added during this work:

- **Tier 0** — discard training pixels (legacy default).
  `pathguiding_combine_training FALSE`.
- **Tier 1 MVP** — accumulate every training-iteration's pixels into
  the final image with sample-count weights (Müller 2017 §5 with
  uniform weighting).  `pathguiding_combine_training TRUE`
  (default in [PathGuidingField.h](../../src/Library/Utilities/PathGuidingField.h)).
- **Tier 1.5** — inverse-MSE-vs-previous-iteration weighting.  Tried
  and reverted: hand-rolled scalar MSE proxy isn't proportional to
  per-sample variance, gave inconsistent results across SPP regimes
  (small 16-SPP win, 64-SPP regression).  See deleted code in commit
  history for details.
- **Tier 2** — online mode: training-iteration loop *is* the entire
  render, every sample feeds both field and image.
  `pathguiding_online TRUE`.
- **Tier 2 + warmup** — the first N iterations render with α=0
  (pure BSDF sampling) before switching to the configured guide α.
  `pathguiding_warmup_iterations 2` is the recommended companion
  for online mode.  Samples still train the field during warmup —
  but because they're produced unguided, their pixels are
  statistically clean to keep in the final image.

K=16 EXR on `bdpt_jewel_vault` (16-SPP-final scene template, RIS,
max-depth 8), with the **variance ROI vs unguided 16-SPP** computed
per the protocol above.  Larger ROI = bigger PGL win at fixed wall
time; ROI < 1 means unguided beats this configuration:

| Condition                        | Wall  | mean σ² | mean ROI | P99 σ²  | **P99 ROI** | max σ²  |
|----------------------------------|------:|--------:|--------:|--------:|------------:|--------:|
| Unguided 16 SPP                  |   13s |  3.316  |  1.00    |  0.137  |   1.00      |  21317  |
| Tier 0 OFF (16 train + 16 final) |   22s |  8.621  |  0.23    |  0.216  |   0.38      |  51457  |
| Tier 1 MVP (combine, 16+16)      |   22s |  8.619  |  0.23    |  0.229  |   0.35      |  40212  |
| Tier 2 online (warmup=0)         |  ~14s |  5.260  |  0.59    |  0.038  | **3.34**    |  30753  |
| **Tier 2 online + warmup=2**     |   22s |  3.667  |  0.53    | **0.023** | **3.52**  |  12944  |
| Tier 1 MVP (combine, 16+64)      |   62s |  2.218  |  0.31    |  0.072  |   0.40      |   5743  |

Reading:

- **No PGL configuration beats unguided on mean σ² at fixed wall
  time** for this scene at this SPP regime.  Path guiding's training
  overhead doesn't pay back through reduced sampling variance for
  typical pixels, because most pixels in `bdpt_jewel_vault` are not
  actually that hard to sample.  This is a property of the *scene*,
  not a bug in the guide.
- **Tier 2 + warmup=2 wins decisively on P99 σ² (3.5×) and max σ²
  (1.6× vs unguided, 3.1× vs Tier 1 MVP).**  Path guiding is
  fundamentally a tail-variance tool — it's most effective on the
  hard-to-sample paths that produce fireflies.  When P99/max is what
  you care about (denoising, animation flicker, perceptual quality),
  Tier 2 + warmup=2 at ~22 s wall is roughly equivalent to ~80 SPP
  unguided.
- **Tier 1 MVP at fixed train:final ratio of 1:1 is a no-op** at
  this SPP regime because field non-determinism (per-trial random
  field training) dominates the per-pixel variance over the actual
  sample variance.  Tier 1 MVP only starts paying off at higher
  final SPP where sample variance becomes the limiting factor (we
  measured −10% mean σ² at 64 final SPP).
- **Median σ² consistently follows mean σ²**.  No configuration
  inverts that ordering — the bulk-of-pixels variance and the mean-
  pixel variance track each other on this scene.  Distinct
  median-vs-mean separations would indicate a small subset of pixels
  causing all the trouble (a firefly localization), but on this
  scene the variance is broadly distributed.

**RMSE-vs-reference was originally omitted from this table** because
references rendered with the buggy progressive convergence path
(see the historical section on *"When RMSE-vs-reference was
unreliable"*) disagreed with each other by ~5%.  Re-render any
references on a post-fix build before quoting RMSE numbers from
that era.

The takeaway: **the right framing for "does PGL help?" is metric-
specific.**  On mean σ², PGL doesn't pay on this scene at this
budget.  On firefly suppression, Tier 2 + warmup=2 is a real win.
A user asking "should I enable path guiding?" needs the answer to
match the metric that actually matters for their use case.

## Worked example: SPP-scaling matters more than you think

Same scene, same algorithm, comparing **fixed α (Cycles-style)** vs
**Adam-learned per-cell α (Müller 2017 v2)** on `pt_jewel_vault` with
one-sample-MIS guiding.  Tom94's reference says learned α should beat
fixed α — but only when each spatial cell sees enough samples for
Adam to converge from its 0.5 prior.

> **⚠ Caveat.** The numbers in the tables below were measured on the
> Windows build before commit `09be84b` added `RISE_ENABLE_OPENPGL` to
> `Library.vcxproj`, so guiding was compiled out and both "fixed" and
> "learned" reduced to the same unguided PT.  Differences are
> threading non-determinism, not a fixed-vs-learned signal.  The
> *principle* below — measure at multiple SPPs because a feature can
> flip from neutral to win as cells accumulate samples — is sound and
> consistent with Müller 2017 v2.  The specific numbers should be
> re-measured on a PGL-enabled binary before quoting.

K=16 at 32 SPP per pixel (final-render budget; cells get ~few×10²
updates each):

| Metric          | Fixed α | Learned α | Δ        |
|-----------------|---------|-----------|----------|
| mean σ²         | 4.495   | 4.511     | +0.36%   |
| 99th-pct σ²     | 3.133   | 3.039     | −3.0%    |
| RMSE vs ref     | 6.372   | 6.381     | +0.14%   |

K=16 at 256 SPP per pixel (cells get ~few×10³ updates each):

| Metric             | Fixed α | Learned α | Δ        |
|--------------------|---------|-----------|----------|
| **mean σ²**        | 1.950   | 1.910     | **−2.0%** |
| 95th-pct σ²        | 0.0240  | 0.0238    | −0.6%    |
| 99th-pct σ²        | 0.2415  | 0.2400    | −0.6%    |
| max σ²             | 2.75e4  | 2.69e4    | −2.2%    |
| relative noise σ/μ | 18.22%  | 18.04%    | **−1.0%** |

**The change goes from neutral → measurable win between 32 and 256
SPP.**  Don't conclude "feature X doesn't help" from a 32-SPP
measurement — re-test at 256+ SPP before reverting an
algorithmically-justified change.  Conversely: don't deploy a feature
that wins at 32 SPP without checking that it also wins at 256+ — some
features (immediate-update online learning with weak gradients) do
the opposite of this.

The mechanics that gave the 256-SPP win:

1. **Adam optimizer** (β₁=0.9, β₂=0.999, lr=0.01) instead of vanilla
   SGD — smooths the per-sample gradient noise that's the dominant
   problem at low SPP.
2. **Deferred f estimate** from path-completion `deltaResult ·
   combinedPdf / throughputBefore` — uses actual radiance flowing
   through the chosen direction, not a BSDF-only proxy that's
   uncorrelated with which directions are bright.
3. **Persistent cell state** across training iterations and into the
   final render — OpenPGL's kdtree only ever subdivides leaves
   (orphaned IDs are bounded memory leak, never reused), so the safe
   thing is to never clear the per-cell map until the field is
   destroyed.
4. **2× scaling** so initial learned=0.5 reproduces the fixed-α
   behaviour exactly, letting the variance-measurement experiment be
   a fair head-to-head.

An MVP without these — immediate updates with `f = BSDF·cos`, vanilla
SGD, map cleared on every iteration — is *worse* than fixed α at all
SPPs we tested.  Bad gradient estimators don't get rescued by more
samples; they get noisier.

## Going forward — for any rendering change

1. **Before you measure, prove the feature is on.**  Wall-time delta
   between guided/unguided.  Log line check
   (`PathGuidingField:: Initialized` in `bin/RISE_Log.txt`).  For a
   non-PGL feature, find the equivalent: a printf in the new code path,
   or check the binary links/imports the new dependency.  Pitfall #9
   exists because we burned hours measuring a no-op binary.
2. **Always include a "feature off" baseline**, not just A-vs-B
   between two on-states.  If A and B both lose to "off," the
   interesting question changed.  Pitfall #11 exists because PGL on
   `bdpt_jewel_vault` at 16 SPP is *worse* than unguided, and the
   master-vs-fix delta we were chasing was meaningless next to that.
3. Add a comparison entry to `var_test/<change-name>/master|fix/` so
   you can re-run the variance with new tooling later.
4. Save the K=16 master + fix EXRs and the reference somewhere if
   the change is significant — re-rendering ground truth on a future
   machine may give different results.
5. If a change shows P99 regression, run the SMS firefly diagnosis
   skill or look at firefly-trace output for the worst-offending
   pixel — it's often diagnosable.  But also reconsider: P99 is
   sensitive to a few outlier pixels, so K=16 P99 ratios in the ±50%
   range can be threading variance, not signal — re-test before
   building a hypothesis on it.
6. Keep this skill updated when you find new pitfalls.  The HDR RLE
   bug, the `LNK1104` SolutionDir trick, the bin/tools/ DLL copy, the
   missing `RISE_ENABLE_OPENPGL` define, and the training-jitter
   dominance at low SPP are all things future me will forget.
