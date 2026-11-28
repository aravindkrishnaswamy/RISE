# HyLIoS observable angular lineage v2

`hylios_angular_lineage_v2` is observational metadata. Physical model versions,
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

### Coupled position and direction support

`ForwardState::AngularSupport()` carries sufficient ensemble certificates in
an individual-body or fixed-complex-layout frame: boundary phase density P4,
boundary position area density Q2, a fresh cosine point-source chord Fan, and
an untouched projected complex Beam. They are proof state, not empirical ranks.
The complete derivation and implemented transitions are recorded in
[the coupled proof](HYLIOS_COUPLED_LINEAGE_PROOF.md) and
[the transition ledger](HYLIOS_COUPLED_LINEAGE_TRANSITIONS.md).

Fresh projected impact and incoming angular AC establish joint P4 after
marginalizing the impact-dependent world centre. A fresh individual projected
chord supplies Q2; the untouched complex beam supplies it at its first child.
A valid reflected cosine with Q2 establishes P4, which subsequent autonomous
flights and noncritical optical maps preserve. Without Q2, a fresh cosine
supplies Fan and angular AC; its same-body chord supplies Q2 at the far point.
Sphere Fan and convex low-index Fan transmission have the narrower proved
angular-preservation domains detailed in the proof. Matched-index transmission
remains an exact identity law.

Accepted geometry and successful optical events acquire support; rejected or
failed proposals do not. Body completion/discard clears frame support while
preserving the public angular class. An unresolved diagnostic inward reentry
retains its last support, without claiming returned density. Physical transport
branches, random draws, sampled directions, weights and work counts are unchanged.

### Remaining unsupported coupled maps

Atom-entered multi-child transmission chains without a sufficient certificate
remain `UnclassifiedSingular`. Angular AC alone does not justify the next coupled
child map. Positive returned unknown mass must refuse production baking; a
histogram cannot supply the missing proof. The complex envelope has no Snell
interface; it is geometric bookkeeping followed by the configured final kick
for a nonempty complex. Empty complexes have no child event or final kick.

Retained individual returns and classified zero/one-child complexes now have
supported proofs, including the coincident full-sphere child/envelope case.
This does not establish a production-complete SWIR response, reciprocity, a
microscopic adjoint, or spatial nonlocal transport.

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
reflection/cosine rejection/acceptance, certified retained and unknown multi-child maps,
independent refreshes, diffuse-bottom and fiber laws, arbitrary untyped boundary
proposals, zero returns, stop/failure states, and copied/resumed quantum partitions.
A separate source-linked baseline comparison serializes and compares raw bytes of
all physical public state,
all counters/path/absorption channels, pending boundary data, and complete RNG
state across 768 histories and 17,974 intermediate/terminal observations. Baseline
and annotated builds use the same compiler mode; classification metadata is
excluded from that physical trace; its diagnostic hash is also recorded. Geometry, overlap, layout and surface boundary
remain strict floating point in mixed full-LTO validation.

V2 adds `HyLIoSCoupledAngularLineageTest`: together the focused controls cover
5,851 assertions, including all 398 quantum compositions of selected scripted
trajectories. Strict, ASan/UBSan and mixed full-LTO private gates passed. An
independent baseline comparison matched all 11,062,496 physical/RNG trace bytes
within each build mode across 768 histories and 17,974 observed states. This
establishes observational parity for those controls, not biological validation.
