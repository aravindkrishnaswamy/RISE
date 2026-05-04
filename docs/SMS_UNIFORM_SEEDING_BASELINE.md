# Phase 0 baseline numbers (pre-Option-A)

This file captures the numerical state of RISE's SMS implementation **before** any Option-A changes (per `docs/SMS_UNIFORM_SEEDING_PLAN.md`).  Every subsequent phase's verification gate compares against these numbers.

Captured: 2026-05-02 with `SMS_DIAG_ENABLED = 1` in `src/Library/Shaders/PathTracingIntegrator.cpp`.

## Build / config snapshot
- Branch: `master`
- Tip: includes the staged two-stage SMS work (`docs/SMS_TWO_STAGE_SOLVER.md`).
- Build: `make -C build/make/rise -j8 all`, clean rebuild, no warnings.
- Diagnostic: `SMS_DIAG_ENABLED=1` (PathTracingIntegrator only — BDPT does not feed this diagnostic).
- Sample count: **all scenes rendered at 4 spp** (uniform across corpus).  Per-scene authored spp varies; for baseline comparability and iteration speed all corpus scenes use 4 spp.  This is sufficient for stable SMS evaluation counts because SMS evaluations occur at every diffuse hit not only every camera-sample.
- Working scene copies: `/tmp/rise_baselines/*_baseline.RISEscene` (4 spp variants of the corpus).

## Test corpus

| Scene | spp | mode | Notes |
|---|---|---|---|
| `mlt_veach_egg_pt_sms.RISEscene` | 4 | PT+SMS, biased=TRUE | Smooth analytic ellipsoid + air cavity (k=2 chains). **Gold reference** — correctness gate at every phase. |
| `mlt_veach_egg_pt_sms_bumpmap.RISEscene` | 4 | PT+SMS, biased=TRUE, two_stage=TRUE | Smooth ellipsoid + bumpmap. Validates two-stage solver work. |
| `mlt_veach_egg_pt_sms_displaced.RISEscene` | 4 | PT+SMS, biased=TRUE, multi_trials=16, photon_count=1e5 | Heavily-displaced outer ellipsoid. Literature open problem. |
| `scenes/Tests/SMS/sms_k1_refract.RISEscene` | 4 | PT+SMS, biased=TRUE | k=1 refraction unit test. |
| `scenes/Tests/SMS/sms_k1_botonly.RISEscene` | 4 | PT+SMS, biased=TRUE | k=1 with single-side caster. |
| `scenes/Tests/SMS/sms_k2_glasssphere.RISEscene` | 4 | PT+SMS, biased=TRUE | k=2 nested-glass scene. |
| `scenes/Tests/SMS/sms_k2_glassblock.RISEscene` | 4 | PT+SMS, biased=TRUE | k=2 glass-block scene. |
| `scenes/Tests/SMS/sms_slab_close_sms.RISEscene` | 4 | PT+SMS, biased=TRUE | Slab caustic, close camera. |
| `scenes/Tests/SMS/sms_slab_close_bdpt_sms.RISEscene` | 4 | BDPT+SMS, biased=TRUE | Spectral path validation (PT diagnostic does not fire — BDPT has its own integrator). |

## Baseline metrics

