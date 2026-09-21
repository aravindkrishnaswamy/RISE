# Legacy photon transport and directional gathers

Work in progress, 2026-09-21. DL239, DL271, DL272 and DL279 have independent
mechanisms and separate direct proofs. Closure, final gates and reviewed cost
claims are pending. DL280 is reserved pending the mixed-material deposit proof.

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
Five PT/BDPT manual probability reconstructions now use the same selector
result. PT's positive-small-probability termination was separately reproduced
and removed; its existing throughput-above-1e6 termination remains unchanged.
This slice does not claim arbitrary-throughput unbiasedness. DistributionTracing
is a negative control: multiple records are traced as branches, and its selector
is only used for at most one record.

## Spatial search (DL279)

The balancing recursion uses inclusive endpoints, so `nth_element` must receive
`to+1`. Excluding the final record violated the KD half-space invariant.
A query exactly tangent to a partition must still visit the near subtree.
Finally, a k-neighbor search must not contract its radius after collecting only
k-1 records. The independent exhaustive-record oracle covers five record
families, empty/small/large sets, several k values, capped counts, and exact
partition tangency with adjacent representable radii.

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

Record sizes in the bounded prototype are 88 bytes per full packet and 56 bytes
per anchor, versus 96 bytes per old irradiance record. At sizes divisible by
four, retained record storage is therefore 4.25 times the old quarter cache,
and 1.15909 times the full directional array alone. These are record/vector
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

Global map format flag2 retains all directions, exact geometric normals,
normal provenance and anchor spacing. Legacy flag0 maps contain full directions
and load as direct gathers; their old stored shading normals are not relabelled
geometric normals. Legacy flag1 scalar caches irreversibly discarded the
incident field and are rejected with a regeneration diagnostic. Checked parsing
commits only after the entire map is valid. A failed Job load retains the
previously installed valid map. New-format roundtrip and malformed-input gates
are required before closure.

## Scope boundaries

Translucent nonreciprocity and stackless/reconstructed state limitations remain
separate DL223 concerns. A normal-measure conversion alone does not prove
reciprocity, arbitrary material conservation, or agreement of every estimator.
Dedicated translucent photon records additionally mix incident flux and
Beer-weighted exit-lobe packets; their producer-to-gather proof and correction
are still in progress.
