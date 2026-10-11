# DL-532 exact copying-target slice

Written before coding, against master 987b43824. The remaining final linear
Parameter clone owns a mutex and must survive if an arbitrary target retains
it. Removing that ownership globally breaks the existing retaining-target
regression. No interface/vtable change or opt-in inference is justified.

Exact subset: target dynamic type Object, whose inherited Transformable
SetIntermediateValue copies each supported numeric value synchronously and
never retains the parameter. Built-in Parameter types and the built-in linear
interpolator already have exact-type checks. Use a borrowed final scratch only
for that target, with the original parameter ID and the same ScalarMult/Add
operations and order. Object subclasses, custom targets and interpolators
retain the existing owned path. The generic zero-allocation strict pin stays
red; this is a narrower exact slice, not total closure.

Gate: count allocations for 4999 interpolated position samples, compare their
bits with the independent legacy expression, retain custom-target lifetime
test. One-thread TimeIndexedMotionBlurTest hashes, AnimationRasterizerParityTest,
MotionBlurTimeAverageTest; alternating old/new renders on a preserved baseline,
reserve0, n8 ValueSalts, no overlapping own build/render jobs during timing.

## Exact counter evidence

`ObjectKeyframeAllocationTest` counts C++ operator-new calls: production master9998 allocations for4999
interpolated position evaluations, zero bit mismatches, rc1; copying-target
scratch0 allocations, zero bit mismatches, rc0. Independent legacy interpolation
computes the comparison before the measured window; object finalization is
outside the allocation window. This measures Timeline evaluation, not all pose
work. Existing generic Scalar counter remains9998 and its retained results
survive later evaluations and Timeline destruction; its zero-allocation strict
pin remains rc1. Focused ASan/UBSan on Timeline.cpp and the Object counter rc0;
other linked library objects are not instrumented.

Self-review checked exact dynamic type (subclasses cannot opt in accidentally),
original parameter IDs, the same arithmetic calls/order, and target ownership.
No camera/light/custom-target ownership, interpolation mode, sampler, map lookup
or transform-finalization change is made. The class whitelist is intentionally
narrow; direct non-Object final-result allocations remain open.

Full named gates returned0: TimeIndexedMotionBlurTest96/0, AnimationRasterizerParityTest102/0, MotionBlurTimeAverageTest77/0. All five named builds returned0 without compiler warnings. Interleaved old/new combo_big PT8spp, reserve0, one thread, n8 ValueSalts: .631439937s SD.011153423 -> .590364870s SD.017041092, paired1.070041825x SD.023260946. All eight benchmark RGBA hashes match exactly. The timing measures the whole frame, not a claimed allocation-only cost. No other own build/render job overlapped the timing pairs.

P2 direct-counter coverage is position/Point3 only; orientation/scale rely on rendered hashes. Exact Object subclasses remain refused by the dynamic-type guard, with arbitrary retaining targets covered by the existing lifetime test.
