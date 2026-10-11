# DL-532 batch 12 camera/light scratch gates

See [design and setter audit](BATCH12_DL532_DESIGN.md). Only the exact native Object, PinholeCamera, ThinLensCamera, FisheyeCamera, OrthographicCamera, PointLight, SpotLight and DirectionalLight types may consume the call-local final arithmetic result. Their setters synchronously copy values. Unknown, retaining and subclass targets still receive an owned final clone. Exact built-in Parameter and linear-interpolator checks are unchanged, as are the arithmetic order and parameter ID. AmbientLight remains outside this whitelist.

## Allocation red/green

`NativeKeyframeAllocationTest` fails on master `72beda3ab` plus the test with **22 failures**; after the Timeline whitelist change it reports **0 failures**. Counts are C++ operator-new calls and cover 4,999 interior interpolation times per case. The independent legacy owned expression supplies the reference before the measured window; every case has zero bit mismatches. Timeline evaluation alone is counted, not map construction or pose finalization. These are deterministic counters, not repeated-render statistics (SD0 per evaluation).

| Exact target | Parameters measured | Allocations/interpolation before → after |
|---|---|---|
| PinholeCamera | location, orientation, target_orientation, exposure | 2 → 0 |
| ThinLensCamera | location, orientation, target_orientation, exposure | 2 → 0 |
| FisheyeCamera | location, orientation, target_orientation, exposure | 2 → 0 |
| OrthographicCamera | location, orientation, target_orientation, exposure | 2 → 0 |
| PointLight | energy, colour | 2 → 0 |
| SpotLight | energy, colour | 2 → 0 |
| DirectionalLight | energy, colour | 2 → 0 |

The 22 cases cover Point3, Vector3, Vector2, Scalar and RISEPel. RetainingPointLight, a subclass that retains the result, still allocates 9,998 times per 4,999 interpolations and preserves its final retained value after Timeline destruction. It therefore also gates refusal of native subclasses.

`ObjectKeyframeAllocationTest`: 0 allocations / 4,999, zero mismatches, rc0. `KeyframeAllocationTest`: 9,998 allocations / 4,999, zero mismatches, retained results survive, rc0. Its documented `--zero-allocation-pin` remains rc1 until an owner ruling. Nonlinear/custom interpolation, map lookup, finalization and generic owned results remain outside this exact slice; this does not claim universal zero-allocation posing.

## Build and sanitizer gates

All six individual named builds returned0, with no compiler warnings. Focused ASan/UBSan instrumented Timeline.cpp and separately NativeKeyframeAllocationTest, ObjectKeyframeAllocationTest and KeyframeAllocationTest; all three run return codes0, no sanitizer diagnostics. Other linked library translation units were not instrumented. The focused retaining tests exercise result ownership across evaluation and Timeline destruction.

## Render and timing gates

After merging DL-482 from master47f2da653 into this branch, rebuilt all six named tests, reran counters and focused sanitizers, then full TimeIndexedMotionBlurTest **96/0**, AnimationRasterizerParityTest **102/0**, MotionBlurTimeAverageTest **77/0**; every actual build/run rc0 except the intentionally red generic strict pin (rc1). The library build emitted no warnings.

The preserved same-source baseline differs only in Timeline.cpp's final-consumer whitelist; both sides otherwise use master47f2da653 and identical test sources. One worker, render_thread_reserve_count0, ValueSalt532120+i*7919 (the child applies its normal HashCombine), PT8spp on 192x192 combo_big; timing encloses RasterizeAnimation only and excludes loading. Alternating A/B and B/A order across n8 pairs; no other own render/build job overlapped timings. All **24** cross-build one-thread hashes (combo/ext_combo, PT/BDPT/VCM x RGB/spectral, shared/time-indexed paths) and **8** benchmark-pair hashes match exactly. Pixel-difference SD0; one salt per small correctness fixture, n8 benchmark salts.

| Metric | Before mean / sample SD | After mean / sample SD |
|---|---|---|
| Whole-frame seconds (n8) | 0.533072932 / 0.005342943 | 0.495902427 / 0.003226179 |

Paired speedup mean **1.074972x**, sample SD **0.010416** (n8), three-SE interval [1.063924, 1.086021]. This interval excludes1, supporting a measured improvement on this fixture. A whole-frame timing does not isolate allocation cost, and no universal posing-speedup claim is made. Counter results establish removal of the two final-result operator-new calls for the measured native interpolation cases.

## Self-review

Checked all immediate-copy setters in the exact whitelist, subtype refusal with an actually retaining native subclass, unchanged arithmetic order/IDs, all five numeric payload types, independent legacy values and post-Timeline lifetimes. No sampler, camera/light optics, transform-finalization, frozen feature or public interface change is made. Fresh independent review is required before merge.
