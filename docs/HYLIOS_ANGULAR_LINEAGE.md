# HyLIoS observable angular lineage v1

`hylios_angular_lineage_v1` is observational metadata. Physical model versions,
work counters, sampling laws and random draws are unchanged. This is an
**ensemble law classification**, conditional on the incident source direction;
it is not an angular PDF conditional on the realized geometry or a realized RNG
path. Every binary64 implementation has discrete outputs; that fact is distinct
from the mathematical law decomposition represented here.

## State and caller contract

`ForwardState::AngularLineage()` returns a class and the reason for the last
classification-changing accepted event:

- `KnownPlanarMirrorEligible`: only parallel planar dielectric maps and exact
  angular identities have occurred. This is eligibility while live, not returned
  mass. An actual exterior return exposes `PlanarSameExteriorMirror` through
  `ReturnedAtomMap()`; every other outcome exposes `None`.
- `AbsolutelyContinuous`: an angular density was proved for the ensemble and all
  subsequent directional transforms have a supported preservation proof.
- `UnclassifiedSingular`: the decomposition is unsupported, with possible
  singular mass. It does not assert that every such history is mathematically
  singular. It can contain continuous or mixed response. A future bake must
  refuse its positive returned mass rather than silently histogram it as finite f.

No classification uses rounded direction equality, a tolerance, terminal type,
random-draw count or geometry counters. Copy/yield/zero quantum, budget,
cancellation and failed/rejected proposals preserve the current classification.
Nonreturn outcomes never expose returned atomic mass, even if eligibility remains.
There is no bake, local response table, finite density evaluator, adjoint,
reciprocity assertion, or claim of a production-complete SWIR response here.

`SlabBoundaryDecision` appends a law annotation defaulting to `Unclassified`.
The helper stamps `PlanarDielectric`. The fold sampler stamps that law for macro
transmission and exact zero-fold reflection; only a Ready accepted nonzero-fold
reflection stamps `IndependentFoldReflection`. Pending/rejected normal proposals
expose no decision. The annotation is a caller law contract, like the existing
responsibility for physical boundary proposals; it is not verified by comparing
direction vectors. Untyped arbitrary caller decisions invalidate prior proof.
An independent-fold annotation attached to a transmission is treated as unknown.

## Supported transitions and proofs

Travel, absorption, compensated roulette, placements/rejections and complex layout
construction leave direction unchanged. Conditioning on survival or acceptance
multiplies an existing probability measure by a measurable bounded event/weight;
it cannot create positive angular mass on an angular null set.

Parallel planar dielectric transforms preserve tangential optical momentum.
Reflections reverse normal travel. A return to the same exterior, before any
angular randomization, therefore has the same planar mirror map despite internal
planar reflections. On AC input, planar reflection and noncritical Snell
transmission are smooth local diffeomorphisms. Critical and horizon boundaries
have zero angular measure. Exact matched particle/host real indices are an
identity law, as is the final exponential law's exact zero width at >=1400 nm.
Random geometry or layout draws alone do not destroy planar eligibility.

A successful bottom cosine return, accepted fiber Rayleigh law, accepted inward
reflected-cosine proposal, accepted positive-width exponential proposal, or
accepted positive-fold reflection refreshes AC, regardless of prior class.
For each fixed past history these laws have a 2D density. Polar laws include
independent uniform azimuth; the cosine restriction to the inward particle
hemisphere is rejection conditioning on positive probability. Nonzero folds
sample a continuous normal law; reflection has normal-to-direction Jacobian
`4*abs(d dot n)`, positive on the strictly facing accepted domain. Rejections
leave the selected Fresnel branch and ForwardState classification unchanged.
The mark is applied only after successful direction/chord construction.

### First independently projected individual-particle chord

