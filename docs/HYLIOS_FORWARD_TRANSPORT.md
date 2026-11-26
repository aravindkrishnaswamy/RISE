# HYLIOS individual-spheroid forward transport

`HyLIoSForwardTransport` is an experimental forward kernel with explicit unresolved outcomes. The independent `HyLIoSSlabTransport` remains the analytic control. There is no renderer integration, complex support, reciprocity, adjoint sampler, or full S1–S4 claim.

`HyLIoSForwardTransport` accepts exactly six resolved monochromatic layers in
SC/SG/SS/SB/PD/RD order, explicit host/particle coefficients and indices, an
explicit PD fiber encounter rate, estimator/RR settings and an explicit placement
policy. Local launch position is (0,0,0); retain scene translation in adapters.
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

## Internal particle process and unresolved angular law

Entry has no extra dielectric event or refraction. Absorption is evaluated along
each true local chord using the particle coefficient alone; host absorption acts
on host distance alone. At surviving exit, one melanin-to-host Fresnel test occurs.
Internal reflection receives a cosine perturbation about the **specular reflected
direction**, conditioned inward. Each rejected proposal costs one quantum. A new
chord uses the analytically deflated known surface root; no origin epsilon.
After transmission, the named 1400nm endpoint exponential forward law is applied
once. At/above1400nm it is an exact forward delta and consumes no angular draws.

A post-exit angular perturbation can point back into its particle. The publication
does not define the continuation. This kernel retains the drawn direction, all
particle state and live weight, reporting `ForwardPerturbationReentry`. It does
not condition away those samples or invent repeated absorption. This currently
prevents a complete visible-range transport claim. The diagnostic is material,
not a negligible endpoint issue: see the exploratory results below.

PD fiber state resets only at actual transmitted exit/re-entry. A Fresnel
reflection within PD cannot reset eligibility. All random inputs come from the
provided variable-budget ISampler. Complete segments and individual proposals are
explicit phases. `Advance` quantum is a latency yield; `StopForBudget` and `Cancel`
are separate unresolved outcomes retaining current weight. Neither is absorption.
Analog absorption and weighted Beer/RR use separate host/particle tallies.

## Validation surfaces

`tests/HyLIoSForwardTransportTest.cpp` exercises independent slab and spherical-chord controls, analog/weighted/roulette means, host versus particle absorption ownership, fiber-pass state, explicit placement policies, quantum/RNG equivalence and unresolved accounting. `tests/HyLIoSForwardFailureTest.cpp` supplies a deterministic overlap fault provider to verify propagation of indeterminate, invalid and unsupported predicates. These mechanics tests do not establish author-model equivalence.

Particle geometry and overlap predicates require strict floating-point compilation even when the transport and caller use fast math. Their separate full-LTO checks preserve this distinction.