| Scene | Wall-clock | sms_evals | sms_valid | valid/evals | ΣL_sms | ΣL_supp | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| smooth Veach egg | 6.892 s | 2,216,320 | 918,256 | **0.4143** | 2.925e+05 | 3.163e+05 | **0.9246** |
| bumpmap Veach egg | 41.697 s | 2,216,395 | 819,073 | 0.3696 | 3.268e+05 | 3.139e+05 | 1.0410 |
| displaced Veach egg | 29.318 s | 2,214,253 | 304,962 | 0.1377 | 3.955e+04 | 3.110e+05 | **0.1272** |
| k1_refract | 0.148 s | 101,026 | 12,929 | 0.1280 | 2.053e+04 | 8.664e+03 | 2.3698 |
| k1_botonly | 0.138 s | 101,200 | 16,866 | 0.1667 | 8.174e+03 | 8.807e+03 | 0.9281 |
| k2_glasssphere | 0.142 s | 99,016 | 5,542 | 0.0560 | 4.629e+03 | 1.110e+03 | 4.1720 |
| k2_glassblock | 0.209 s | 101,413 | 17,851 | 0.1760 | 1.114e+04 | 1.084e+04 | 1.0280 |
| slab_close_sms | 15.053 s | 409,865 | 93,018 | 0.2269 | 5.311e+04 | 1.748e+04 | 3.0376 |
| slab_close_bdpt_sms | 14.746 s | 0 | 0 | — | 0 | 0 | (BDPT path — PT diagnostic doesn't fire) |

## Interpretation

- **Smooth Veach egg ratio 0.92** — close to but not exactly 1.0; the gap is the sum of (a) MIS-weight rounding, (b) emission-suppression overcounting at non-SMS hits in the same chain, and (c) the biased mode's count-once-per-trial-loop semantics.  This is the gold-reference number.  **Option A's geometric Bernoulli (Phase 5) should produce ratio ≈ 1.00** on this scene — that's the non-negotiable correctness gate.
- **Bumpmap Veach egg ratio 1.04** — slight over-count (consistent with Phase 0's lower spp, was 1.15 at 16 spp during prior measurement).  Two-stage solver's design intent.
- **Displaced Veach egg ratio 0.13** — large energy loss; literature open problem.  Option A is **not** expected to fix this; it's expected to keep it stable.
- **k1_refract 2.37, k2_glasssphere 4.17, slab_close_sms 3.04** — these scenes show ratio > 1 in baseline.  Mechanism: biased multi-trials sum distinct converged solutions per shading point, and these scenes have multiple coexisting basins per pixel (typical of caustic concentrations).  Eq. 8's biased estimator is consistent (`Σ_l f(x₂⁽ˡ⁾)`) so over-1 ratios are paper-canonical, not bugs.  Each post-Phase verification must keep these ratios in the same ballpark.
- **k1_botonly 0.93, k2_glassblock 1.03** — well-behaved scenes near unity.

## Per-phase pass criteria (anchored to these numbers)

For each subsequent phase:
- **Default-mode renders**: every metric within ±5% of baseline (Phases 1, 2, 3 — no functional change).  Bit-exact ideal but may differ at the last decimal due to FP non-determinism in multi-threaded reductions; ±5% tolerance is the operational gate.
- **Phase 5 unbiased uniform-mode smooth Veach egg**: ratio in [0.95, 1.05].  This is the math working.
- **Phase 5 unbiased uniform-mode displaced Veach egg**: ratio improves from 0.13 baseline, but no specific target — literature open problem.
- **Phase 6 multi-scatter validation**: k=2 air-cavity caustic appears in uniform-mode renders.
- **Phase 7 photon-aided integration**: scenes with `sms_photon_count > 0` show no regression vs Phase 6 numbers.

## Reproducer

To re-render this baseline:
```sh
cd /Users/aravind/Working/GitHub/RISE
# 1. Enable diagnostic
sed -i '' 's/^#define SMS_DIAG_ENABLED 0$/#define SMS_DIAG_ENABLED 1/' src/Library/Shaders/PathTracingIntegrator.cpp
make -C build/make/rise -j8 all
# 2. Render corpus
export RISE_MEDIA_PATH="$(pwd)/"
for scene in /tmp/rise_baselines/*_baseline.RISEscene; do
  echo "=== $(basename "$scene") ==="
  printf "render\nquit\n" | ./bin/rise "$scene" 2>&1 | grep -E "SMS-DIAG|Rasterization Time"
done
# 3. Disable diagnostic
sed -i '' 's/^#define SMS_DIAG_ENABLED 1$/#define SMS_DIAG_ENABLED 0/' src/Library/Shaders/PathTracingIntegrator.cpp
make -C build/make/rise -j8 all
```

## Next step

Phase 1: Citation hygiene.  Fix the fabricated "Kondapaneni 2023" citation in source/docs to "Weisstein, Jhang, Chang. 'Photon-Driven Manifold Sampling.' HPG 2024. DOI 10.1145/3675375".  Re-render this corpus, expect numbers identical (or within ±0.1% rounding) to baseline.