Condition on the incoming history and the chosen ellipsoid orientation. Sampling
projected area supplies a 2D absolutely continuous impact point. Mapping the
impact point to its far exit surface is a smooth bijection away from the
silhouette, with area projection Jacobian `d dot n > 0`. A strictly convex
ellipsoid has positive Gaussian curvature, so its Gauss map to exit normals is
locally nonsingular. Thus the **ensemble** exit normal is AC conditional on that
past history and orientation. Containment, predecessor rejection, Beer survival
and the Fresnel branch condition only reweight this law. Orientation mixtures
retain AC. Individual samples at the central chord remain classified by this
law; their particular vector or particular sampled normal is not the proof.

For reflection, the normal-to-direction Jacobian above is positive away from
grazing. For unmatched positive indices, put eta=n_particle/n_host, c=d dot n>0,
ct=sqrt(1-eta^2*(1-c^2)), beta=ct-eta*c and t=eta*d+beta*n. On noncritical
transmission beta cannot be zero unless eta=1. For tangent variation dn,

`dt=beta*dn + beta'(c)*(d dot dn)*n`.

The two terms are orthogonal and beta!=0, so the map has rank two. The
transmitted ensemble direction is therefore AC. The transmitted critical set
and the projected silhouette have zero measure. Matched indices use the exact
identity rule instead, including a zero final kick.

An explicit `independentProjectedChord` proof flag is set only on a newly
accepted individual projected-area chord. It is consumed on that chord's exit
Fresnel event and cleared for retained chords and complex children. It records
which mathematical conditional sampling argument applies; it is not inferred
from counter values or the number of children visited.

### Deliberately unsupported coupled curved maps

After an internal reflected-cosine event the direction is AC, but the later
exit point and normal depend jointly on that direction and retained geometry.
The fixed-incoming-normal proof above does not establish the rank of that
coupled map. An unmatched exit or a retained curved reflection consequently
marks `UnclassifiedSingular`; a later accepted independent angular law can
refresh AC. Exact matched-index transmission still preserves its prior class.

Complex child exit normals depend on retained spatial layouts and traversal.
This increment does not claim their projected-normal independence. Unmatched
complex-child refractions and reflections are conservatively unclassified,
including the exactly coincident full-sphere child/envelope topology. The
complex envelope adds **no Snell interface**: it is geometric bookkeeping, then
one final exponential law for a nonempty complex. An empty complex has no child
optical event and no exponential kick, at any wavelength.

These conservative dispositions may leave positive unknown return mass at the
zero-width SWIR endpoint. Production baking remains blocked for that mass until
a separate coupled-map proof or explicit supported singular representation is
implemented. A histogram alone cannot discharge this limitation.

## Direction frame

The oracle uses depth +Z and outward normal -Z; kernel XY azimuth is legacy
measurement metadata. A right-handed local outward frame uses basis
(kernel +X, kernel -Y, kernel -Z). Local source direction wi launches kernel
travel `(-wi.x,+wi.y,+wi.z)`; escaped kernel d maps to local
`wo=(d.x,-d.y,-d.z)`. In that frame the mirror map is
`S(wi)=(-wi.x,-wi.y,+wi.z)`, with unit solid-angle and projected-angle Jacobians.
The focused test uses nonzero polar and azimuth components to detect the Y sign.
No local response bridge or incident-jitter averaging is added in this increment.

## Verification

The focused test exercises law seams, exact identity limits, intermediate first
reflection/cosine rejection/acceptance, unknown retained and complex maps,
independent refreshes, diffuse-bottom and fiber laws, arbitrary untyped boundary
proposals, zero returns, stop/failure states, and copied/resumed quantum partitions.
A separate source-linked baseline comparison serializes and compares raw bytes of
all physical public state,
all counters/path/absorption channels, pending boundary data, and complete RNG
state across 768 histories and 17,974 intermediate/terminal observations. Baseline
and annotated builds use the same compiler mode; classification metadata is
excluded from that physical trace; its diagnostic hash is also recorded. Geometry, overlap, layout and surface boundary
remain strict floating point in mixed full-LTO validation.
