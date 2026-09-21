# Legacy photon transport and directional gathers

Work in progress, 2026-09-21. Owned rows DL239, DL271, DL272 and DL279–282
have distinct mechanisms and committed direct proofs. Final source-sensitive
gates, render validation, costs and independent review remain pending.

## Measures

An incident photon packet estimates `Li(wi) |Ng.wi| dA dwi`. A density gather
therefore prices the query BSDF as

```
f_area(wi,wo) = f_shading(wi,wo) |Ns.wi| / |Ng.wi|.
```

This changes the response, not the spatial kernel area, thin-surface slab,
solid-angle PDFs, or geometric path-density Jacobians. The BSDF owns its
reflection/transmission support; a signed shading-normal cosine test in the
gather incorrectly rejects valid directions when both light and view lie below
Ns but above Ng. An exactly zero geometric cosine contributes zero measure; the
implementation does not introduce a grazing clamp.

A light walk samples `wo`, and its SPF weight already contains
`f_shading |Ns.wo| / pdf(wo)`. Its additional adjoint factor is

```
A(wi,wo) = |Ns.wi| |Ng.wo| / (|Ng.wi| |Ns.wo|).
```

This factor is distinct from discrete lobe-selection compensation. A selected
ray carries `kray / q`; a branch-all consumer carries each `kray` once. The
adjoint belongs to importance walks and adds no radiance-mode eta-squared
factor. Both directions point away from the surface. SMS must recover `wi`
from the saved intersection because its local loop ray has already advanced.

## Legacy selector law (DL271/DL272)

The three old two-argument selector entry points remain available. The new
optional probability output reports the continuous legacy experiment while
preserving returned pointer identity, CDF comparisons, iteration order, stored
weights/PDFs, IOR stacks, and count/type shortcuts. Count-one and the filtered
two-record shortcut return probability one even for zero response. A null
selection reports zero. Inputs require finite nonnegative weights, finite
sums, and a random argument in `[0,1)`.

Floating CDF intervals and the finite random grid are not an ideal continuous
experiment: an extreme positive weight can disappear in a rounded cumulative
sum, and a quotient can underflow. The tests distinguish the intended law from
these unchanged legacy limitations. Probability comparisons use a derived
rounding bound only where the operands and products are normal; pointer and
shortcut comparisons remain exact. An intentionally halved probability is
rejected by that oracle.

The deficient consumers include global/caustic Pel and NM photon tracers, SMS,
the non-gradient FinalGather selector and four detector families. IsotropicRGB
spectral measurement also selected with RGB weights after ScatterNM (DL272).
DistributionTracing's NM method separately called RGB Scatter and then read
krayNM; it now calls ScatterNM. Its default HWSS implementation delegates each
active wavelength to that method. Standard/Advanced shader NM and HWSS dispatch
were already correct, as were Reflection/Refraction's supplied-record consumers.
FinalGather's existing NM implementation intentionally uplifts its RGB gather;
this slice does not turn it into a wavelength-resolved final gather.
Five PT/BDPT manual probability reconstructions now use the same selector
result. PT's positive-small-probability termination was separately reproduced
and removed; its existing throughput-above-1e6 termination remains unchanged.
This slice does not claim arbitrary-throughput unbiasedness. DistributionTracing
is a selection-probability negative control: multiple records are traced as
branches, and its selector is only used for at most one record. That conclusion
is separate from its corrected NM dispatch. The expanded actual-material and
synthetic mode proof is 136/10 in the committed red mode state and 136/0 after
the two mode corrections. The actual Phong control uses a physical zero SPD
for Rs; an earlier RGB-black setup had a small nonzero spectral uplift and its
failed exact-equality observation is retained as a setup correction.

## Spatial search (DL279)

The balancing recursion uses inclusive endpoints, so `nth_element` must receive
`to+1`. Excluding the final record violated the KD half-space invariant.
A query exactly tangent to a partition must still visit the near subtree.
Finally, a k-neighbor search must not contract its radius after collecting only
k-1 records. The independent exhaustive-record oracle covers five record
families, empty/small/large sets, several k values, capped counts, and exact
partition tangency with adjacent representable radii. Zero-neighbor requests
return immediately: the old path popped its sole candidate then read the empty
heap. The committed zero-k test traps under libc++ extensive hardening before
that guard and passes 3390 checks afterward.

The initial spatial proof was 3000 checks / 390 failures. Partition/tangency
corrections left 40 failures; raw distances showed k-1 premature contraction.
Correcting that mechanism gave 3000/0. Spatial repair changed the fixed caustic
flat response from 2038.4282729247636 to 2568.1443048892588 before angular
correction. Those baselines must not be mixed when attributing the cosine fix.

