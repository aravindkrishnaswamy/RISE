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

## Worked example: OpenPGL fix evaluation

Background: an OpenPGL integration fix changed (a) the
`rrAffectsDirectContribution` flag from false to true,
(b) the recorded `russianRouletteSurvivalProbability`, and
(c) the recorded `eta` and `roughness` per path segment.  Question:
does it measurably help?

K=16 EXR variance on `pt_jewel_vault` (32 SPP, RIS, max-depth 8):

| Metric                | Master    | Fix       | Δ        |
|-----------------------|-----------|-----------|----------|
| mean σ²               | 4.520     | 4.508     | -0.28%   |
| median σ²             | 5.17e-5   | 5.09e-5   | -1.6%    |
| 99th percentile σ²    | 2.93      | 3.02      | +3.0%    |
| RMSE vs 256-SPP ref   | 6.379     | 6.383     | +0.07%   |

K=16 EXR variance on `pt_torus_chain_atrium` (32 SPP, RIS, max-depth 8):

| Metric                | Master    | Fix       | Δ        |
|-----------------------|-----------|-----------|----------|
| mean σ²               | 0.4292    | 0.4370    | +1.8%    |
| median σ²             | 6.22e-3   | 5.90e-3   | -5.2%    |
| 99th percentile σ²    | 6.08      | 5.98      | -1.6%    |
| RMSE vs 256-SPP ref   | 2.170     | 2.172     | +0.08%   |

K=16 EXR variance on `bdpt_jewel_vault` (16 SPP, RIS, max-depth 8):

| Metric                | Master    | Fix       | Δ        |
|-----------------------|-----------|-----------|----------|
| mean σ²               | 3.022     | 3.584     | +18.6%   |
| median σ²             | 9.68e-6   | 9.30e-6   | -3.9%    |
| 99th percentile σ²    | 0.090     | 0.149     | +66%     |
| RMSE vs 128-SPP ref   | 4.592     | 4.562     | -0.67%   |

Honest read: the fix is **net-neutral on average** but the median is
consistently slightly better while a few tail pixels get worse.  The
BDPT 99th-percentile spike (+66%) signals an RR amplification problem
in some path subset — likely the light-subpath
`rrAffectsDirectContribution=true` flag interacting with how Le is
recorded at vertex 1 of the reversed light walk.  Fix is correctness-
sound but doesn't yield a measurable speed win at this configuration.

## Worked example: SPP-scaling matters more than you think

Same scene, same algorithm, comparing **fixed α (Cycles-style)** vs
**Adam-learned per-cell α (Müller 2017 v2)** on `pt_jewel_vault` with
one-sample-MIS guiding.  Tom94's reference says learned α should beat
fixed α — but only when each spatial cell sees enough samples for
Adam to converge from its 0.5 prior.

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

1. Add a comparison entry to `var_test/<change-name>/master|fix/` so
   you can re-run the variance with new tooling later.
2. Save the K=16 master + fix EXRs and the reference somewhere if
   the change is significant — re-rendering ground truth on a future
   machine may give different results.
3. If a change shows P99 regression, run the SMS firefly diagnosis
   skill or look at firefly-trace output for the worst-offending
   pixel — it's often diagnosable.
4. Keep this skill updated when you find new pitfalls.  The HDR RLE
   bug, the `LNK1104` SolutionDir trick, and the bin/tools/ DLL copy
   are all things future me will forget.
