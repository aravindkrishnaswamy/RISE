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
and publishes total distance to the accepted hit. Coverage evaluation itself
sees that original ray and total distance, not the shortened recast context.
Differential origins are relative offsets: on a null continuation, each auxiliary
origin offset gains its direction offset times the accumulated distance. This
preserves the original auxiliary ray lines and the receiver texture footprint. It consumes neither a bounce
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
live Job cannot switch SMS mode. The census starts at visible scene roots and
recurses through material-inheriting CSG operands (including nested snapshots).
An explicit material at any level ends inheritance, so an opaque override
suppresses child alpha; hidden unused objects are not roots. For an alpha scene, both requested Snell/biased
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
SSS endpoint receives coverage from its actual intersected material, captured
before shading modifiers. CSG root or entry optical-kernel material is not a
substitute for that effective endpoint binding. The random-walk front-face
fallback obeys the same rule; accepted endpoint probability is forwarded to
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

`AlphaRayContextTest` compares pinhole and nonzero-origin differential rays to a
checker-textured sphere with zero, one and two MASK0 sheets. It pins full world
filter width, the entire UV Jacobian, published ray context, and a scalar
coverage query that depends on original ray origin and total distance.

## Emission budgets and bounded legacy photon maps

VCM counts each independent light-emission attempt, including an alpha-rejected
endpoint or empty path, in both initial and rebuilt stores. Branches and deposits
are not independent emissions. `AlphaEmitterNormalizationTest` exercises the
actual rasterizer with an explicit nonzero merge radius in VM-only, VC-only,
and combined RGB/NM/HWSS modes.

For legacy photon tracing, `numPhotons` now means the number of attempted
emissions and the maximum stored packet count. It no longer means a per-light
stored quota retried until full. Consequently a map may contain fewer packets;
increasing the budget increases work predictably, including for fully masked
emitters and scenes that deposit nothing. This changes opaque multi-light maps
too: the old fixed stored quotas and shared shot divisor underweighted sources
and coupled each light to other lights' deposition efficiency.

At each attempt choose source i with unmasked power probability q_i, and divide
its emitted packet by q_i. The existing conditional emitter-direction law is
retained; nonmesh RGB lights additionally retain their directional PDF divisor.
Sampled alpha rejects contribute zero but still count in the final 1/N average.
Thus source selection contributes q_i * (power_i/q_i) * alpha_i, without
renormalizing away coverage or changing another source's power. Spectral legacy
tracing continues to support mesh emitters; its source importance uses unmasked
RGB exitance while packet power uses the sampled wavelength. This does not add
spectral nonmesh emission support. Source lookup remains linear in the number of lights per attempt. The legacy
mesh packet law still uses average exitance and its existing directional
sampling, including preexisting textured-emission limitations; this change
does not claim a new texture-importance emission estimator.

Constant and spatial coverage use the same
accounting; no alpha-average compensation is estimated.

A path may deposit more than once. All eligible deposits enter a uniform
reservoir: after M deposits, retain K=min(M, capacity) packets. Each survives
with probability K/M, so multiply retained packet powers by M/K before the final
1/N. Continuation never stops when storage fills. The reservoir uses its own
fixed-seed 64-bit engine and unbiased integer selection, independent of transport
and wavelength RNGs; the 64-bit deposit counter rejects overflow. It is enabled
only for a fresh shooting map and disabled before publication. Manual stores
and successfully deserialized maps retain ordinary bounded append semantics;
no reservoir state is serialized. Flux corrections are applied once by the
emission driver, not implicitly by `ScalePhotonPower`.

RGB global, caustic and translucent maps, and NM global/caustic maps share this
rule. Shadow maps are a separate heuristic categorical presence cache: uniform
reservoir sampling preserves a representative set of lit/shadow labels, but
neither labels nor their automatic gather-radius metadata receive M/K. Their
historical 1/N radius scaling remains. A shadow cache is not an unbiased flux
estimator. Existing finite-radius density-estimation and cache approximations
remain; uniform packet retention does not remove them.

Progress batches distribute every integer remainder. For T equal-width time
strata, use N_t attempts in stratum t and packet weight N/(T*N_t), giving final
weight 1/(T*N_t). T is capped at N so no nonempty time stratum receives zero
attempts. Source probabilities are rebuilt after animation changes. Cancellation
releases the partial map and returns failure without publishing it.