## Directional anchor representation

The full incident field is retained. A quarter-density compact KD tree stores
anchor position and exact geometric normal. At query time, the anchor supplies
only the uniform spatial kernel center, slab normal and projected-area
normal. The query supplies material position, BSDF support, shading frame and
view direction. Each retained incident direction is evaluated separately.
When no compatible anchor exists, the map uses its direct Gaussian query
estimator. Direct Gaussian gathering is a valid alternative but changes the
finite spatial estimator; it is not a cache-equivalent substitution.

A scalar precomputed irradiance cannot support an arbitrary changed shading
frame or non-Lambertian query BSDF. The old cache also selected anchors using
incident photon direction instead of the surface normal. The replacement uses
exact geometric normals with the existing intended 0.9 similarity policy.

Historical record sizes in the bounded prototype were 88 bytes per full packet and 56 bytes
per anchor, versus 96 bytes per old irradiance record. At sizes divisible by
four, that prototype retained record storage was therefore 4.25 times the old quarter cache,
and 1.15909 times its full directional array alone. The final exact-direction
layout is larger and needs renewed measurements; these prototype figures are
not final-production costs. These are record/vector
bytes, not total allocation or RSS. The old scalar query evaluated one BSDF;
the directional estimator evaluates up to k, a real cost.

The matched-plane prototype compared direct Gaussian and anchored uniform
queries at actual anchors with identical candidate populations. Each mode
performed exactly 300000 BSDF evaluations per run (2000 queries, k150). At
1k/10k/100k packets, n3 timing means/sample SD were respectively
0.036721/0.002559, 0.081961/0.000181, 0.097777/0.000358 seconds for direct,
and 0.034708/0.000851, 0.082179/0.000433, 0.097882/0.000724 for anchored.
Kernel weights and resulting values differ. The earlier oblique-plane timing
has unequal slab acceptance and is retained as such. Neither experiment is a
final-production or universal performance bound.

Anchors store geometry only. They read live packet powers, so power rescaling
needs no geometry rebuild. New packet insertion invalidates anchors. Explicit
precomputation and gather-parameter changes rebuild them. The full packet array
remains the authoritative stored/count/scaling population.

## Serialized compatibility

The final Global Pel format flag3 stores exact incident vectors, exact geometric
normals, normal provenance and anchor spacing. Caustic Pel and both spectral
maps use an exact-direction version2 envelope with a checked map-kind tag.
Spectral streams additionally preserve the wavelength interval, branch/count,
NM half-width and all exact discrete representatives. Loads validate the whole
stream into temporary records before replacing the map. Failed Job loads
return false and retain the installed map; command parsing propagates that
failure. Public constructors/factories and Store entry points remain, but packet
layout and spectral virtual-interface additions require clients to rebuild.
There is no ABI-compatibility claim.

All older compressed-direction Global raw/flag2, Caustic Pel and spectral
streams are intentionally rejected with a regenerate-from-scene diagnostic.
Their quantization can change the side of the geometric horizon, so they cannot
supply the corrected area response even when they retain theta/phi. Scalar-only
Global flag1 caches also lack the incident field. An earlier intermediate
loader accepted legacy raw max+1 packets; its committed red/green evidence is
retained historically, but that policy is superseded by the exact-support
requirement. No incompatible map is silently installed as empty or relabelled.

The repository scene/test/asset census found no committed photon cache files
or shipped load/save commands requiring regeneration. Runtime-generated legacy
fixtures remain deliberate rejection tests. New exact-format roundtrip,
insertion, truncation and invalid-direction tests exercise each packet family.

## Exact incident support (DL239)

An eight-bit polar direction can map a below-surface incident ray to a tiny
positive cosine at the horizon. The projected-area division then magnifies the
lost sign. Actual caustic gathering of a below-support packet produced about
1.77e15 instead of zero; the shipped normal-map candidate retained a bright
outlier rather than treating it as statistical noise. All directional packets
now retain the original vector, without horizon snapping or an epsilon clamp.
Rotated geometric normals, positive/negative near-horizon directions and exact
tangency are tested independently. The full cache uses those same vectors.

A neighborhood whose selected radius squared is zero has no finite-area density
estimate. DL279 returns zero before any reciprocal for this exact degeneracy.
Seven committed co-located gather controls failed with NaN before the guard;
positive-radius cases retain their established kernels and weights.

## Wavelength measure (DL281)

