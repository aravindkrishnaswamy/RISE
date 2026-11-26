# HYLIOS forward transport with individual particles and complexes

`HyLIoSForwardTransport` is an experimental forward kernel with explicitly selected placement and post-scatter policies and explicit unresolved outcomes. The independent `HyLIoSSlabTransport` remains the analytic control. There is no renderer integration, reciprocity, adjoint sampler, or full S1–S4 claim. Version 2 adds the explicitly selected finite complex organization described below.

`HyLIoSForwardTransport` accepts exactly six resolved monochromatic layers in
SC/SG/SS/SB/PD/RD order, explicit host/particle coefficients and indices, an
explicit PD fiber encounter rate, estimator/RR settings and an explicit placement
policy and an explicit post-scatter policy; neither has a default. Local launch position is (0,0,0); retain scene translation in adapters.
No spectrum, absorber composition, biological defaults, or hidden RNG is owned.
Particle geometric rates and Eq5 orientations reuse checked optical primitives;
projected hits reuse the geometric sampler. Unversioned complex organization is refused; `ExplicitComplexPackV1` is the selected finite spatial organization described below.

The caller controls each outer boundary decision through the existing slab
encounter/token/decision record. The adapter can select the existing smooth Fresnel helper or the separate
`HyLIoSSurfaceBoundary` forward law using authored surface fold aspect. The kernel
keeps boundary decisions external; the CLI owns the selected law's branch,
proposal, cancellation and budget state. See
[HYLIOS_SURFACE_BOUNDARY.md](HYLIOS_SURFACE_BOUNDARY.md). Internal planar interfaces own smooth Fresnel/Snell once; bottom
reflectance is one with cosine diffuse return. Histories preserve lateral travel.

## Two explicitly named placement interpretations

`FixedEventDiagnosticV1` retains the geometric event at a sampled **entry**
position, resampling orientation/projected hit there. Its centre is
`eventPosition - sampledEntryPoint`. Whole-particle layer containment and true
accepted-predecessor exclusion are mandatory. Candidate rejection does not replace
the predecessor. The process can have no admissible placement, especially near
walls or where predecessor exclusion and wall containment conflict. Exact boundary
contacts and certified sphere downstream-impossibility produce live-weight
`UnplaceableBoundaryCandidate`; generic prolate uncertainty remains resumable.
The caller must budget unfinished work explicitly. No retry cap is absorption.

`NullCollisionPlacementV1` samples one particle geometry per geometric event. A
layer-crossing or definitely overlapping candidate is consumed as a null event,
then the process resumes host memoryless clocks from that event position. It does
not replace the accepted predecessor. This defines a distinct, explicit
interpretation of under-specified publication placement; it makes no claim that
the accepted fraction equals the authored fraction or that it is author code.
`particleCandidates`, `placementAttempts`, `acceptedParticles`, `nullPlacements`,
containment and overlap counters distinguish nominal and realized processes.
An uncertain/unsupported overlap is **always** numerical failure, never a null
collision or retried geometry. The status is retained in `FailureDetail()`.

## Internal particle process and explicit post-scatter law

Entry has no extra dielectric event or refraction. Absorption is evaluated along
each true local chord using the particle coefficient alone; host absorption acts
on host distance alone. At surviving exit, one melanin-to-host Fresnel test occurs.
Internal reflection receives a cosine perturbation about the **specular reflected
direction**, conditioned inward. Each rejected proposal costs one quantum. A new
chord uses the analytically deflated known surface root; no origin epsilon.
After individual transmission or nonempty-complex envelope exit, the named
1400 nm endpoint exponential forward law is applied once. At or above 1400 nm it is an exact forward delta and consumes no angular draws.

A post-exit angular perturbation can point back into its particle. The constructor
requires one of two policies; neither conditions or resamples the final angular
law or adds absorption at the angular event:

- `ForwardPostScatterPolicy::DiagnosticRetainParticleV1`
  (`hylios_diagnostic_retain_particle_v1`) retains the drawn direction, all
  particle state and live weight, reporting `ForwardPerturbationReentry`.
- `ForwardPostScatterPolicy::DiscardAfterScatterV1`
  (`hylios_discard_after_scatter_v1`) is the user-selected clean-room virtual
  particle rule. After scattering, the active particle is discarded and travel
  resumes through the host, even in the former particle interior. Only the
  accepted predecessor's shape and relative centre survive, for exclusion of
  later proposed placements. No new entry Fresnel, absorption chord, outward
  conditioning, or extra angular draw is inserted. The zero-angle endpoint also
  discards the active particle without consuming angular draws.