`AlphaPhotonEmissionTest` exercises the real emission and publication loops
with ideal deposit sinks: absolute source power, independently masked sources,
spatial alpha with unequal deposit efficiency, multiple deposits per attempt,
capacity overflow, progress/time remainders, cancellation, RGB/NM global/caustic
and RGB translucent maps, and mixed mesh/point lights. Empty-source, all-alpha-zero and N=0 shoots
publish valid empty maps without dividing by zero. Existing transport suites
separately test actual photon continuation and gather responses.

## Shadow-disabled medium queries

Direct lighting uses scene-local alpha presence and distinguishes unavailable
boundary records from an authoritative empty list. A visibility-enabled ray
reuses the boundaries accepted by its shadow query. With `receives_shadows FALSE`,
a full-segment sampled boundary query ignores ordinary blockers, accepts alpha
only at actual medium boundaries, and supplies those records to attenuation.
Every ray gets fresh records, including successive deterministic directional
lights. `AlphaShadowMediumTest` covers RGB/NM/HWSS, unused local alpha and an
unrelated live Job, both shadow flags, accepted/rejected alpha boundaries, and
ordinary blockers that must not stop medium traversal, mesh/environment rays,
and successive directional lights with different boundary segments.

## Physical medium segments and endpoint exclusions

A shadow query has an occlusion interval and a physical medium interval on the
same original ray. LightSampler retains its finite light self-hit exclusion,
but accepted medium records extend to the actual light endpoint. BDPT/VCM
collect over the original connection, including both endpoint exclusion regions;
only occlusion ignores those regions. Record selection uses prepared scene-local
alpha presence, so another live Job cannot change the medium estimator.

`RayIntersection` retains optional raw geometric entry/exit parameters in the
caller's ray-distance units. Object captures them before its published shading
backoff; CSG carries the chosen operand's entry or exit parameter and converts
units once at each transform. Shading points and ranges keep their existing
contract. Physical interval membership, recorded boundary positions and recast
progress use the raw parameters, avoiding a second alpha decision caused by a
backed-off point. Exact physical endpoint events are excluded; the immediately
next representable endpoint includes a real preceding boundary. Unknown external
object implementations without this metadata retain their published-point
fallback. Geometry solver accuracy and self-root policies still apply.

DL-05 sampled interface walks resume with the original ray, raster/footprint
inputs and total hit range. Each accepted event supplies both visibility and
medium state; there is no independent alpha replay. Authoritative records do
not use a recast epsilon, which could otherwise hide additional boundary events
inside the last epsilon-sized interval. The older geometric walker already
integrated a lone final active-medium remainder after its loop; that was not the
finite-tail defect. Known starting/global media now integrate positive short
connection lengths even below the old whole-connection early-return threshold,
with or without records. Ordinary geometric boundary discovery retains its
existing recast precision limitations; this is not a general geometry overhaul.

`AlphaMediumTailTest` renders PT/BDPT/VCM RGB/NM/HWSS against an exp(-1)
endpoint-medium oracle and checks unrelated-job isolation.
`AlphaMediumBoundaryTest` checks leading/trailing records, draw counts, short
connections and multiple events inside the final recast interval.
`AlphaBoundaryEndpointTest` covers exact and adjacent-representable endpoints,
nonuniform transforms, nested CSG, subtraction/intersection, front/back,
snapshot objects, record copies and unchanged shading backoff.

## Snapshot alpha ownership

Every material reconstructed by `CloneMaterialForSnapshot` copies its root
alpha mode, cutoff and scalar-painter binding. Scalar painters follow the
existing reference-counted sharing policy, while reconstructed materials retain
independent slot bindings. A source rebind or destruction therefore does not
remove the snapshot's alpha. Existing baked/wrapped-material fallback still
shares the material itself; its documented slot-mutation limitations remain.
Nested wrappers retain root alpha rather than substituting a child's coverage.
`AlphaSnapshotTest` pins these ownership and default-opaque behaviors.

R2 composite/endpoint regression coverage: `AlphaCSGCapabilityTest` checks
inherited, nested, snapshot, root/intermediate override and hidden/foreign-scene
controls; `AlphaCSGTransportTest` renders PT/BDPT/VCM in RGB/NM/HWSS against
matched zero-absorption and explicit-root controls. `AlphaSMSTransportTest`
also checks inherited-CSG mode selection. `AlphaRandomWalkMaterialTest` pins
actual endpoint material, pre-modifier UVs, fallback hits, exact final alpha draw
count, unchanged proposal queries/PDF/weights and preserved shading modifiers.
`AlphaBSSRDFMaterialTest` independently identifies mixed-material CSG endpoints
and checks their coverage, pre-modifier context, unchanged chord counts and
retained opaque-endpoint weights in RGB/NM.