Each spectral map declares one uniform producer law. Counts below10000 sample
uniformly among stored, strictly ordered representable atoms in [a,b); larger
counts use the continuous branch. The actual stored atoms drive sampling,
packet membership and window probability, and survive serialization exactly.
An empty unconfigured map cannot Store until its valid law is set. A nonempty
map cannot change laws. The continuous branch maps a rounded upper endpoint to
its preceding representable value using the same draw; this enforces support in
finite arithmetic and is not a claim of exact continuous sampling on a machine.

Packet power is conditional spectral power divided by the total shot count.
An NM gather with half-width h divides by the producer probability of its
clipped wavelength window: k/N for discrete atoms or clipped length/(b-a) for
the continuous branch. Zero-mass and out-of-support queries return zero. Spatial
neighbor selection is independent of that spectral window. A Pel gather instead
sums CMF(lambda) times packet power times the actual scalar valueNM BSDF, then
multiplies by (b-a)/integral(Ybar). Multiplying XYZ channels by an RGB BSDF is
not this spectral integral. Finite windows smooth spectra; this correction does
not claim exact evaluation for arbitrary nonsmooth spectral functions.

The independent flat and colored fixed-deposit oracle covers both global and
caustic families, five window widths and clipped boundaries. The first committed
measure red exposed eight normalization failures; after represented-grid and
caustic-denominator corrections, the expanded oracle passes4069 checks. The
first failed candidate and all operands remain in the external evidence index.

## Partial CIE cells (DL282)

CIE_Y_Integral formerly truncated interval endpoints to five-nanometre table
indices. For example [550,551] returned zero instead of .9955. The corrected
helper integrates each clipped linear table segment, including partial boundary
cells. Empty/reversed intervals and nonfinite endpoints return zero; values
outside [380,780] contribute zero. Standard aligned ranges retain their integral.

The helper sets the constructor scale in PixelBasedSpectralIntegratingRasterizer
and its PT/BDPT/VCM spectral subclasses, including HWSS and splats, as well as
new spectral-photon metadata. MLT uses a separate normalization path and is not
an affected caller of this helper. Committed ordinary/HWSS constructor controls
fail for unaligned ranges and pass for aligned400..700 and380..780 before the
fix. This is integration of the tabulated observer, not a correction for arbitrary
nonsmooth scene spectra.

## Scope boundaries

Translucent nonreciprocity and stackless/reconstructed state limitations remain
separate DL223 concerns. A normal-measure conversion alone does not prove
reciprocity, arbitrary material conservation, or agreement of every estimator.

## Translucent packet meaning and DL280

Ordinary receivers store full incident flux. Subtracting the traced specular
continuation at a mixed Phong receiver removed part of that flux before its
query BSDF was evaluated. The new actual-material proof exposed all 96 affected
channel samples; this is DL280, separate from lobe-selection compensation and
normal conversion. The earlier DL39 pure Lambertian wall had no traced
non-diffuse lobe and could not distinguish that subtraction from full flux.
Its Beer-weighted interior-exit fix remains valid.

A translucent interior exit instead stores `power * Beer * (1-scattering)`.
That packet already includes the exit lobe weight. Its remaining clipped-cosine
law has density `|Ns.wo| / (pi * valid)` on the exterior support, where
`valid=(1+dot(orientedNs,NgExterior))/2`. Multiplying this density by the adjoint
factor and dividing by the outgoing geometric projection gives

```
exit_area = |Ns.wi| / (|Ng.wi| * pi * valid).
```

The gather therefore retains incident direction and an explicit exit/incident
kind. Incident packets evaluate the query BSDF; exit packets apply this law and
its exterior support without multiplying front reflectance or Beer again.
The real tracer tests sweep exit shading tilt, view, per-channel front colors,
Beer and scattering. Together with compatibility tests they pass 243/0 at
213e2fe5. The initial exit-to-gather red was 74 checks / 39 failures; adding the
mixed-material deposit proof gave 171/135, and fixing only DL280 gave 171/39.
This isolates the two mechanisms.

The old `TranslucentPelPhotonMap::Store(power,pos)` symbol remains, but now
reports an error and returns false without mutation because it cannot provide
the missing direction/type. This is an intentional behavior change. The sole
in-tree production caller is `TranslucentPelPhotonTracer::TracePhoton`, migrated
to the directional overload. Public factory/Job construction is unchanged;
`IPhotonMap` exposes no Store method. The shader uses the Pel map; its existing
NM entry remains unsupported and returns zero, so there is no new claim of a
translucent spectral photon-map implementation.

New translucent files carry an impossible legacy header marker, version1,
and exact incident direction/kind per record. Directionless legacy files are
rejected with a regenerate diagnostic. Checked Job loading preserves the
installed valid map on failure. Tests cover old Store rejection with spare
capacity, unchanged records, exact tagged roundtrip, truncation retention,
legacy Job-load rejection, and successful new-format Job load.