`inwardPostExitEvents` records accepted inward directions under **both** policies,
including completed discard histories; it is distinct from the diagnostic-only
unresolved `ForwardPerturbationReentry` termination. The selected discard rule
completes this previously unresolved step for the individual-particle model; it
is not a persistent spatial particle medium or a claim of author-code equivalence.
Separate bounded measurements must still report physical absorption, escaped power
and every unresolved outcome before drawing conclusions about a specimen.

PD fiber state resets only at actual transmitted exit/re-entry. A Fresnel
reflection within PD cannot reset eligibility. All random inputs come from the
provided variable-budget ISampler. Complete segments and individual proposals are
explicit phases. `Advance` quantum is a latency yield; `StopForBudget` and `Cancel`
are separate unresolved outcomes retaining current weight. Neither is absorption.
Analog absorption and weighted Beer/RR use separate host/particle tallies.

## Validation surfaces

`tests/HyLIoSForwardTransportTest.cpp` exercises independent slab and spherical-chord controls, analog/weighted/roulette means, host versus particle absorption ownership, fiber-pass state, explicit placement policies, quantum/RNG equivalence and unresolved accounting. `tests/HyLIoSComplexForwardTransportTest.cpp` exercises real-layout host/child chords, revisits, boundary ownership, construction work and the empty-complex limit. `tests/HyLIoSForwardFailureTest.cpp` supplies a deterministic overlap fault provider to verify propagation of indeterminate, invalid and unsupported predicates. These mechanics tests do not establish author-model equivalence.

Particle geometry, overlap predicates and complex layout require strict floating-point compilation even when the transport and caller use fast math. Their separate full-LTO checks preserve this distinction.


### Independent controls and accounting failure regression

For a matched-index, nonabsorbing sphere at its first encounter, uniform projected
hits give `P(reentry | theta) = (1-cos(theta))/2`. Integrating this geometric
probability against the normalized polar law `exp(-theta/t)*sin(theta)` gives

`P(reentry) = [1 - (1+t^2)/(1+4*t^2) * tanh(pi/(2*t))]/2`.

At `t=5 degrees` the exact probability is approximately `0.0110855`. The fixed-seed
100,000-history test observes 1,126 reentries (`0.01126`, binomial SE `0.000331098`)
with no numerical or placement failures. It checks the independent integral within
five standard errors and preserves live power at every inward event. Both explicit policies independently
observe the same 1,126 inward events with the same seed. Diagnostic reports those
as unresolved; discard resumes host travel without changing the sampled angular
law. This
is a single-particle analytic control; it is not a full skin reflectance estimate.

Exact scripted controls force an accepted polar angle greater than `pi/2` at a
central sphere exit. They assert the unchanged full angle, four angular draws,
and one inward counter event under either policy. Under discard, two subsequent
known steps inside the former sphere use exact host distances and host Beer loss;
particle path length and absorption remain unchanged. Two overlapping proposals
are rejected against the retained accepted predecessor. The 1400 nm discard delta
uses zero angular draws. Invalid explicit policy values are refused. The targeted
control is available as `--discard-only`.

A separate combined control uses absorbing host and spheres with mismatched
particle/host indices, including repeated internal reflected chords. Its 12,000
independent analog and weighted histories compare escaped power, host absorption
and particle absorption separately, each within five combined standard errors.
It reports sample counts, standard errors and reflection counts. The fixed-seed
strict run records 22,424 analog and 163,512 weighted particle reflections, with
4,273 and 9,341 histories respectively containing repeated reflections, and no
failed histories. A second 12,000-history control at 550 nm with the discard policy
compares the same three means, requires completed inward events and repeated
internal reflections, and refuses failed histories. The quantum/RNG checks cover
the cross product of both placement and both post-scatter policies, including
absorption tallies and inward counters. This complements the exact central-sphere Beer/chord tests.

The `hylios_absorbed_tally_overflow` regression builds a finite preexisting host
absorption tally, promotes live power with a valid caller boundary proposal, then
samples complete absorption on a tangent. Overflow of `tally + liveWeight` must
produce `NumericalFailure` while preserving both finite prior tally and live
unresolved weight. The same guarded `Flight` branch serves host and organelle
analog absorption; weighted Beer additions have their own finite guard. The slab
control's analog and weighted additions were independently audited and already
have finite guards. The kernel is monochromatic, so there are no parallel Pel,
NM or HWSS absorption implementations to update. The regression was observed RED
against the pre-fix implementation and GREEN with the guard.

