# Scalar material coverage (DL-214, 2026-09-29)

`IJob::SetMaterialAlpha(material, scalar, mode, cutoff)` is appended to the
construction API. Every material defaults to OPAQUE and accepts the common
scene parameters `alpha_coverage`, `alpha_mode`, and `alpha_cutoff`. The name
`alpha_coverage` avoids changing Ward roughness and hair tilt parameters named
`alpha`. Coverage reads the scalar painter's single-sample channel; it never
passes through RGB-to-spectrum conversion. Negative/NaN values become zero,
values above one clamp to one. MASK accepts at or above cutoff; BLEND accepts
with probability a. Zero/one do not draw random numbers. OPAQUE ignores alpha.

## Traversal and compatibility

`IObjectManager::IntersectRaySampled` is shared transport traversal above object
intersection. Raw object/geometry intersections remain geometric. A rejected
hit advances along the same ray, preserves cast inputs and the original ray,
and publishes total distance to the accepted hit. It consumes neither a bounce
nor a medium/IOR boundary transition. Finite visibility segments retain their
original endpoint. Accepted hits stamp their exact coverage probability;
photon deposits must use that stamp rather than evaluate a painter again after
modifiers or vertex reconstruction.

Camera, continuation, light and photon walks use the sampled traversal. NEE,
BDPT/VCM connection visibility and SMS visibility have explicit sampler
arguments. Legacy shader rendering and AO use independent draws when no sampler
is carried in the runtime context. Existing raw interfaces remain available
for geometric queries and compatibility. glTF and Blender producers install
material coverage, without alpha shader ops. Blender preserves its historical
`<material>.shader` name with ordinary emission/direct ops. Explicit user-authored
`alpha_test_shaderop`/`transparency_shaderop` chains retain legacy semantics;
combining one with material coverage deliberately applies two separate effects.

Medium visibility optionally records accepted interior-medium boundaries,
including non-shadow-casting objects. Records retain world points and geometric
side. The attenuation consumer orders/project these points on its original
connection ray, including reversed connections and DL-05 repeated subcasts.
There is no replay, second alpha draw, or update from a rejected boundary.
The vector allocates only when a medium boundary is recorded. Opaque-only
queries keep the original fast path. DL-05 restrictions on non-alpha refractive
visibility are unchanged.

## Sampling, endpoints, and MIS

Sobol/ZSobol reserve dimensions [2^29,2^30) for variable alpha work. The counter
starts at construction, not `StartStream`; normal fixed-budget streams do not
consume or reset it. Current supported transport streams end below this range.
The explicit upper-bound guard prevents wraparound. Independent samplers draw
the next independent variate. PSSMLT uses sparse stream 3072, separate from
normal/path/film lanes, with an index reset only at `StartIteration`. Alpha
access saves/restores the normal cursor. Accept/Reject therefore mutates or
restores alpha primary samples together with the path. SMS tail roulette also
uses this variable lane. Test-only/composite hash samplers inherit the ordinary
independent-lane default when they do not own transport stream budgets.

A sampled emitter endpoint is accepted exactly once in emission/NEE sampling.
Emission evaluation on an already accepted path vertex does not draw again.
The unconditional opaque proposal densities remain the common MIS partition:
for any full path, each strategy's accepted transport numerator contains the
same product of physical endpoint coverage and visibility factors. Normalized
opaque-proposal weights still sum to one on that path, although they are not
the variance-optimal heuristic for the thinned proposal. No local alpha factors
are inserted into only one direction of the VCM recurrence.

Photon density estimation samples both an eye receiver at x and a deposited
photon at y. Their acceptance probability contains a(x)a(y), while the desired
incident-flux gather conditional on the eye surface contains a(x). Divide the
stored incident packet by its stamped a(y). Continuing photon throughput is
unchanged. The same deposition-only correction applies to VCM's light-vertex
store; VC uses the unmodified light subpath. Thus VM-only, VC-only, and combined
strategies evaluate the same coverage-weighted target and retain a common
normalized weight partition. This argument is exact for constant coverage;
spatial coverage retains ordinary finite-radius density-estimation boundary
bias. It does not claim exact unbiasedness at arbitrary texture discontinuities
with a finite merge radius.

## SMS proposal normalization

The prepared LightSampler records alpha presence for its own scene. An unrelated
live Job cannot switch SMS mode. For an alpha scene, both requested Snell/biased
and uniform settings use uniform-caster Bernoulli proposal normalization. Seed
tracing still samples alpha, allowing discovery behind holes. Its success
probability p includes that proposal thinning. Final solved-chain surface
coverage is sampled at the validated solved point outside the reciprocal loop,
so the physical coverage factor appears once after proposal normalization.
Geometry projections/Newton derivatives remain geometric.

For alpha scenes the old hard-cap discard would darken rare proposals. Let
B be the configured Bernoulli budget (1024 when zero). The reciprocal estimate
starts with term 1. Each failed trial adds the next term; after failure n>=B,
continue with q_n=(n/(n+1))^2. On survival add ((n+1)/B)^2; on termination return
the accumulated prefix. Survival telescopes to (B/(n+1))^2, cancelling the term
weight, so E[estimate]=sum(k>=0)(1-p)^k=1/p. Expected trial work is bounded by
about 2B even as p approaches zero. For p>0, exponential failure decay dominates
polynomial weights, giving finite variance. Variance can nevertheless be large
when pB is small. This removes the alpha-induced cap bias; it does not remove
existing solver/geometric clamps or change the opaque-scene capped algorithm.

## Geometric-only queries

Acceleration structures, CSG operands, object intersections, containment/IOR
initial seeding, emitter surface payload probes, Newton projection/derivatives,
and final-gather gradient classification query geometry rather than sample a
new transport segment. The final-gather probe classifies the already traced
sample and does not add radiance. BSSRDF chord candidates and random-walk SSS
boundaries likewise define a geometric nonlocal-kernel proposal. They remain
unthinned, with original candidate count/spatial PDF. Only the selected physical
SSS endpoint receives coverage; accepted endpoint probability is forwarded to
BDPT. This multiplies the opaque-domain SSS kernel by endpoint coverage; it does
not simulate a physically perforated scattering volume or change every internal
Fresnel event. Actual scene rays still null-pass rejected surfaces normally.

## Regression coverage

- `AlphaIntersectionTransportTest`: finite controls, MASK/BLEND shadows under
  PT/BDPT/VCM/MLT, sampled emissive endpoints, NM/HWSS, delta continuation with
  absorbing medium, and active VM-only/VC-only/combined receiver coverage.
- `AlphaMediumBoundaryTest`: one decision per accepted/rejected nested boundary,
  original/reversed segment ordering, finite endpoint, RGB/NM attenuation.
- `AlphaSamplerLaneTest`: normal-lane isolation, repeated stream changes,
  PSSMLT mutation reset/rollback, thousands of distinct alpha dimensions.
- `AlphaSMSReciprocalTest`, `AlphaSMSTransportTest`: reciprocal expectation and
  cost at small p, actual mirror transport, requested-mode equivalence, RGB/NM,
  and unrelated-live-material isolation.
- `AlphaSubsurfaceEndpointTest`: paired geometric proposals and endpoint coverage.
- `GLTFAlphaImportTest`, `BlenderBridgeAlphaTest`: real textured glTF alpha,
  cutoff/default/OPAQUE behavior and bridge legacy/modern cutout rendering.
- `LegacyPhotonTransportTest`: accepted global/caustic RGB/NM deposited incident
  flux retains one receiver-coverage factor.
