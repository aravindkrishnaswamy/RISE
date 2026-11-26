# Beauty adaptive luminance moments

The former online update `M2 += delta * (sample - roundedNewMean)` could
report zero variance for distinct adjacent binary64 samples. For example,
`1 + 2^-52` followed by `1 + 2*2^-52` rounds the second updated mean to the
second sample, erasing the second factor. Small mean updates could also
stall after a large sample count.

`Rendering/BeautyMoments` stores a compensated high/low mean and M2. For an
old count n, the second moment increases by `delta^2 * n/(n+1)` using the
compensated mean in delta. Mantissa scaling bounds the square; the prior M2
and increment are added at a common exponent before restoring physical
units. No fixed reference sample anchors the mean. Signed finite beauty
luminance, including a zero mean with positive variance, is supported.

The utility compiles with `-fno-fast-math -ffp-contract=off`. Binary64
error-free-transform nodes are explicitly materialized; FMA computes product
and quotient residuals. Compensation is finite precision, not exact real
arithmetic. Extremely small positive total M2 that rounds to zero, an
unrepresentable delta/M2, a nonfinite trial, or count exhaustion makes the
state permanently invalid. Invalid moments disable all adaptive early
termination, including the near-black shortcut; color accumulation continues
through the unchanged rasterizer color path. The uint32 counter is checked
before incrementing UINT32_MAX, so it never wraps.

Standard error takes the square root of compensated M2 before dividing by
`sqrt(n)*sqrt(n-1)`. This avoids the former sequential underflow of
`M2/(n-1)/n`; a nonrepresentable positive standard error disables convergence.
The historical 32-observation relative check, confidence `1-4/n`, strict
threshold comparison, and 64-observation near-black M2 shortcut are retained.
The shared check validates moments before considering the black shortcut.

All eight producer families use the same utility: PT Pel/spectral, BDPT
Pel/spectral, VCM Pel/spectral, and legacy shader-dispatch Pel/spectral.
Pel uses its existing max-channel statistic; spectral uses its existing
XYZ.Y statistic, including both hero wavelength and HWSS contributions.
Sampling/color/filter arithmetic is unchanged. The seven producers with an
adaptive configuration retain their adaptive-only gates. The legacy spectral
producer retains its existing progressive-only gate and threshold 0.01;
there is no adaptive configuration on that class. Both legacy camera-rejection
and ray-miss branches still add zero observations.

`ProgressivePixel::beautyMoments` persists the whole state between passes.
Copying a film carries both correction components and the sticky invalid
flag. `Clear()` default-initializes all of them. A repository consumer sweep
found progressive state in the eight producers, `ProgressiveFilm`,
`PixelBasedRasterizerHelper`, `BDPTRasterizerBase`, and FrameStore tests;
helper/base consumers read color/sample progress, not the moment fields.
No ProgressivePixel binary serialization or disk checkpoint exists in these
consumers. The separate radiometric checkpoint uses its own pixel type and
schema and is unaffected. All translation units using this internal struct
must be rebuilt when integrating the changed layout.

The measured arm64 layout is 40 bytes for BeautyMoments and 88 bytes for
ProgressivePixel, previously 72: **16 extra bytes per progressive pixel**
(31.64 MiB at 1920x1080, 126.56 MiB at 3840x2160). Local per-pixel estimator
state also grows from 24 to 40 bytes. Non-first adaptive observations now
perform compensated sums, FMA residuals, exponent extraction/scaling, and an
out-of-line update instead of the former short Welford update.
Convergence checks likewise call the strict utility. This has greater
arithmetic and call cost; no end-to-end render throughput claim or benchmark
is made. Nonadaptive producer gates continue to avoid moment updates, apart
from the legacy spectral progressive policy described above. Progressive
allocation pays the additional storage even when adaptive sampling is off.
No scene pointer, camera vtable, ray intersection layout, or TLS cache changes.

`BeautyMomentsTest` checks exact adjacent-float behavior, independently
computed Fraction/Decimal mean/M2/SE controls after 100,000 small updates and
near UINT32_MAX, signed samples, misses/zeros, overflow/range failure,
threshold policy, and exact progressive load/store/copy/reset state.
`beauty_moment_oracles.py` regenerates the committed oracle header from exact
sums and squared sums, independent of the implementation's recurrence.
`BeautyMomentsRasterizerTest` directly calls real Pel and legacy spectral
IntegratePixel seams with controlled camera/caster trials. It checks adjacent
floats through actual progressive passes, zero misses/rejections, sticky
failure, and original producer gates. It schedules no image render.