### Standalone verification without a global build

For a focused strict-FP check from the repository root:

```sh
inc=src/Library/Materials/HyLIoS
clang++ -std=c++17 -Wall -Werror -pedantic -O2 \
  -fno-fast-math -ffp-contract=off -I src/Library -I "$inc" \
  "$inc/HyLIoSForwardTransport.cpp" tests/HyLIoSForwardTransportTest.cpp \
  "$inc/HyLIoSParticleOverlap.cpp" "$inc/HyLIoSComplexLayout.cpp" \
  "$inc/HyLIoSOptics.cpp" \
  "$inc/HyLIoSParticleGeometry.cpp" "$inc/HyLIoSSlabTransport.cpp" \
  -o /tmp/hylios-forward-test
/tmp/hylios-forward-test
/tmp/hylios-forward-test --absorption-overflow-only
/tmp/hylios-forward-test --discard-only
```

The sanitizer form uses `-O1 -g -fsanitize=address,undefined` with the same strict
FP flags. For mixed full LTO, compile `HyLIoSParticleGeometry.cpp`,
`HyLIoSParticleOverlap.cpp` and `HyLIoSComplexLayout.cpp` separately with `-O3 -flto=full -fno-fast-math
-ffp-contract=off`. Compile the transport, optics, slab and test caller with
`-O3 -flto=full -ffast-math -fno-finite-math-only`, then link all objects with
`-O3 -flto=full`. Do not put geometry, overlap or layout into the fast-math caller batch. Their strict
compilation guards are part of the tested contract.

## Explicit complex integration (`hylios-forward-explicit-complex-draft-v2`)

`ForwardOrganization::ExplicitComplexPackV1` uses the selected
`hylios_complex_explicit_pack_v1` layout. `envelopeRadiusMetres` and `childCount`
are explicit inputs; the former unversioned `UnsupportedComplexes` organization
remains rejected. `childCount` is the actual number of spatial children, bounded
by 64, rather than an optical interaction cap. Individual aggregate inputs retain
zero defaults for these two unused fields. The outer geometric rate is
`3*volumeFraction/(4*envelopeRadiusMetres)`. The authored fraction is nominal for
the outer placement process, not a certified stationary occupied fraction.

An outer sphere proposal uses two projected-area draws and no sphere orientation
draw. Existing whole-layer containment and predecessor exclusion apply to the
envelope. Accepted envelopes generate their layout resumably, with five draws per
proposal and no physical travel or absorption during construction. All helper
proposal, containment/overlap rejection, restart and pair-check counters survive
in aggregate transport counters, even after layout deletion. The helper's
isotropic axes are generated independently of the incoming ray; child hits do not
resample Eq.(5) orientations or projected points.

Within a completed envelope, the kernel scans all children at each host step,
so nearest-boundary work is O(N) per step, with N<=64; there is no acceleration or
constant-cost claim. Child entries and envelope exit are compared geometrically.
Host gaps own host absorption; true child chords own particle absorption. Entry
has no extra Fresnel. Existing exit Fresnel and conditional reflected cosine
perturbations apply to children. Child transmission resumes the same spatial
layout without a per-child exponential kick. Revisits remain allowed and can
exceed N entries. The final exponential event occurs once at envelope exit, with
no extra envelope Fresnel. An empty layout is exactly host-only and introduces
no final angular kick, including at visible wavelengths.

The sampled envelope entry chord is retained across layout yields and reused for
the first internal host segment, including empty layouts. An exactly coincident
centred child sphere owns a zero-distance entry; its transmitted child exit is the
envelope exit at zero host distance. These rules use exact shape/centre identity,
with no tolerance or new query of an already owned surface.

The last outward-transmitted convex child is excluded only while its host ray is
unchanged; this removes its owned zero surface root without shifting the ray.
A reflected direction clears that exclusion. Other touching children remain
eligible for zero-gap entering intersections. Tangent zero-length chords do not
enter. Stable child array order resolves equal child entries; owned child exits
finish before a coincident envelope exit. Geometry exceptions propagate as live
`NumericalFailure`, never a miss, placement retry, null event or absorption.

