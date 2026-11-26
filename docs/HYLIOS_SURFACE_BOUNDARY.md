# HYLIOS forward surface-fold boundary

`HyLIoSSurfaceBoundary` implements the selected forward physical-power law
`hylios2015_macro_fresnel_normal_perturb_v1`. The measurement CLI selects it with
`--boundary hylios2015` and the resolved specimen's explicit
`surface_fold_aspect_ratio`. `--boundary smooth` remains the existing planar
diagnostic override. No renderer BSDF, NEE/connection PDF, reciprocity, adjoint
response or author-code equivalence is claimed.

## Publication evidence and selected interpretation

Paper §4.3 gives a Fresnel test followed by surface-normal perturbation for
reflected rays, uniform azimuth and Eq. (8). The thesis discussion corroborates the normal
perturbation and relates s to fold aspect ratio. The inspected thesis Table 5.1
uses .1/.25/.1/.45; a separate goniometric footnote uses .3. Tests cover those
source values as fixtures, without installing a material default. The source
inventory and inspected download identities are recorded in
[HYLIOS_RESEARCH_NOTES.md](HYLIOS_RESEARCH_NOTES.md); the equation reconciliation
is [HYLIOS_EQUATIONS.md §6](HYLIOS_EQUATIONS.md#6-layer-size-reduction-and-boundary-events).

The inspected publication does not supply the printed polar weight's
normalization, both crossing-side cases, or a complete wrong-hemisphere rejection
rule. This implementation explicitly normalizes Eq. (8), orients the normal
about the incident-facing macro normal, rejects incident-backfacing normals and
reflections outside the incident macro hemisphere, and retains macro-normal
Snell transmission. These are named clean-room choices, not undisclosed author
implementation details.

## Normalization and one-proposal sampler

Let mu=cos(theta), 0<=mu<=1, D=1+(s²-1)mu², and
I(s)=integral_0^1 D^-2 dmu. The printed Eq. (8) polar weight has normalization
Z(s)=s⁴ I(s). The normalized per-radian density is
sin(theta)/(I D²), with independent uniform azimuth.

| Fold | I(s) |
|---|---|
| 0<s<1, k=sqrt(1-s²) | 1/(2s²)+atanh(k)/(2k) |
| s=1 | 1 |
| s>1, a=sqrt(s²-1) | 1/(2s²)+atan(a)/(2a) |

The implementation uses atanh(k)=log1p(k)-log(s) and
1-k=delta=s²/(1+k), avoiding cancellation.

For s<1, the normalized proposal is q(mu)=delta/(1-k mu)², with inverse
mu=u/(delta+k u). Independent rejection acceptance is A(mu)=1/(1+k mu)².
Thus q A=delta D^-2, proportional to the desired target. A>=1/4 pointwise.
The inverse denominator is evaluated as u+delta(1-u), retaining
1-mu=delta(1-u)/(u+delta(1-u)); sine uses sqrt((1-mu)(1+mu)). A rounded cosine
of one therefore does not silently erase a representable small polar sine.

For s>1, use q(mu)=a/[atan(a)(1+a²mu²)], inverse
mu=tan(u atan(a))/a, and acceptance A=1/(1+a²mu²).
Average proposal acceptance is a I/atan(a)>=1/2. This is an average bound;
pointwise acceptance can be much smaller. For s=1, sample mu=u and accept the
polar proposal. At s=0, use the exact smooth delta without normal proposals.
These bounds concern Eq. (8) sampling, not the subsequent geometric hemisphere
acceptance. Total reflected completion work has no finite bound for every input.

## Explicit numerical domain

Supported folds are zero or
sqrt(binary64 epsilon)<=s<=1/sqrt(binary64 epsilon), approximately
[1.4901161193847656e-8,67108864]. This is an implementation domain, not a physical
fold range. It keeps s² in [epsilon,1/epsilon], makes the characteristic small-fold
cosine departure resolvable at binary64 precision, and keeps the large-fold
arctangent endpoint about sqrt(epsilon) or farther from pi/2. At the largest
supported fold the inverse can still lose roughly sqrt(epsilon) relative accuracy.

The immutable constructor rejects unsupported source values before histories.
It does not convert them to a smooth surface. Strict IEEE binary64 compilation
is required. Invalid consumed variates and unrepresentable inverse/reflection
arithmetic fail explicitly; no origin epsilon, fitted angular cutoff or
invented direction is used.

## Pinned Fresnel branch and crossing ownership

`Begin(encounter)` validates and pins the complete encounter, normalized incident
direction and macro Fresnel/Snell optics; it consumes no work or random values.
The slab frame has outward normal (0,0,-1), x/y tangents and top z=0.

`TrySample` performs exactly one transition:

1. `Branch` uses one uniform to choose macro reflection or transmission once.
   Transmission becomes `Ready` with the original macro Snell direction.
   Reflection becomes `NormalProposal`, or `Ready` for zero fold.
2. `NormalProposal` uses three uniforms: polar proposal, azimuth and acceptance.
   It attempts one Eq. (8) normal, then checks incident-facing normal and strict
   incident macro hemisphere. Rejection keeps the same reflected branch and
   encounter. Acceptance becomes `Ready` with multiplier one.

Rerolling Fresnel after rejected normals would multiply branch masses by normal
acceptance and is incorrect. The selected law keeps reflection mass F, including
macro total internal reflection. It never uses a micro-normal for transmission.
There is no second Fresnel throughput factor or radiance eta² factor.

| Incident side | Reflection | Transmission |
|---|---|---|
| Exterior | Perturbed outward direction; external reflection terminates transport | Macro Snell inward; continue transport |
| Interior | Perturbed inward direction; continue transport | Macro Snell outward; subsurface return terminates transport |

The final decision uses the pinned transport encounter token. The law must
outlive its states. Sampling after `Ready`, `Cancelled` or `BudgetExceeded` is
rejected. Invalid consumed variates and wrong-owner states throw without changing
boundary state. Unused branch-phase azimuth/acceptance arguments are ignored.

## Measurement adapter, budgets and failure accounting

Measurement schema `hylios-forward-measure-v4` separates transport work from
outer-boundary work. With `--boundary hylios2015`, every branch or normal-proposal
attempt costs one caller work unit, including rejected proposals and attempts
ending in a diagnostic. `max_transitions_per_history` caps their sum with kernel
transport transitions. The pinned state remains alive across each proposal;
cancellation is checked between boundary transitions. Transport batches remain
bounded by 64 transitions and the remaining caller budget.

`aggregate_counters.workUnits` keeps its original transport-only meaning.
`surface_boundary_counters` records work/branch/proposal attempts, normal
rejections, exterior/interior R/T branch counts, ready decisions, pending-state
budget stops/cancellations, invalid samples and numerical failures.
`caller_work_units` is transport work plus boundary attempts. Pending-state
cancellations include diagnostic cleanup; terminal outcome counts distinguish
user cancellation from invalid/numerical failure. A selected reflected branch
can be counted while its live power is still unresolved; it is not yet escaped
power.

The smooth diagnostic keeps its exact legacy cap ordering, keeps helper errors as
pre-publication aborts, and excludes smooth boundary draws from work, as reported in `boundary_work_convention`. At zero
fold and sufficient budget, its physical histories, transport counters and RNG
consumption equal the selected HyLIoS boundary exactly; caller work differs
because the new mode counts branch decisions.

Budget exhaustion and cancellation stop both pending boundary state and kernel
state while preserving kernel weight and already tallied absorption. A consumed
invalid random value or boundary arithmetic exception cancels pending transport
to preserve that same physical state, then an explicit adapter terminal
classification publishes `invalid_sample` or `numerical_failure`. Boundary
numerical diagnostics use `numerical_failure_details.surface_boundary`. None is
absorption, and every incident history remains in the denominator. Invalid source
fold values are preflight input errors and leave existing output untouched.

The CLI emits the existing signal-handler readiness line, plus one
`hylios_measure: reflected surface branch pending` line when reflected-normal
sampling first becomes pending. It emits no per-proposal stream. This is progress
and cancellation-test readiness, not scientific identity.

## Validation and current limits

The boundary unit test independently integrates printed Eq. (8), checks proposal
and accepted-normal CDFs, compares reflected moments/CDFs to direct theta/phi
quadrature at normal/oblique/grazing incidence, checks the analytic s=1
normal-incidence reflected CDF, zero-fold four-sided collapse, matched indices,
TIR/critical angle, Fresnel masses, incident-side mirroring, invalid inputs,
pinned rejection, cancellation, budgets and arbitrary resume partitions.

CLI contracts independently check normal-incidence Fresnel mass in an opaque
host despite frequent normal rejection; exact zero-fold physical/RNG parity with
smooth; deterministic reflected-proposal caps preserving live weight and branch;
source-fold atomic validation; and readiness-driven SIGINT inside a long-running
reflected sampler. The cancellation reader has a deadline and uses binary pipe
chunks rather than timing sleeps. All-history accounting includes unstarted
histories at their original unit live weight.

Standalone source builds must include `HyLIoSSurfaceBoundary.cpp` with strict
`-fno-fast-math -ffp-contract=off` flags, also under full LTO. Geometry, overlap and
complex layout retain their separate strict contracts. See
[HYLIOS_MEASUREMENT.md](HYLIOS_MEASUREMENT.md) for source lists and test commands.
No shared archive relink is required for these gates.

The current CLI launches normal incidence and publishes scalar power/counters;
angle sweeps and outgoing histograms are separate increments. Implementing this
forward boundary does not establish a reciprocal renderer boundary or adjoint.
The input-data approximation labels and the other selected clean-room transport
interpretations remain separate from boundary implementation correctness.
