# HYLIOS individual-spheroid forward transport

`HyLIoSForwardTransport` is an experimental forward kernel with explicitly selected placement and post-scatter policies and explicit unresolved outcomes. The independent `HyLIoSSlabTransport` remains the analytic control. There is no renderer integration, complex support, reciprocity, adjoint sampler, or full S1–S4 claim.

`HyLIoSForwardTransport` accepts exactly six resolved monochromatic layers in
SC/SG/SS/SB/PD/RD order, explicit host/particle coefficients and indices, an
explicit PD fiber encounter rate, estimator/RR settings and an explicit placement
policy and an explicit post-scatter policy; neither has a default. Local launch position is (0,0,0); retain scene translation in adapters.
No spectrum, absorber composition, biological defaults, or hidden RNG is owned.
Particle geometric rates and Eq5 orientations reuse checked optical primitives;
projected hits reuse the geometric sampler. Complex organization is refused.

The caller controls each outer boundary decision through the existing slab
encounter/token/decision record. The supplied smooth Fresnel helper can be used
for forward measurement. This kernel neither implements nor claims publication
surface folds. Internal planar interfaces own smooth Fresnel/Snell once; bottom
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
After transmission, the named 1400 nm endpoint exponential forward law is applied
once. At or above 1400 nm it is an exact forward delta and consumes no angular draws.

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

`tests/HyLIoSForwardTransportTest.cpp` exercises independent slab and spherical-chord controls, analog/weighted/roulette means, host versus particle absorption ownership, fiber-pass state, explicit placement policies, quantum/RNG equivalence and unresolved accounting. `tests/HyLIoSForwardFailureTest.cpp` supplies a deterministic overlap fault provider to verify propagation of indeterminate, invalid and unsupported predicates. These mechanics tests do not establish author-model equivalence.

Particle geometry and overlap predicates require strict floating-point compilation even when the transport and caller use fast math. Their separate full-LTO checks preserve this distinction.


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
  "$inc/HyLIoSParticleOverlap.cpp" "$inc/HyLIoSOptics.cpp" \
  "$inc/HyLIoSParticleGeometry.cpp" "$inc/HyLIoSSlabTransport.cpp" \
  -o /tmp/hylios-forward-test
/tmp/hylios-forward-test
/tmp/hylios-forward-test --absorption-overflow-only
/tmp/hylios-forward-test --discard-only
```

The sanitizer form uses `-O1 -g -fsanitize=address,undefined` with the same strict
FP flags. For mixed full LTO, compile **both** `HyLIoSParticleGeometry.cpp` and
`HyLIoSParticleOverlap.cpp` separately with `-O3 -flto=full -fno-fast-math
-ffp-contract=off`. Compile the transport, optics, slab and test caller with
`-O3 -flto=full -ffast-math -fno-finite-math-only`, then link all objects with
`-O3 -flto=full`. Do not put geometry into the fast-math caller batch. Its strict
compilation guard is part of the tested contract.