The envelope remains the outer predecessor throughout child walks. After the
final kick, discard removes the layout while retaining only that envelope copy
for later outer exclusion. Diagnostic policy retains unresolved state on an
inward final draw. `acceptedParticles` counts accepted outer objects;
`acceptedComplexes` is its complex subset. `complexChildEntries`,
`complexChildExits` and `complexChildReflections` distinguish internal child
optics; `complexExits` counts successful outer exits. Existing `particleExits` and
`particleReflections` count actual melanosome events, including complex children.
`complexHostSegments` counts host-gap flights. `complexLayoutProposals`,
`complexLayoutContainmentRejections`, `complexLayoutOverlapRejections`,
`complexLayoutRestarts` and `complexLayoutPairChecks` expose construction cost.

PD complexes with a nonzero papillary fiber rate are explicitly unsupported and
refused by the constructor. Individual PD particles retain the existing fiber
clock behavior. Current S1/S2/S3 complexes occur in SG/SS/SB. No camera/adjoint
sampler, reciprocity, stationary whole-medium geometry or author-code equivalence
is claimed.

The focused complex test uses real proposal uniforms to make exact empty,
centred-one-child and separated-two-child layouts. Its independent Beer oracle is
`exp(-muHost*hostLength-muChild*childLength)`. A two-child reflection fixture
performs three entries and four complete child diameters, proving N is not an
optical cap. Separate touching-envelope, touching-child and exact-tangent
fixtures verify boundary ownership. A 257-proposal construction fixture witnesses
one full restart and preserves path/power; cancellation and hard budgets retain
live unresolved weight. Unsupported layout predicates and an unsupported FP
environment exercise sticky numerical failure before host travel.

A conditional S3 N5 control uses a=.085um, b=.205um, R=.41um, with 300 histories per
analog/weighted/weighted-RR estimator. It conditions a known accepted envelope
entry and normalizes live weight there to one using the public boundary weight
proposal; it is not a specimen reflectance estimate. It compares escaped, host
absorbed and child absorbed power separately. Its fixed-seed strict run recorded
748125 proposals, 596676 pair checks, 1737 restarts, 1044 child entries and 1084
roulette trials across 900 envelope histories. Quantum tests compare proposal and
optical RNG state, counter totals and absorption tallies under 1/64-work yields.
The visible empty-complex control compares 3000 analog and weighted whole histories
against the independent slab and exercises 295 accepted envelopes.

The nearest-boundary sphere reference retains a near-tangent case that exposed an
existing external-intersection precision defect. Its naive binary64 quadratic
falsely predicted a miss; a 100-digit Decimal polynomial using the exact binary64
input tuple established a real tiny chord. The pre-fix primitive overestimated
that chord by about 22%. The focused entry-distance regression was observed RED
with the original geometry and GREEN with the strict raw-sphere precision repair.
For radius .25, origin `(-0x1.3ffffffffffffp-1,0,-0x1.9520cd1372fe9p+0)` and
direction `(0x1.0000000000001p-1,0,0x1.bb67ae8584cacp-1)`, the independent entry
is `1.6830126965630604843973119973056605864` and the full chord is
`1.0658316972466380597130430092403321767e-8`. With exact decimal conversions
of those binary64 inputs, compute A=d·d, B=o·d, C=o·o-r², D=B²-A*C;
entry=(-B-sqrt(D))/sqrt(A), chord=2*sqrt(D)/sqrt(A). The corresponding lower
sphere's origin z `-0x1.2a419a26e5fd2p-1` gives a strictly negative D and misses.
No input is skipped, treated as a null event or hidden with an origin epsilon.


The measurement adapter exposes this model only when the caller selects
`--complex-layout explicit-pack-v1`. Its report maps resolved
`complex_diameter_m/2` to R and `maximum_internal_count` to actual N, verifies any
declared rate against `3*f/(4*R)`, and retains construction conventions and all
counter groups. See [HYLIOS_MEASUREMENT.md](HYLIOS_MEASUREMENT.md) for CLI scope,
atomic validation and standalone build sources. To run the focused complex
control directly, use the standalone command above with
`tests/HyLIoSComplexForwardTransportTest.cpp` in place of the individual test.


Measurement v4 adds explicit `--boundary hylios2015`, counting each boundary
branch/proposal attempt against the caller cap while preserving separate kernel
transport counters. The smooth diagnostic preserves its legacy ordering and RNG.
Neither selected forward boundary supplies an adjoint or renderer connection PDF.