SSS coverage queries additionally copy the selected probe geometry and forward
`ri.rast`, `ri.signals.pScene`, the root `pObject` as `pSelf`, and the actual
pre-modifier endpoint as `ptWorld`. Only the alpha query receives this context;
the raw hit used by existing modifiers and the optical proposal is unchanged.
Opaque mode skips the copy. The probe's own ray/distance and local geometry
remain its own, so this does not assert reciprocity for arbitrary ray-dependent
coverage programs. There is no per-ray time field to forward, and scalar
expression time retains its documented fixed-zero behavior.
`AlphaSubsurfaceContextTest` checks the real `1-interior(2)` expression against
an enclosing-sphere closed form and nonzero pixel coordinates in RGB/NM for
both kernels; `BSSRDFEntrySignalsTest` checks existing downstream signal payloads.

All five manual emitter coverage sites (light-root sampling, RGB/NM mesh NEE,
RGB/NM photon emission) use `LightSampler::AcceptEmitterAlpha`. Its alpha-only
copy stamps the actual published sampled point, scene manager and luminary
identity regardless of the optional signal-payload probe gate. NEE additionally
forwards the receiver's raster coordinates. Light roots and photons have no
camera pixel, so their raster context remains the existing null state. This
changes only coverage evaluation: emitted-radiance records, photon proposals,
attempt normalization and reservoir accounting are preserved. A single-sided
clipped-plane sample includes its geometry's documented normal offset; tests
obtain their expected point from `UniformRandomPoint` rather than introducing
a larger comparison tolerance. Optional local signal payloads retain the
existing `ProbeEmitterSurface` acceptance/refusal policy.

SMS final coverage consumes the immutable pre-modifier hit that actually
produced the solved endpoint, including its actual inherited CSG material.
A separate fixed-distance normal probe can skip an SDF face or select a nearby
CSG face, so it is not an endpoint certificate. Seed hits and actual Newton
projection hits retain records only in scenes with effective alpha; FD-only
verification cannot replace a retained endpoint with its neighboring hit.
The exact published position and root object key the record. Copies and
rejected-step rollback share it immutably; changed positions/objects cannot
use it, and analytical smoothing invalidates it. A missing record during
geometric initialization publishes an actual hit before Newton evaluates the
vertex. An alpha Newton step with no successful geometric snap is rejected
instead of publishing a linear, unverified endpoint. Existing optical seed
metadata and geometry/projection rules remain otherwise unchanged.

The final alpha-only copy uses the previous physical segment, attached scene,
root object and solved world point. No receiver raster is available in this
API, so none is invented. Final acceptance occurs once outside proposal
normalization. Visibility subqueries are bounded before sampling coverage,
so surfaces beyond their segment cannot consume endpoint draws. All production
alpha main/reciprocal proposals use the same BuildSeedChain/Solve pipeline;
the existing alpha override excludes biased branched/photon extensions in RGB
and NM. Direct internal visibility calls with fabricated or stale uncached
vertices conservatively fail; they cannot move an endpoint to fabricate proof.
This is a stricter internal input contract, not unchanged manual-call behavior.
No-alpha solves allocate no endpoint payload. Alpha solves add immutable hit
copies/allocations at seed/projection/validation queries; chain copies share
records. Borrowed object/material pointers have the same owning-scene lifetime
requirement as existing manifold vertices. This records the solver's actual
geometry result, not exact mathematical surface coordinates, and does not
remove its existing finite-precision or projection-support limits.

`AlphaSMSGeometryTest` covers analytic and SDF mirrors at scales 0.1/1/10,
nonuniform transforms, actual two-stage ellipsoid calls, modifier/UV behavior,
nearby disconnected CSG faces, remote MASK1 invariance, actual MASK1/MASK0,
one BLEND draw, immutable copies/rollback, and missing/stale records.
`AlphaEndpointContextTest` covers real scalar world/scene expressions, direct
emitter/NEE contexts, unchanged MASK proposal draws, RGB/NM/HWSS rendering and
production `BuildSeedChain`→`Solve` crossing between equally reflective CSG
materials with different alpha. `AlphaPhotonEmissionTest` adds the five actual
emission-loop context controls; `SignalEmitterRecordTest` guards unchanged Le
and optional surface payload behavior.
