# HYLIOS predecessor overlap predicate

This is a geometry prerequisite for stochastic particle placement, not a complete transport kernel.

`HyLIoSParticleOverlap.{h,cpp}` uses the existing `ProlateParticle` and `ParticleVector` types.
`ClassifyProlateOverlap(first, second, secondCentreRelativeToFirst)` accepts axes, orientations,
and a directly supplied relative displacement in metres. It does not subtract scene coordinates.

A particle with raw nonzero orientation `v` has shape matrix

    Q = a² I + (b²-a²) vvᵀ/(vᵀv).

This interprets the direction of the supplied binary64 vector as an exact real direction without
silently replacing it with a rounded unit vector. Independently power-of-two-scaled orientation
components and common power-of-two-scaled lengths retain this matrix and the dimensionless contact.

Perram and Wertheim, *Statistical mechanics of hard ellipsoids. I. Overlap algorithm and the
contact function*, JCP 58 (1985), 409–416, https://doi.org/10.1016/0021-9991(85)90171-8,
provide the ellipsoid contact criterion (maximum below 1: overlap; above 1: disjoint). This is a
clean-room implementation of the mathematical formula, not a source-code port.

    C(t) = (1-t) Q1 + t Q2
    x(t) = C(t)^-1 r
    f(t) = t(1-t) rᵀx(t)
    f'(t) = (1-2t) rᵀx(t) - t(1-t) x(t)ᵀ(Q2-Q1)x(t).

All construction/solve/derivative operations use outward binary64 intervals. Cofactor inversion
is accepted only when the determinant enclosure is strictly positive. Nonfinite intermediate
interval operations propagate an unbounded interval and produce Indeterminate, never a verdict.
The contact function is concave because it is the minimum over position of affine mixtures of
positive quadratic forms. A sample gives a lower bound for its maximum. The tangent at that
sample gives an upper bound over the bracket containing the maximizer. A positive/negative
derivative interval narrows that bracket. The result reports the evaluation count, capped at 64.
There is no distance tolerance or epsilon band. This is an ellipsoid predicate, not bounding-ball
exclusion; a tilted fixture explicitly distinguishes those behaviors.

Sphere pairs use an exact degree-two polynomial sign. Equal coordinate-aligned spheroids use an
exact degree-four polynomial sign. FMA TwoProduct and TwoSum floating expansions retain all bits
of these controls in the supported exponent range, resolving exact Touching and nextafter cases.
Other pairs can return Indeterminate near contact, including exact generic touching; transport
must propagate this diagnostic, not turn it into rejection/retry or a physical outcome. Touching
means no strict interior intersection. InvalidInput and UnsupportedRange are separate statuses.

This intentionally does not claim an exact predicate for every binary64 input. The supported
input spread is explicit: after independently scaling each orientation and jointly scaling all
lengths/displacement components so the maximum is in [0.5,1), every nonzero component must be at
least 2^-100. This is a supported exponent domain, not a contact tolerance. Degree-four exact
control products have no bit below 2^-612, avoiding arithmetic underflow. Some ill-conditioned
supported spheroids still return Indeterminate when the interval solve is too wide. Micron-scale
physical fixtures and shared scales from 2^-1000 to 2^1000 pass. Invalid rounding or FTZ/DAZ modes
return UnsupportedRange. No hidden RNG, TLS, cached scene state, or unbounded retry exists.

Compile the implementation with `-fno-fast-math -ffp-contract=off` (MSVC `/fp:strict`). The make library and test-copy rules, Android CMake source property, Visual Studio compile item and both Xcode source phases record these flags. Explicit `std::fma` is intentional. Ordinary, sanitizer and separate-TU full-LTO tests exercise the predicate; in the mixed build, the caller uses fast math while this TU stays strict.
