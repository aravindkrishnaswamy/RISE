# DL334: absolute contrast at grazing incidence

Fresh review found that conservation alone missed an inaccurate physical law.
At measured 503 nm, nt=1.48, ni=nextafter(nt,0)=1.4799999999999998 and
mu=1e-8, the lower-to-higher branch gave R=.080840058312675725 instead of
Decimal110 exact-binary64 R=.111133435057377626. T had the opposite error, so
R+T was exactly one. Twenty fresh measured/Sellmeier raw/green cases reproduced
this; the earlier critical repair's gate is historical evidence.

Both index orderings now retain the absolute contrast before division. Let
L=max(ni,nt), S=min(ni,nt), q=S/L and delta=(L-S)/L. Then
1-q^2=delta*(2-delta). The subtraction retains its TwoSum residual, the division
its explicit FMA residual, and the contrast product its FMA and low-component
residuals. For adjacent indices the subtraction is exact by Sterbenz's lemma;
its general residual also preserves farther-index critical cases. The contrast
error scales with the actual contrast rather than an order-one rounded square.

For ni<nt, cosT^2=(1-q^2)+(q*mu)^2 is a sum of nonnegative terms. For ni>nt,
cosT^2=(mu^2-(1-q^2))/q^2; the square and subtraction residuals retain the small
critical discriminant. Its computed sign alone determines TIR. Exact matched
indices and exact normal incidence retain their existing shortcuts. All index
operations are scaled; no absolute index square can overflow. No epsilon,
matched-index snapping, Fresnel clamp, changed transmission window or acceptance
tolerance is introduced. Local compiler ordering controls remain necessary.

SSSNearUnityFresnelTest is separate from all three prior regression sources. It
contains 3128 independently generated Decimal110 exact-binary64 fixtures over
fresh measured and Sellmeier wavelengths 397/457/503/613/727 nm, adjacent indices
on both sides (1/2/8 ULP), near-unity and unequal ratios, critical ULP neighbors,
matched/grazing/normal cases, seven absolute scales from 1e-6 to 1e6, three
coordinate axes and common power-of-two index scalings. Public diffusion/RW/skin
SPFs, profile transmission, entry adapters, shared NM and constant Pel paths are
checked against individual R and T oracles, not only partition.

The cosine test bound propagates compensated-discriminant rounding and sqrt
conditioning: 32*eps*|cosT| + 64*eps^2*max(contrast,mu^2)/(cosT*q^2).
The Fresnel bound adds 64*eps and at most 8*cosineBound/max(mu,cosT) from the
polarized amplitude derivatives. These conservative engineering bounds are
computed from independent Decimal values; they are not a production tolerance
or a claim of arbitrary-precision behavior. Strict represented TIR-side checks
remain. The partition acceptance remains 1e-6.

Shared vertex evaluation has an existing cosTheta<=NEARZERO admission gate,
which the test explicitly checks as zero instead of interpreting it as a
Fresnel-law output. Adapters have open-hemisphere support. RW pointwise Ft is
tested separately from de-normalized adapter weights: multiplying a normalized
weight back by its normalization adds ordinary rounding and is not an exact
[0,1] physical transmission observation. Both are still compared to the same
independent physical T with the derived bound. No production gate changed.

Only Optics.cpp changes production in this follow-up. No header, virtual API,
layout, material query, seeding, density, strategy, eta-square or MIS arithmetic
change occurs. The earlier caller audit still applies: all scalar/vector
refraction/Fresnel callers, Pel/NM/HWSS material consumers, manifold/fibre,
BioSpec layers, RW, straight shadows and graded refraction inherit this kernel.

Tests and compiler execution cover macOS ARM64 Clang shipping O3/LTO/fast-math
and actual Opto objects. GCC/MSVC controls are source-audited, not executed.
Compensated binary64 still describes represented inputs rather than recovering
unrounded geometry; subnormal/flush-to-zero edge behavior is not claimed by the
normal-input corpus. Existing containment/topology and bounded furnace limits
are unchanged.
