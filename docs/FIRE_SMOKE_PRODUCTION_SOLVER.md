# Fire/Smoke Production Solver Contract

Status: **r86 design contract; P0/P1 landed, P2 pinned**
Reference date: 2026-08-18  
Normative companions: [FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md),
[FIRE_SMOKE_SOLVER_SPEC.md](FIRE_SMOKE_SOLVER_SPEC.md), and
[FIRE_SMOKE_DESIGN_HISTORY.md](FIRE_SMOKE_DESIGN_HISTORY.md).

## 1. Role and non-goals

RISE has two deliberately different fire solvers.

1. The existing fp64 conservative solver is the **certified reference oracle**.
   Its fail-closed envelopes, Picard coupling, V-tier fixtures, and preserved
   tier-10 prefix define the validation evidence. It remains available for
   manufactured gates and bounded oracle runs; it is not the production
   throughput path.
2. The new solver is the **production solver**. It is fp32, Metal-GPU-first,
   uses one pressure projection per accepted schedule step, and is judged by a
   validation contract against the oracle rather than by reproducing every
   fp64 per-step certificate internally.

The production solver is not a relaxed mode of the oracle and may not share a
runtime switch that silently changes certified semantics. It has a distinct
algorithm/version tag and distinct producer build identity. The oracle's
debug-only MacCormack requirement does not constrain production advection.

## 2. Fixed target and feasibility budget

The shipping target on the recorded Apple M4 Max (40-core GPU, 64 GiB unified
memory) is:

- tier-10, D=0.30 m, Qdot_ref=33.0 kW, t=25.032480502915522 s in **no more
  than one hour**;
- tier-6 pipeline/validation cases in minutes;
- GPU-resident state with no per-step full-grid CPU readback.

The production schedule uses a maximum physical step of 1/480 s, split only at
identity-bearing case events and output times. Semi-Lagrangian transport removes
an advective CFL stall; the local semi-implicit source update owns its stated
stability range. The tier-10 upper count is therefore 12,016 ordinary steps
plus a small number of event splits. The p95 step budget is 250 ms:

| Component | p95 budget |
|---|---:|
| conservative semi-Lagrangian remap | 45 ms |
| local source maps and record tables | 25 ms |
| momentum forces and viscosity | 20 ms |
| one variable-density projection | 120 ms |
| reductions, probes, and scheduled transfer | 20 ms |
| contingency | 20 ms |

At 250 ms the main loop is approximately 50.1 minutes, leaving about 9.9
minutes for durable frames, checkpoints, manifest construction, and rendering
handoff. Tier-10 storage is 976,272 cells; double-buffered fp32 conservative,
MAC, coefficient, multigrid, and diagnostic allocations must remain below
2 GiB. A milestone that exceeds either 250 ms p95 or 2 GiB at the tier-10
shape is a design failure, not a request to weaken validation.

## 3. GPU-resident step

### 3.1 State and execution

The authoritative production state is fp32 SoA storage for rho_tot Z, the
record-ordered species/aerosol densities, sensible enthalpy density, and
staggered momentum. Metal kernels use fixed grid-to-thread mappings. State,
temporary remap buffers, coefficients, and the multigrid hierarchy remain on
the GPU across steps. CPU interaction is limited to command submission,
scheduled durable output/checkpoint transfers, and fixed-size diagnostic
reductions.

The same build on the same GPU family must be deterministic: fixed threadgroup
ranges, deterministic max reductions, and fixed reduction trees for sums.
Cross-device bit identity is not promised; device/OS/Metal-library identities
are producer metadata and cross-device results still face the oracle contract.

### 3.2 Advection choice

Production uses a **conservative semi-Lagrangian flux-form PPM remap**:

- trace each face's swept interval over the fixed step with midpoint velocity;
- reconstruct a monotone piecewise-parabolic profile in each conserved SoA
  channel;
- integrate the swept profile to a face flux and apply one unsplit conservative
  update;
- use the identical geometric weights for rho_tot Z, every constituent, and
  sensible enthalpy;
- remap staggered momentum on its dual control volumes with the same scheme.

This scheme was selected because it removes the advective CFL throughput wall
without sacrificing conservation. Classic MacCormack/BFECC was rejected for
production because its correction/clamp path is neither conservative nor a
clean oracle-comparison surface. First-order semi-Lagrangian interpolation was
rejected because its plume diffusion would consume the puffing and McCaffrey
error budgets. A multi-projection RK/Heun path was rejected because the target
requires one projection per step.

The remap must be monotone for nonnegative inputs without post-update clamping.
Any signed trace inventory is logged; no hidden repair changes stored state.

### 3.3 Sources and thermochemistry

The certified records remain the sole source of physical constants. A build-time
table compiler consumes their canonical payload bytes and emits fp32 tables plus
a canonical table manifest containing every source record ID, compiler version,
domain, knot bytes, maximum certified interpolation error, and table SHA-256.
There is no hand-authored duplicate constant.

- Species h_s(T) and cp(T) are one-dimensional adaptive tables.
- Temperature inversion uses a monotone table-assisted bracket followed by a
  fixed-count local iteration.
- Planck-mean gas opacity uses the adopted record's T/composition coordinates;
  carbon continuum remains its analytic record law.
- Reaction/source maps retain record stoichiometry and use a local
  semi-implicit update over the production step.

Adaptive subdivision continues until the table's independently evaluated fp64
error is at most one quarter of the relevant oracle-calibrated validation
allowance. Table compilation fails if the record domain is not completely
covered. Runtime never extrapolates; an out-of-domain access is logged as a
validation failure and terminates with a structured runtime error rather than
clamping or halving.

### 3.4 Momentum and projection

Body force, molecular viscosity, and the retained Vreman closure are evaluated
once per production step. Advection produces provisional staggered momentum.
Exactly one variable-density pressure projection then enforces the record-owned
source divergence and pressure-open boundary model.

Projection uses a GPU geometric multigrid hierarchy with fixed coarsening,
fixed smoother order/count, and a fixed residual-check cadence. Its cycle count
is selected by the validation increment, not adapted to make individual steps
pass. The runtime records pre/post divergence norms and boundary residuals but
does not halve dt for a missed oracle-style certificate. A nonfinite solve or
Metal execution failure aborts structurally; a finite residual outside the
validated band is retained in diagnostics and fails the validation run.

## 4. Monitored acceptance

Every scheduled finite production step advances exactly once. The runtime logs:

- dt, simulated time, T_min/T_max, total mass and element ledgers;
- HRR, fuel consumption, radiative loss and effective chi_r;
- projection residuals, maximum EOS residual, signed inventory minima;
- table-domain excursions and all nonfinite/runtime failures;
- kernel timings and GPU/device identity.

Finite deviations do not trigger retry, under-relaxation, clamping, or
fail-closed halving. They are evaluated by the validation harness. Nonfinite
state, incomplete command execution, allocation failure, record/table digest
mismatch, or output/checkpoint I/O failure remains a structured runtime abort;
continuing from unknown bytes is not monitored acceptance.

## 5. Oracle-derived validation contract

### 5.1 Calibration rule

Calibration has two horizon classes and they do not share a tolerance model.
For a deterministic short-horizon metric `m`, define the outward-rounded
triangle bound

`T_m = E_h,prod,m + E_dt,prod,m + E_h,oracle,m + E_dt,oracle,m + B_fp32,m`.

`E_h` and `E_dt` are Richardson distances from the compared baseline to the
solver's own apparent continuum limit. They are evaluated separately for the
production scheme and the certified scheme; a production-versus-oracle
difference is never used to estimate either term. `B_fp32` is an analytic
roundoff bound for the production operation graph, instantiated from sealed
input bytes. The fp32-Metal versus fp64-same-scheme difference only confirms
that bound and never selects it. A short-horizon metric passes only when its
observed difference is at most `min(T_m,C_m)`.

Three temporal levels (`dt`, `dt/2`, `dt/4`) reach the same physical end time.
For a factor-two refinement, let `D_coarse=|U_dt-U_dt/2|` and
`D_fine=|U_dt/2-U_dt/4|`. Then
`p=min(p_formal,log2(D_coarse/D_fine))` and the baseline-distance estimate is
`E_dt=D_coarse/(1-2^-p)`. The two differences must
be finite, positive, and strictly decreasing; an exact zero is admissible only
for a separately proved algebraic identity. The retired r106 adjacent-tier
spatial instrument is not an acceptance path. Its ratios `h5/h6=1.2` and
`h6/h7=7/6` make the positive-order ratio interval only
`[1.18274896,1.65846154]`; front placement and unequal domains then dominate
that ill-conditioned quotient. Spatial calibration instead uses two exact
dyadic pairs on one analytic, smooth, fixed-domain beginning state. Each pair
evaluates the V2-verified lower order `p=1.8`, and the two independent
fine-grid Richardson balls must overlap. This tests consistency without
estimating an order from three nearly adjacent grids. The tier-6 distance is
the outward maximum of the direct `{6,12}` estimate and the `{5,10}` estimate
rescaled by `(h6/h5)^p`; no fitted factor is permitted.

Long-horizon validation remains statistical and integral. Its block bootstrap,
RMS, onset, frequency, and conservative time-bin terms are calibrated on the
corresponding V-tier, empirical, or prefix campaign. Short-horizon pointwise
bands are valid for at most eight chained steps and may not be reused for a
ninth step or a long chaotic trajectory. Calibration inputs, output evidence,
and resulting tolerances are separate versioned artifacts; production output
cannot mutate or complete the sealed input manifest.

### 5.2 Scientific ceilings

| Gate | Ceiling C_m |
|---|---:|
| manufactured advection L1 error | 1.25 times oracle error, observed order >=1.8 |
| manufactured projection L_inf divergence | 5e-3 U/L and no worse than 1.25 times oracle error |
| V4 mass/element/H_s ledger drift | 0.25% of accumulated throughput |
| V5 homogeneous radiation exchange | 2% relative |
| ignition time / puffing onset | 0.10 t_ft |
| T_max trajectory | 75 K max error and 3% normalized RMS |
| HRR trajectory/integral | 5% normalized RMS / 3% integral |
| fuel-consumption versus HRR ledger | 2% |
| puffing frequency | 10% |
| McCaffrey centerline T and u | 10% at every adopted station |
| integrated chi_r | 0.02 absolute and still inside 0.07--0.28 |

A calibration band wider than its ceiling rejects the model/configuration; the
ceiling is never widened to admit production output.

### 5.3 Required comparisons

1. **V-tier:** production analogues of V1--V6 run against independent expected
   values and the oracle outputs. V4/V5 algebraic and radiation fixtures may
   call production table kernels directly; V6 compares fixed-grid histories
   and ignition graphs.
2. **Tier-6 empirical:** cold-start methane with gravity must ignite within the
   pilot window, sustain after pilot-off, remain below 2300 K, and meet the
   puffing, McCaffrey, HRR/fuel, and chi_r ceilings.
3. **Golden prefix:** the preserved reference prefix through t=2.8854439500002069
   s, checkpoint SHA-256
   `1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`,
   supplies time-aligned HRR, T_max, EOS, dt, and puffing-onset evidence.
   Production is compared at oracle timestamps by conservative time-bin
   integration, never by dropping samples. Required gates are HRR trajectory
   and integral, T_max trajectory, ignition/puffing onset, and physical ceilings.

### 5.4 Short-horizon calibration quantities

The short-horizon registry is dimensioned, never a maximum across unlike
units. It contains a volume-normalized `L1` field metric and one physical
integral for each of the nine conservative channels, plus volume-weighted `L2`
cell-centered velocity and projection residual. Density-like channels,
`rho_tot Z`, carbon aerosol, and sensible enthalpy retain their own units and
bands. Pointwise `L_inf` at a transported discontinuity is not used for a
Richardson term; exact local donor envelopes and signed inventory minima remain
separate structural gates. Every metric declares its support, cell/face
overlap rule, normalization (if any), formal spatial/temporal order, scientific
ceiling, and horizon class before evidence is produced.

The exact step-3479 bytes seed two different gates. A local-slice ensemble may
restart from separately SHA-bound oracle beginnings and diagnoses one-step
operator behavior. A short trajectory must instead chain each solver's own
accepted state from the root for at most eight steps. Substituting the next
oracle snapshot into a production chain is a structural failure. `S_div`,
source operands, boundary records, and schedules are sealed comparison inputs;
none is obtained live from the oracle invocation being judged.

## 6. Provenance and artifacts

Production uses the existing case file and `case_record_id` unchanged. Solver
choice is producer semantics, not case authorship. Sequence manifests,
`sequence_backed` active-fire media, canonical one-preimage hashes,
`preview_primary` labels, reason codes, and per-frame sidecars remain mandatory.

Producer/run metadata additionally records:

- `solver_kind=production_metal_fp32` and algorithm version;
- executable/library/Metal-kernel SHA-256 values;
- GPU registry ID/name/family, OS and Metal language versions;
- every certified source record ID and compiled table manifest ID;
- validation-package ID and gate-evidence IDs;
- fixed dt schedule, projection cycle policy, timing distribution, and any
  monitored deviations.

This is integrity and reproducibility metadata, not authentication.

## 7. Implementation increments

Each increment is a pathspec commit with baseline timing, focused gates, and a
fresh zero-P1 review before the next dependency is built.

1. **P0 -- capability and table compiler.** Metal capability query, deterministic
   command harness, record-to-table compiler, table digest/domain/error REDs,
   and a CPU oracle reader. Gate: table values/derivatives against certified
   records and no duplicated physical literals.
2. **P1 -- advection only.** GPU SoA state and conservative PPM remap. Gates:
   constant state, translated pulse, smooth manufactured order, scalar/enthalpy
   common weights, mass/element conservation, crossflow/open-boundary cases,
   and CPU reference comparison from the first kernel. Record p50/p95 timing.
3. **P2 -- projection only.** Staggered momentum, one variable-density GPU
   projection, pressure-open boundaries, source-divergence manufactured fields,
   and projection residual/timing gates. No chemistry is present in this step.
4. **P3 -- coupled transport.** Momentum forces, Vreman update, source divergence,
   one remap plus one projection. Gates: V1--V3 histories and golden-prefix
   advection/projection slices.
5. **P4 -- sources/radiation.** Record-derived fp32 thermochemistry/opacity
   tables, local semi-implicit chemistry, pilot map, and radiative exchange.
   Gates: V4/V5, table-domain fail-fast, pilot exclusion from Qdot_tot, and
   chemistry/radiation oracle comparisons.
6. **P5 -- validation and artifact path.** V6, tier-6 empirical contract,
   preserved-prefix comparison, sequence writer/loader, gate-5 preparation,
   renderer, full sidecars, and the one-hour tier-10 performance gate.
7. **P6 -- production capstone.** Run the validated production tier-10 case,
   extract locked empirical rows, render preview-primary output, and publish the
   oracle-comparison report.

### 7.1 P0 landing pins (r83)

P0 is a library capability, not a test-local shell probe. The platform API
returns an explicit `available` result, device name/registry ID/family, maximum
threadgroup width, and unified-memory status. On Metal hosts it must compile and
dispatch an embedded fp32 identity-map kernel and verify the returned bytes;
device discovery without command execution is insufficient. Non-Metal builds
compile an honest capability-unavailable stub and retain all CPU table gates.

The first table manifest contains record-ordered species sensible enthalpy/cp
tables and CO2/H2O Planck-mean tables. Thermochemistry subdivision continues
until midpoint interpolation error is no more than the record cp lower bound
times 0.25 K (one quarter of the P0 1 K equivalent table gate). Opacity
subdivision/error evaluation must remain below 0.5% relative (one quarter of
V5's 2% ceiling), with a scale from the evaluated endpoint/midpoint magnitudes
rather than a near-zero result. Those derived thresholds are compiler semantics,
not caller knobs. The canonical manifest binds source record IDs, domains,
all emitted knot/value fp32 bytes, measured fp64 error bounds, compiler version,
and its one-preimage table ID. Runtime lookup rejects out-of-domain input.

### 7.2 P1 conservative-remap landing pins (r84)

The production advection operator is an unsplit-in-state, directionally
composed flux-form semi-Lagrangian PPM remap. For one Cartesian sweep, every
arrival face is backtraced with the prescribed midpoint face velocity. A
single face thread evaluates the signed integral of the donor parabolic
profile from the departure face to the arrival face, including every complete
cell crossed and the two fractional cells. The two cells sharing a face read
that one stored flux; no cell scatters mass and no atomic addition is allowed.
The update is the difference of those shared face integrals divided by cell
width. Thus the method remains conservative for Courant numbers above one;
silently truncating a departure to the adjacent donor is forbidden.

The unlimited interface value is the uniform-grid fourth-order value
`7(q_i+q_{i+1})/12 - (q_{i-1}+q_{i+2})/12`. Each cell converts its two
interfaces to the standard cell-average-preserving parabola. One cell-local
limiter coefficient in `[0,1]` is then selected as the minimum required over
the complete transported conservative tuple so every component polynomial
stays within its three-cell donor envelope and has no interior extremum beyond
that envelope. The same coefficient scales every component's two edge
deviations from its cell average. This common limiter and the shared departure
geometry make every transported component a linear combination with identical
weights; element, mixture-fraction, and sensible-enthalpy affine relations
therefore move together. Component-wise clipping after remap is forbidden.

Periodic antiderivatives wrap by an integer number of domain integrals plus a
canonical remainder. Pressure-open extension is selected from the accepted
face-velocity sign, not the reference solver's pressure active-set bit:
ambient authored state supplies resolved inflow and the nearest interior donor
extends resolved outflow. A wall face has zero swept interval. The production
bed/source map enters in P4 and is not synthesized by P1.

GPU determinism is structural: SoA components use fixed buffer order; face and
cell kernels write disjoint indices; the domain antiderivative uses one pinned
Blelloch tree padded to the next power of two; no atomics, subgroup-width
dependent reductions, `fast::` intrinsics, or compiler fast-math are allowed.
The CPU comparison kernel explicitly rounds the same stored intermediates to
binary32. Cross-device validation uses the r82 numerical bands, while repeated
runs on one recorded device must be byte-identical.

P1 gates comprise: exact constant-state transport; a periodic translated pulse
at Courant numbers below and above one; smooth manufactured order at least 1.8;
shared-weight mass/element/mixture-fraction/enthalpy residuals; pressure-open
inflow and outflow with distinct ambient/interior scalars; wall zero flux; and
CPU-versus-Metal face-flux and updated-state comparisons. The tier-10-shaped
remap must fit the r82 45 ms p95 budget before P2 begins. MacCormack/BFECC,
adjacent-cell CFL truncation, component-specific limiting, and post-remap
clipping are rejected because they respectively lose conservation, mis-handle
the target Courant range, split the affine tuple, or hide monitored deviation.

#### 7.2.1 Executable swept-interval closure (r85)

The first P1 implementation review measured three failures that the original
wording did not exclude strongly enough.  With a constant `0.1f` field, a
`C=0.3` periodic sweep returned values from `0x1.999992p-4` through
`0x1.9999aap-4`; subtracting two absolute fp32 antiderivatives had erased the
free-stream invariant.  At a periodic seam, independently evaluated endpoint
fluxes differed by one or more ulps even with equal velocities, and unequal
endpoint velocities changed a test-domain sum from 36 to 32.  Finally, two
outward faces with `C=0.75` drained a nonnegative unit cell to `-0.5` because
their backtraced faces had crossed.

The executable rule is therefore:

- fractional-cell integrals use a factored interval polynomial in `b-a`; they
  never subtract two large absolute antiderivatives.  Complete crossed cells
  are accumulated in canonical donor order, while the padded Blelloch tree is
  retained for the whole-domain integral used by periodic wraps;
- a periodic line has one physical seam.  Its two API endpoint velocities must
  be equal, face zero is canonical, and the stored face-`n` flux is the exact
  byte duplicate of face zero;
- the scheduled backtraced face map must be finite and nondecreasing on every
  line.  This is a pre-step scheduling condition on velocity gradients, not an
  absolute-Courant restriction: uniform transport may still cross any number
  of cells and whole domains.  The future production driver selects a valid
  scheduled dt before dispatch; it does not discover a fold by retry-halving;
- every published state, face flux, limiter coefficient, and device timing is
  finite.  The two-GiB admission calculation counts every actual Metal buffer
  (input state and velocity, ambient tuple, both edge arrays, limiter, prefix,
  flux, output, and parameter bytes) with checked arithmetic.

Rejected alternatives are post-remap clipping (hidden state repair), an
outgoing-flux cap introduced solely to rescue a folded map (changes the pinned
operator and common weights), independently recomputing the periodic duplicate
(permits secular leakage), and tolerating nonfinite output until validation
(structural failure, not model deviation).  RED coverage includes late-cell
large-prefix tiny sweeps, ordinary non-power-of-two constants, a
cancellation-sensitive Blelloch total, negative open-boundary roles, a folded
departure map, finite-input arithmetic overflow, GPU common-limiter behavior,
and source/build guards binding safe math and the four real Metal kernels.

### 7.3 P2 one-projection landing pins (r86)

P2 owns one linear, variable-density MAC projection.  Its public batch has a
uniform Cartesian shape `(nx,ny,nz,dx)`, positive finite cell gas density,
record-ordered provisional face momentum, a finite cell divergence target,
`dt`, ambient density, and one boundary kind per side.  Face arrays have the
open-MAC shapes `(nx+1)ny nz`, `nx(ny+1)nz`, and `nx ny(nz+1)` even when an
opposite side pair is periodic.  Periodicity is legal only as an opposite-side
pair; the other supported kinds in P2 are adiabatic wall and pressure-open.
Fuel-bed prescribed momentum enters P4 and is rejected by the P2 API rather
than synthesized as a pressure boundary.

Face density is the fp32 arithmetic mean of its two adjacent cell densities;
an open boundary's outside value is the authored ambient density.  Provisional
velocity is face momentum divided by that stored face density.  Wall normal
velocity is exactly zero.  A pressure-open face freezes one production class
before the solve from the provisional outward normal velocity: resolved
outflow uses ambient static gauge pressure zero; resolved inflow uses the
reference total-head linearization
`p_b=-rho_amb |u_star|^2/2`, including both tangential components sampled by
the same centered boundary-cell stencil.  Classification and `p_b` are frozen
for the only projection; they are not iterated, relaxed, or changed to make a
step pass.  The post-projection complementarity discrepancy is a monitored
diagnostic and an oracle-validation metric.

With cell pressure `p`, arithmetic-mean inverse face density `beta`, centered
MAC divergence `D`, gradient `G`, and the half-cell open-boundary gradient,
the operator is

`A p = -D(beta G p) = (S_div-D u_star)/dt`.

Interior gradient and divergence are an adjoint pair.  An open boundary adds
the exact factor-two Dirichlet coefficient and its frozen `p_b` contribution;
a wall adds neither coefficient nor flux.  Periodic face zero is canonical and
the opposite stored face is its byte duplicate.  A closed/periodic component
has a constant nullspace: its right-hand side mean is removed by one pinned
Blelloch tree and pressure is returned mean-zero.  A component containing an
open face has no nullspace.  The corrected momentum is `m'=m-dt Gp`; velocity
is derived from the same stored face density.  No velocity, pressure, or
residual clamp exists.

The production solve is a matrix-free fp32 geometric multigrid with a fixed
hierarchy and schedule:

- each dimension greater than four coarsens independently by `(n+1)/2` until
  every dimension is at most four; density and multigrid residual restrict by
  volume-weighted averaging over the actual fine cells, so odd dimensions have
  no invented duplicate cell;
- each level rediscretizes the same MAC operator from its restricted density
  and inherited boundary kinds; correction prolongation is cell-centered
  trilinear interpolation with the same clamped coarse-coordinate rule in CPU
  and Metal. Hierarchies containing a pressure-open side multiply every coarse
  correction by the stored binary32 `0.75f`; periodic/wall-only hierarchies
  retain the established undamped correction;
- one V-cycle has three weighted-Jacobi pre-sweeps and three post-sweeps with
  the stored binary32 result of `2.0f/3.0f`; the coarsest level performs 32
  Jacobi sweeps;
- exactly 12 V-cycles execute from zero pressure for periodic/wall-only
  hierarchies, and exactly 16 execute for a hierarchy containing any
  pressure-open side. There is no residual-based
  early exit, retry projection, warm-start history, atomics, subgroup scan, or
  CPU solve. Residual reductions occur only before cycle one and after the
  fixed final cycle through padded max/sum trees.

The boundary-class schedule is fixed by r104, not selected from the runtime
residual. Twelve undamped cycles preserve the qualified periodic/wall oracle
ratio; sixteen `0.75f`-damped cycles stabilize the rediscretized pressure-open
coarse correction and fit the r82 120 ms projection allocation. If an
independent manufactured field misses the scientific ceiling, the
schedule/model is revised in a new numbered design revision; a runtime knob or
per-case cycle adaptation is not allowed.

The CPU comparator stores and rounds the same fp32 face densities, hierarchy,
Jacobi states, restriction, prolongation, and correction expressions.  Metal
safe math and the same strict host build bindings as P1 apply.  Same-device
repetition must be byte-identical.  Structural failures—malformed shapes,
nonpositive density, nonfinite derived arithmetic, allocation/command failure,
or output nonfiniteness—return no result.  A finite residual outside the
validation band is returned with `validation_passed=false`; it is not converted
into dt halving or another projection.

Independent P2 gates are: exact rest for periodic, all-wall, and all-open
boxes; a constant-density sinusoidal pressure field whose provisional momentum
is formed independently so the known pressure is recovered up to its gauge;
a variable-density manufactured field with nonzero `S_div`; an x-only
pressure-open expansion with every boundary face checked; sign-reversed
inflow/outflow total-head roles; odd and non-power-of-two shapes; repeat-byte
determinism; CPU/Metal comparison; and a tier-10-shaped timing/memory run.
Post-projection `L_inf |D u-S_div|` must be no more than `5e-3 U/L` and no
worse than 1.25 times the fp64 oracle error.  Both Metal command time and an
external completed-call p95 must be at most 120 ms on the recorded M4 Max.

Rejected alternatives are conjugate-gradient or BiCGStab dot products
(additional global reductions and ordering sensitivity), adaptive cycles
(data-dependent runtime and a hidden acceptance loop), a CPU coarse solve
(GPU residency and fallback ambiguity), harmonic face density (unnecessary
departure from the certified arithmetic-mean oracle), nonlinear open-boundary
iteration (more than one projection in substance), and treating a finite
residual miss as a structural abort (recreates the certified solver's stall in
the monitored production path).

### 7.4 P3 coupled-transport landing pins (r87)

P3 owns one production transport step without chemistry, radiation, phase
change, pilot, or prescribed bed flux. Those source maps enter at P4. Its
beginning snapshot contains record-ordered cell conservative channels,
staggered momentum, a finite cell `S_div` field, and the molecular kinematic
viscosity field that P4 will derive from the record tables. Component zero is
gas density. The Vreman coefficient is read from the canonical transport
record; it is not a request knob or duplicated literal.

The step has one immutable beginning state and this exact order:

1. derive stored arithmetic-mean face density and beginning MAC velocity,
   applying wall normal prescription before any gradient or norm;
2. evaluate the centered beginning-velocity gradient and retained Vreman eddy
   viscosity once per cell; combine it with the supplied molecular field and
   assemble gravity plus symmetric viscous-stress momentum RHS from that same
   snapshot;
3. trace all three remap directions from the beginning velocity. Every cell
   channel and every dual-volume momentum channel obtains its x/y/z fluxes
   from the same beginning bytes. The conservative update accumulates the
   three signed flux divergences in x, then y, then z order; no direction sees
   another direction's updated state;
4. add exactly one `dt` times the frozen nonpressure momentum RHS to the
   remapped staggered momentum; and
5. invoke the r86 projection exactly once with remapped gas density, that
   provisional momentum, and the supplied `S_div`. The projected momentum is
   the accepted P3 momentum. Scalars are not pressure-corrected.

The staggered-momentum remap uses the r84 swept-profile operator on each
component's dual control volumes. Transport velocity at a dual face is the
arithmetic average of the two beginning MAC velocities that meet that dual
face, with the boundary role inherited from the transported component's
normal direction. This construction is fixed before any remap dispatch and
uses the same shared limiter across the three momentum components. It is not
three sequential dimensional-splitting steps.

All coefficient evaluation and maps use strict binary32 and fixed index
ranges. Max diagnostics use fixed max reductions; ledger sums use padded
Blelloch trees. A finite projection validation miss publishes the full P3
state and `validation_passed=false`; malformed input, table-domain failure,
nonfinite derived arithmetic, Metal failure, or nonfinite output is structural
and publishes no state. No retry, halving, clamp, adaptive viscosity, or second
projection exists in this path.

The binding P3 gates are:

- V1 open hydrostatic rest with gravity written as `(rho-rho_ambient) g`,
  requiring exact zero momentum and unchanged scalars;
- the V2 nonzero variable-density manufactured field, including independently
  assembled gravity, viscous stress, Vreman zero/rotation/linear-gradient
  limits, nonzero `S_div`, and the r86 fp64 projection comparison;
- V3 constant preservation, translated and deforming conservative fields,
  affine/common-weight ledgers, and the smooth three-refinement L1 order gate
  inherited from P1;
- an unsplit-cross term whose x and y fluxes are both nonzero, so a sequential
  x-then-y state mutation is RED; a dual-volume momentum pulse that makes
  cell-grid remapping or component-wise limiter weights RED; and exact
  diagnostics proving one remap assembly and one projection; and
- at least eight accepted slices reconstructed from the preserved certified
  tier-10 prefix. Compare gas mass, each transported integral, momentum,
  `T_max` proxy inputs, `S_div`, and projection residual histories. Tolerances
  are frozen from certified 1-vs-N variability (zero for bit-identical integral
  ledgers) before production output is inspected.

P3 has a `200 ms` completed-call p95 allocation at the 976,272-cell tier-10
shape: 45 ms remap, 20 ms coefficient/force assembly, 120 ms projection, and
15 ms orchestration margin. Exceeding it is a design failure before P4.

Rejected alternatives are sequential dimensional splitting (changes the
multidimensional operator), post-remap momentum repair (hidden clamp),
re-evaluating Vreman after an axis update (order-dependent coefficient), using
cell-centered velocity as the momentum transport velocity (wrong dual
geometry), implicit viscosity iteration (an unbudgeted solve), and a second
projection after forces (violates the one-projection architecture).

### 7.5 P3 closure after multidimensional review (r88)

The r87 additive beginning-state x/y/z commit is rejected before
implementation. It is not a multidimensional monotone remap: an isolated
nonnegative cell with nonfolded `C_x=C_y=0.75` can lose `0.75q` through each
independently admissible one-dimensional map and land at `-0.5q`; at unit
Courants it also omits the translated corner term. A sum-Courant admission
would restore the throughput wall the production architecture exists to
remove. The ruling is therefore one fixed **palindromic split remap** using the
already certified arbitrary-Courant P1 map:

`R(dt) = R_x(dt/2) R_y(dt/2) R_z(dt) R_y(dt/2) R_x(dt/2)`.

Each submap consumes the previous submap's conservative output, so P1's
monotonicity and conservation compose without a clamp. Carrier velocities,
Vreman coefficients, molecular coefficients, boundary classifications, and
ambient tuples remain frozen from the beginning snapshot; only transported
state advances through the palindrome. This five-dispatch composition is one
P3 remap and remains symmetric for the frozen operator. A reverse-only or
three-full-step Lie split is RED. The r87 unsplit-cross fixture is replaced by
the exact diagonal unit-Courant translation and the `0.75+0.75` donor-survival
counterexample.

For momentum component `c`, the dual lattice has extents
`(nx+[c=0], ny+[c=1], nz+[c=2])`. Each directional submap transports the tuple
`(rho_face_c,m_c)` on that lattice with one common P1 limiter for those two
collocated channels; limiters are not shared across the three noncollocated
momentum components. The dual density at the beginning is the r86 stored face
density. For sweep direction `a`, the carrier at a dual flux face is:

- when `a=c`, the arithmetic average of the two adjacent beginning `u_c` MAC
  faces;
- when `a!=c`, the arithmetic average of the two beginning `u_a` MAC faces on
  opposite sides of the `c`-face.

Periodic indices wrap before averaging and the positive dual seam is a byte
copy of face zero. Boundary behavior is selected by sweep direction `a`, never
by transported component `c`:

| `a` side | dual tuple rule |
|---|---|
| periodic | wrapped tuple and carrier; opposite sides must be paired |
| wall, `a=c` | endpoint normal-momentum DOF is prescribed zero; no swept flux |
| wall, `a!=c` | even tangential tuple extension and zero normal carrier; no swept flux |
| pressure-open | accepted beginning carrier selects ambient inflow or nearest-dual-cell outflow; ambient dual density is `rho_amb`, ambient normal momentum is `rho_amb u_a` when `c=a`, and ambient tangential momentum is zero |

P3 rejects fuel-bed prescribed faces until P4. The 3x3 component/sweep matrix,
all six side orientations, mixed wall/open pairs, and canonical periodic seams
are direct fixtures with independently expected face-flux bytes. Mutations for
cell-grid momentum remap, sweep-by-component boundary selection, wrong carrier
average, and unshared dual-density/momentum limiting are RED. A commuting
fixture independently remaps dual density and requires the published momentum
velocity to use that same collocated density.

The force operator is the strict-fp32 transcription of the certified open
reference stencil, not a new discretization. Beginning cell velocity is the
average of its two MAC faces. Centered gradients use nearest-interior extension
at pressure-open sides and odd reflection about prescribed zero normal
velocity at walls; tangential wall velocity uses even extension. With
beginning gas density `rho`, `nu_eff=nu_mol+nu_vreman`, and
`mu_eff=rho*nu_eff`, the deviatoric cell stress is

`tau_ij = mu_eff (du_j/dx_i + du_i/dx_j - (2/3) delta_ij div u)`.

Its face divergence uses the exact normal difference and four-cell transverse
average/difference stencil of `BuildOpenNonpressureMomentumRHS3D`. Relative
gravity at a face is `(rho_face-rho_amb) g`, with the same arithmetic face
density used by r86. Tests bind a variable-density gravity face, off-diagonal
stress, variable `mu_eff`, rigid rotation, zero gradient, and the Vreman linear
gradient value derived from `FireSimulationTransportRecord::OpenV1()`.
Supplied molecular kinematic viscosity must be finite and nonnegative in every
cell; violation is structural. The Vreman `B_beta` expression and zero floor
use the record implementation's operation order, with per-cell ULP comparison,
not only a maximum diagnostic.

Vreman viscosity and `mu_eff` remain frozen for the step, but explicit
viscous evolution uses a deterministic stability schedule rather than a retry.
Let `nu_bound` be the maximum over momentum faces of the largest adjacent
`mu_eff` divided by that face's minimum adjacent positive density. Define

`N_nu = max(1, ceil((8/3) dt nu_bound (dx^-2+dy^-2+dz^-2)))`.

P3 applies `N_nu` forward-Euler viscous substeps of size `dt/N_nu`,
re-evaluating only the linear stress divergence on the evolving momentum with
the frozen coefficient. Thus every substep satisfies the derived deviatoric
bound `dt_sub nu_bound sum(dx^-2) <= 3/8`. Relative gravity is added once over
`dt` from beginning density after the remap and before the sole projection.
`N_nu` is a diagnostic and fixed function of accepted input bytes, not an
outcome-driven retry or tuning knob. Implicit viscosity remains rejected; a
frozen single RHS beyond the bound and an unrecorded adaptive loop are RED.

The one-projection claim is bound by more than a diagnostic scalar. The test
seam captures and hashes the sole P2 request and counts the actual public P2
invocation. A nonzero-divergence force witness independently assembles
palindrome-remapped gas density, remapped momentum plus the specified viscous
substeps and one gravity increment, and authored `S_div`; every captured P2
input field must match it. Invocation count is one, P2 reports exactly 12
V-cycles and its pinned smoother count, and accepted momentum is byte-identical
to that one P2 result. Projection-before-force, beginning-density projection,
doubled force, and a hidden second projection are RED.

The Metal P3 path owns a resident state handle. Its checked working-set query
counts the beginning/accepted conservative state, five-pass ping-pong storage,
all three dual density/momentum pairs, carrier/flux/edge/limiter scratch reused
between submaps, frozen velocity/gradient/viscosity/stress/gravity fields, and
the complete r86 projection peak. Lifetimes are interval-accounted rather than
blindly summed, but overlapping buffers are never omitted. Peak must be at most
2 GiB, with an independently computed one-buffer boundary pair. No production
step may read back a full grid between remap, force, and projection; only fixed
diagnostics leave the device. CPU publication exists only on the comparator
API and scheduled output/checkpoint paths.

The preserved-prefix contract is made executable by a pure-serialization
oracle extraction seam. Starting from the immutable step-3479 checkpoint, the
certified binary advances exactly steps 3480 through 3487 and writes a hashed
slice package containing each beginning state, dt, six boundary records,
ambient density, gravity, molecular viscosity, `S_div`, and the reference
transport/projection observables. Extraction cannot alter the trajectory and
the source checkpoint digest remains unchanged. Each metric uses the r82 rule
`B_m=max(3 sigma_m,1.25 Delta_refine_m,B_fp32_m)` and its predeclared hard
ceiling; zero 1-vs-N variability makes only `sigma_m` zero. A paired tier-6
adjacent-resolution extraction supplies `Delta_refine`; analytic accumulation
bounds supply `B_fp32`. The slice manifest freezes indices, hashes, metric
bands, and calibration inputs before production results are inspected. Zero
tolerance is retained only for identities that are algebraically byte-preserved
within the same fp32 path, never for fp32-versus-fp64 comparison.

These rulings replace r87's additive update, cross-component limiter, frozen
single viscous RHS, and selectable/zero-tolerance slice language. They do not
alter the 200 ms or 2 GiB milestone budgets.

### 7.6 P3 dual ownership and executable certificates (r89)

Review of r88 found four remaining contradictions; none reached code. First,
two nonlinear limited half-remaps have no semigroup identity, so total unit
Courant is not an exact one-cell translation through the palindrome. The exact
integer gate instead uses total `C_x=C_y=2` (and `C_z=1`): every half x/y pass
and the full z pass is an exact unit shift, giving exact translation by
`(2,2,1)`. At total `C_x=C_y=1`, the expected bytes come from an independently
composed five-pass CPU oracle; an asymmetric variable-carrier field must
distinguish the palindrome from its reverse and from a three-pass Lie split.
The `0.75+0.75` isolated donor remains a conservation/nonnegativity gate, not
an exact-translation claim. No formal-order claim follows merely from the
palindrome; the measured V3 refinement gate remains authoritative.

Second, MAC storage and dual ownership are now explicit. For component `c`,
the periodic c-normal positive seam is publication-only: exactly `N_c` unique
DOFs are remapped and the `N_c` face is copied from face zero afterward. A
c-normal wall has `N_c-1` owned interior DOFs; both endpoint planes are
prescribed zero and excluded from remap/forces. A c-normal pressure-open side
has a boundary-reservoir endpoint DOF that is carried from the beginning into
the sole projection but is not an advected dual control volume; the `N_c-1`
interior DOFs are remapped. Transverse sweeps retain those owned c-normal
planes except prescribed wall planes.

For the `a!=c` carrier average at a c-normal endpoint, the c-side ghost is:
periodic wrap; odd reflection of every velocity component at a wall (the
certified no-slip rule); and nearest-interior extension at pressure-open. A
prescribed c-wall plane wins at a corner, then an a-wall zero carrier, then an
a-open reservoir rule. Pressure-open ambient normal momentum uses the frozen
boundary carrier selected at that a-side; ambient tangential momentum is zero.
The asymmetric-line P3 kernel, not the symmetric public P1 wrapper, implements
these six-side rules while reusing the P1 reconstruction/integral/limiter
arithmetic. The fixture matrix covers every `(c,a,side)` combination (54
orientations), both signs on each open side, and mixed-side corners with
nonzero expected flux where the boundary is not prescribed.

The dual density transported beside `m_c` is **limiter-only auxiliary state**.
It provides the common collocated limiter and conservation witness during each
momentum submap, then is discarded. It is never a published velocity
denominator. After the cell palindrome, r86's arithmetic mean of remapped cell
gas density is the sole authoritative face density for viscous evolution,
gravity, projection, and accepted velocity. A non-affine density fixture must
prove the auxiliary remap and arithmetic mean differ while the captured P2
request and accepted velocity use only the latter. This resolves the former
noncommuting double authority without revising P2.

Third, wall gradients match the certified no-slip stencil: all velocity
components use odd reflection about zero at an adiabatic wall; pressure-open
uses nearest-interior extension. During every viscous substep, evolving
momentum is converted to velocity with the single frozen authoritative face
density derived from remapped cell gas density. `mu_eff` stays frozen.

The old constant-coefficient `3/8` shortcut is withdrawn. The implementation
assembles the actual strict-fp32 linear viscous operator—including variable
`mu`, frozen face-density division, odd/Neumann ghosts, and the four-cell
transverse stencil—and simultaneously accumulates each row's absolute
coefficient sum. Energy dissipation of the symmetric-stress divergence makes
its spectrum nonpositive; Gershgorin gives `|lambda|<=Lambda_inf`, the maximum
row sum. The exact schedule is

`N_nu = max(1, ceilf((dt * Lambda_inf) * 0.5f))`,

with the written binary32 association. It enforces
`dt_sub Lambda_inf <= 2`. Nonfinite product, integer overflow, or `N_nu>8` is
an unsupported production request and fails structurally before dispatch. The
eight-substep ceiling comes from the 20 ms P3 force allocation and must be
confirmed by the tier-10 stress-kernel p95 before P3 closes; otherwise a new
design revision is required. A strict-fp32 `N_nu>1` fixture independently
checks the integer, every intermediate momentum digest, frozen density and
`mu`, and single gravity addition. A separately assembled matrix/eigenvalue
witness verifies the row-sum bound for variable coefficients and wall/open
ghosts.

Fourth, the production invocation is a new **resident P2 seam** operating on
the owning P3 Metal buffers and encoding the same r86 hierarchy and twelve
cycles into the existing command sequence. It performs no full-grid upload or
readback. The standalone public P2 function becomes a comparison wrapper that
uploads, calls that resident seam, and publishes after completion. The P3 test
counter binds resident projection encodes/V-cycles, not wrapper calls; the
captured request digest is computed on-device through fixed trees. A process-
wide test counter also catches a discarded hidden solve. The allocation ledger
records every real Metal allocation and lifetime high-water mark; it must equal
the independent interval query on the nearest admitted shape. A one-buffer
mutation changes the boundary result. Encoder capture rejects any full-grid
blit/readback between P3 stages.

The oracle slice package schema is fixed. Each of steps 3480--3487 contains
beginning conservative/MAC bytes, dt, six boundaries, ambient density,
gravity, molecular viscosity, `S_div`; certified advection flux/integral
observables; per-cell Vreman/`mu`; viscous and gravity momentum increments; the
isolated projection operand/result; and mass/species/enthalpy/momentum,
`T_max`, EOS, and projection-residual metrics. Source/chemistry/pilot/radiation
increments are separately serialized and excluded from the isolated P3 metric
surface. Package dt/Tmax/EOS sequences and eight frame digests must equal the
already pinned r80 continuation, proving instrumentation transparency.
`B_fp32` is generated from `gamma_n` accumulation bounds using enumerated
operation counts and absolute operand sums; `Delta_refine` compares
volume-normalized physical integrals and restriction-matched fields from the
paired tier-6 adjacent resolutions. The manifest stores those inputs, formulas,
and outputs, not only final allowances.

These corrections supersede r88's unit-Courant exactness, uniform dual-grid,
dual-density authority, tangential free-slip wording, `3/8` shortcut, public
P2 invocation, and self-certifying allocation/slice evidence. The palindrome,
fixed 3480--3487 extraction, r82 bands, 200 ms, and 2 GiB limits stand.

### 7.7 P3 outward stability and independent evidence (r90)

The r89 phrase “reverse palindrome” is deleted: reversing
`x/2,y/2,z,y/2,x/2` is the same sequence and no fixture can distinguish it.
The ordering mutant is the axis-reversed palindrome
`z/2,y/2,x,y/2,z/2`; the second mutant is the three-full-pass Lie split.
Before either is used as a RED, the independent CPU composition must prove its
bytes differ from the canonical result on the asymmetric variable-carrier
fixture.

The viscous schedule is an outward certificate over stored fp32 coefficient
bytes. Each coefficient is promoted exactly to binary64. In fixed column order,
the row accumulator performs `nextafter(sum+abs(a_ij),+infinity)` after every
addition; the maximum of those outward row bounds is `Lambda_up`. Schedule
work is `nextafter((double(dt)*Lambda_up)*0.5,+infinity)`, with the written
association, and the first candidate is `max(1,ceil(work))`. The stored fp32
`dt_sub=dt/float(N_nu)` is then certified directly: evaluate
`nextafter(double(dt_sub)*Lambda_up,+infinity)`. While that value exceeds two,
increment `N_nu` deterministically and recompute `dt_sub`; this is
pre-dispatch integer selection, not an attempted physical step or retry. Any
nonfinite intermediate, integer overflow, or required `N_nu>8` fails before
encoding. Thus the represented substep, not an ideal quotient, satisfies the
bound. REDs straddle every `N=1..8` ceil threshold with `nextafter`, include a
long-row downward-rounding mutant, exercise upward-rounded fp32 division, and
bind the 8/9 structural edge.

Boundary coverage is an explicit bitmap indexed by transported component,
sweep axis, side, and role `{periodic,wall,open_inflow,open_outflow}`. Every
semantically applicable entry must be visited; open signs are separate. Raw
nonzero wall endpoint input must distinguish prescription from a skipped
branch, and transverse open fixtures pair zero ambient tangential inflow with
nonzero nearest-interior outflow so both role mutations have nonzero deltas.
Mixed-side corners are an additional matrix, not substitutes for bitmap cells.

A c-normal pressure-open endpoint is excluded only from the c-normal sweep.
It remains an owned half-support plane for transverse sweeps, using the pinned
c-open nearest-interior carrier ghost. During force application it receives
the certified relative-gravity increment formed from interior/ambient face
density, receives no viscous endpoint increment (the reference stress
divergence updates interior normal faces only), and then participates in r86
projection. A c-wall endpoint is prescribed zero through every sweep and force.
These rules remove the former ambiguity between “reservoir” and transverse
transport; mixed-corner tests bind both paths.

Calibration operation counts are not self-authored manifest values. Every
kernel has a test-only trace build that counts executed fp32 arithmetic by
operation kind and emits fixed-reduction depths. A separate host topology
oracle walks the declared index/reduction graph without consuming the manifest
and must reproduce those counts and depths. The bound generator consumes the
trace/oracle-agreed count and independently accumulated absolute operands.
It rejects `n*epsilon>=1`, any disagreement, and any generated `B_fp32` or
final `B_m` above its already declared scientific ceiling before production
output is available. A high-cancellation RED removes one accumulation level;
the observed fp32-versus-high-precision error must then exceed the mutated
bound. These traces are test instrumentation only and cannot select runtime
work.

All full-grid buffers on the resident P3 path use
`MTLResourceStorageModePrivate`. Shared storage is limited to fixed-size
diagnostics and explicit scheduled staging for checkpoint/frame publication.
The standalone P1/P2 wrappers may retain Shared staging around their resident
kernel calls, but no Shared full-grid buffer survives into P3 interstage
ownership. Thus direct CPU dereference cannot bypass the encoder/readback
capture. The allocation ledger classifies every resource by storage mode and
the gate rejects a resident full-grid Shared allocation.

The certified-prefix extractor obtains a source-free comparison through a
discarded **zero-source shadow**, not algebraic subtraction. At each immutable
beginning state for steps 3480--3487 it executes the certified transport and
projection with all chemistry/radiation/pilot/phase packets identically zero,
using the accepted dt, boundary, molecular, gravity, and `S_div` operands, and
serializes that shadow's isolated observables. The shadow has separate storage,
never feeds the accepted trajectory, and cannot publish a run checkpoint. The
ordinary sourced continuation runs independently and must reproduce the pinned
r80 dt/Tmax/EOS/frame evidence, proving instrumentation transparency. “Pure
serialization” applies to capturing either result, not to pretending the
nonlinearly coupled source can be subtracted afterward.

r90 changes certificate evaluation and RED definitions only. It does not
change the P3 physical operator, case identity, or milestone budgets.

### 7.8 P3 executable Metal preflight and isolated shadow (r91)

Apple Metal has no native fp64, so r90's per-row binary64 walk is replaced by
an executable split certificate. The resident kernel forms and stores the
actual fp32 operator coefficients, then accumulates each row in fixed column
order as `nextafter(sum+abs(a_ij),+infinity)` in fp32. Coefficient formation is
not reinterpreted: the certificate bounds the exact stored bytes the operator
will consume. A fixed outward max tree reduces those row bounds to one fp32
`Lambda_up` scalar in the Shared diagnostic block. Only that scalar is read by
the host. The host promotes `dt` and `Lambda_up` exactly to binary64 and performs
r90's outward work/ceil and represented-fp32-`dt_sub` checks. A high-precision
host oracle independently bounds every stored row in fixtures. Source/build
guards require the private coefficient buffers, forbid their blit/readback,
and bind the sole scalar diagnostic transfer.

The eight-substep capability is timed at its actual edge. A tier-10-shaped
manufactured coefficient field must select exactly `N_nu=8` under the outward
preflight; completed-call p95 for coefficient/operator construction, outward
row/max reduction, scalar synchronization, all eight viscous updates, and one
gravity addition is at most 20 ms. An `N=7` control executes seven updates; the
nextafter-adjacent `N=9` control fails structurally before any update dispatch.
Timing the natural methane field at a smaller `N` cannot qualify the cap.

The isolated certified shadow removes the fuel bed as well as source packets.
For its P3 comparison only, every `bottomFuelMask` face replaces the separate
`FuelInletBoundary3D` constituent/enthalpy tuple with bit-zero flux and inherits
the underlying adiabatic-wall/no-slip geometry. The supplied shadow `S_div`
operand remains the serialized P3 input; it is not recomputed to disguise the
exclusion. Original bed mask, fuel record, injection temperature, and mass-flux
bytes are serialized separately as excluded P4 evidence. A nonzero-bed fixture
requires ordinary continuation ledgers to contain prescribed fuel/enthalpy
flux while every corresponding shadow boundary flux byte is zero; restoring
the bed tuple changes the isolated flux and integral digests. The sourced
continuation still cross-binds r80.

r91 makes the r90 certificates executable on the target and closes P4 boundary
leakage from the P3 oracle surface. It changes no case identity, operator, or
budget.

### 7.9 Line-resolved pressure-open ambient tuples (r92)

The one-dimensional P1 kernel accepts an optional boundary-owned ambient tuple
for every `(component,line)` on each side, indexed exactly as
`component*lineCount+line`. The lower and upper arrays are distinct. When this
mode is disabled, both sides consume the original component-only ambient tuple
and the settled P1/P3 bytes remain unchanged. Reconstruction ghosts and swept
extensions read the same selected tuple; CPU and Metal do not restate the
selection differently.

This seam is required by r89's dual-momentum boundary table. At a normal
pressure-open face the ambient momentum is `rho_amb*u_boundary`, and the frozen
boundary carrier can vary along the side; one component-wide value therefore
cannot represent the ruled operator. Each side also has an independent role,
so one shared line tuple cannot represent simultaneous lower/upper inflow.
Fixtures assign distinct exact-binary values to every component, line, and
side, bind both boundary fluxes, and compare the Metal bytes with the fp32 CPU
oracle. Incomplete or nonfinite arrays fail before dispatch.

The r92 implementation review also closed an older r85 accounting mismatch.
One purported below-cap request was 936 requested bytes under two GiB but
147,456 bytes over after the M4's per-buffer allocation rounding. P1 therefore
computes its certificate by rounding each of its eleven Metal buffers outward
to 16 KiB, then sums every runtime `allocatedSize` and requires the measured
total to be no greater than both that certificate and two GiB before creating
or committing a command buffer. The result is assembled privately and moves to
the caller only after finite-output validation; persistent host allocation
failure returns the complete default result.

Using a side average was rejected because it changes the conservative flux;
launching one remap per line was rejected because it defeats the resident
batched kernel and command budget; and inserting ambient values into the
transported state or limiter stencil was rejected because boundary data are
not owned conservative state. r92 is an internal representation closure for
the already pinned P3 operator. It changes no case identity, physical model,
or milestone budget.

### 7.10 Metal frozen-force comparison bound (r93)

The standalone Metal frozen-force comparison is gated per published cell and
face by ordered-binary32 ULP distance. On the M4 target, the complete
zero-flow, nonlinear periodic, all-axis wall/open, all-axis periodic,
variable-density authority, and deviatoric-compression fixture surface was
measured against the strict-fp32 CPU oracle. Zero flow, periodic publication,
and same-device repetition were bit-identical. The largest nonzero CPU/Metal
difference was 8 ULP, at the authority fixture's x-viscous face index 25
(`0.778665` CPU versus `0.778666` Metal at printed precision). The binding
comparison ceiling is therefore 8 ULP for every Vreman, `mu_eff`, viscous, and
gravity value; exact analytic zeros and canonical seam copies remain exact-byte
gates.

The former `3e-5*max(1,|a|,|b|)` comparison is rejected because it becomes a
`3e-5` absolute allowance below unity and can hide millions of ULPs in small
viscosity and force values. Requiring byte identity everywhere is rejected
because the measured eight-ULP target/compiler difference is finite,
repeatable, and confined to an otherwise matching strict operation topology;
it would reject the intended operator rather than distinguish a model change.
A maximum-only diagnostic is rejected because it can hide a bad cell behind a
good aggregate. Source gates separately bind Vreman beta association and
accumulation order, the exact `2/3` deviatoric coefficient, all twelve live
Metal buffers, and every command/encoder fail-closed stage. This is a measured
comparison rule for the already pinned operator, not a physical-tolerance,
identity, case-record, or budget change.

### 7.11 Resident force certificate and transfer boundary (r94)

The resident force path remains matrix-free. At the tier-10 shape
(`86 x 86 x 132`, 2,958,916 MAC faces), materializing even 27 fp32
coefficients per output face would require 319.56 MB; 54 coefficients require
639.13 MB and 96 require 1.136 GB. Those arrays are not consumed by the
viscous update itself and would overlap the resident state and projection
hierarchy under the two-GiB cap. The r91 phrase "stores the actual fp32
operator coefficients" is therefore narrowed to the coefficients already
stored by the matrix-free operator: authoritative face density, frozen
`mu_eff`, grid scale, and boundary-role bytes. No expanded sparse matrix is
materialized.

The stability preflight uses the analytic global row envelope of this fixed
symmetric-stress stencil,
`Lambda_up = 24 * max(mu_eff) * max(1/rho_face) / dx^2`. The factor 24 is the
coefficient-count bound after the two face-to-cell averages, centered/ghosted
gradient, symmetric deviatoric combination, and face stress difference are
expanded; wall removal can only reduce a row, while periodic wrap and nearest
pressure-open extension never exceed the same absolute coefficient sum. The
GPU forms `nextafter(1/rho,+infinity)`, the fixed-tree maxima, outward `1/dx`,
and each multiplication in the displayed order with
`nextafter(...,+infinity)`. A separate small-grid host oracle assembles the
signed strict-fp32 matrix column by column for periodic and mixed wall/open,
variable-density/viscosity cases and requires every exact absolute row sum to
be no greater than this scalar. Per-face dependency-envelope storage was
considered and rejected with sparse materialization: it adds bandwidth and
state without tightening the capability edge enough to justify it. Threshold
and N=7/8/9 fixtures remain those of r90--r91. The envelope may select more
work than the exact row sum, but it may never select less; exceeding eight is
a structural capability failure before any update.

Residency is measured, not inferred from output agreement. The preflight may
publish exactly one fixed-size `Lambda_up` diagnostic before scheduling. The
substep command then performs all selected viscous updates, boundary
publication after each update, the single relative-gravity addition, and the
single resident projection without a device-to-host transfer or host-visible
full-grid resource between those operations. Per-substep oracle taps remain
Private snapshots and are staged together only after command completion at the
step boundary; the host then applies the existing canonical FNV-1a momentum
digest to those bytes. Production execution omits the snapshots. A
call-scoped allocation/encoder ledger must report the exact scalar transfer,
zero loop transfers, one terminal diagnostic/publication staging event, the
selected substep count, and exactly one projection invocation.

The measured starting point on the target is 11.3556 ms device / 23.6152 ms
completed-call p95 for the standalone projection, 21.1372 / 35.2078 ms for the
tier-10-shaped five-pass conservative remap, and 2.69608 / 17.2882 ms for the
standalone tier-10 P1 remap. These numbers preserve the credibility of the
20 ms force, 120 ms projection, and 200 ms P3 allocations; the resident
eight-substep and combined measurements still have to pass their own gates.

An expanded sparse matrix was rejected because it duplicates a matrix-free
operator and consumes hundreds of megabytes to more than a gigabyte of the
resident peak. A global hand-tuned viscosity margin was rejected because it
would not prove the boundary/variable-density operator. Reading every
intermediate momentum field after each substep was rejected because it makes
the oracle tap change the architecture being validated. GPU serial FNV was
rejected because it adds a single-lane full-grid pass to every substep; a
terminal test-only staging of Private snapshots preserves both the canonical
digest and the resident production schedule. r94 changes only the executable
form of the r90--r91 stability/evidence certificate. It does not change the
force operator, case identity, validation bands, or milestone budgets.

### 7.12 Eight-substep composed force comparison (r95)

The per-kernel r93 ULP ceiling is not applied blindly through cancellation.
On the strict N=8 periodic, variable-momentum resident fixture, the maximum
absolute CPU/Metal difference after all eight frozen-viscosity updates and the
terminal gravity addition is exactly `2^-25 = 2.98023223876953125e-8`. Where
the result magnitude is at least `1e-5` during characterization, the measured
maximum is 2 ULP, below the naive `8 * 8 = 64` ULP accumulation ceiling. The
raw ordered-ULP maximum is 1,614,348,289 only because one cancellation cell is
`-4.47471e-10` on CPU and `+7.09406e-10` on Metal; treating that sign crossing
as a billion-ULP physical error is numerically meaningless.

The composed comparison therefore accepts each non-analytic output only when
either its ordered-binary32 distance is at most 64 ULP or its absolute
difference is at most `2^-25`. This is a disjunction, not a magnitude switch,
so no chosen near-zero threshold enters the runtime or the gate. Analytic
zeros, prescribed wall values, and periodic publication seams retain exact-byte
requirements. The gate applies to every final momentum face, and the eight
Private intermediate snapshots retain their canonical FNV evidence after the
single terminal staging event.

Increasing r93's per-kernel 8-ULP ceiling was rejected because the individual
kernel evidence did not move. Using raw ordered ULP alone was rejected because
it is singular across zero and would report a sub-nanounit sign crossing as a
billion-ULP failure. Using only an absolute band was rejected because it would
weaken ordinary values up to magnitude 14.4414, where the measured composed
difference is only 2 ULP. The r95 bound is comparison evidence for the fixed
N=8 composition; it changes no state bytes, operator, validation tolerance,
case identity, or budget.

### 7.13 Mixed-boundary composed force bound correction (r96)

The r95 periodic characterization was not universal. A deterministic matrix
of 160 exact-N8 cases now crosses ten boundary classes (periodic, all-open,
and all eight lower-wall/lower-open mixed masks), two Vreman coefficients
(`0` and `0.07`), and eight phase shifts of variable cell density, molecular
viscosity, face density, and three-axis momentum. The matrix observed 20 faces
outside r95. Its maximum ordered distance was 8192 ULP at an absolute drift of
`2^-25`; its maximum absolute drift was `2^-21` at only 8 ULP. Among faces
over the retained 64-ULP ordinary-value ceiling, the maximum absolute drift
was exactly `2^-23`.

The corrected composed comparison is therefore `ULP <= 64 OR abs <= 2^-23`
for every non-analytic final momentum face. The load-bearing witness is mixed
`x wall/open, y open/wall, z wall/open`, phase 4, `Cv=0`, y-face 18: CPU
`-0x1.6c96p-10`, Metal `-0x1.6c9ep-10`, 1024 ULP and exactly `2^-23`
absolute drift, with `Lambda_up=83.52005767822266 s^-1` and represented
`dt=0.17959757149219513 s`. Analytic zeros, wall prescriptions, and periodic
publication seams remain exact-byte requirements. The alternative
`ULP <= 1024 OR abs <= 2^-25` was rejected because it weakens the ordinary ULP
evidence rather than extending only the cancellation-safe branch. This is an
empirical oracle-comparison correction; it changes no kernel arithmetic,
accepted state, runtime validation band, identity, or budget.

### 7.14 Resident force--projection transaction and golden P2 gate (r97)

The production transaction now carries authoritative gas density, packed MAC
momentum, and the authored divergence target in Private Metal buffers from the
frozen-force preflight through all eight viscous updates, one relative-gravity
addition, and exactly one P2 projection. The projection accepts byte offsets
into the force-owned packed momentum allocation; it does not upload or
reconstruct those faces on the host. The only pre-update device-to-host value
is the fixed-size outward `Lambda_up` scalar needed to select the represented
substep schedule. No full-grid host access occurs between force and projection.
One terminal staging event publishes the projected state and diagnostics at
the step boundary.

This absence is observed at the command/access seams rather than authored as a
zero. Thread-local counters surround every command commit, every Metal
`contents` access, and every blit copy in the two translation units. Transfer
wrappers classify an actual Private-to-host-visible copy by phase; raw blit,
commit, and host-access primitives are globally source-bound to those wrappers.
The resident owner requires
one projection invocation, zero interstage full-grid reads, zero projection
uploads, and one terminal stage; injected interstage access and a hidden second
projection both fail with a completely default result. The transfer RED encodes
a real Private-to-Shared full-grid blit before the projection and proves that
the observed ledger rejects it atomically. The projection's
rounded working-set certificate counts the resident provisional-state stage,
and its actual ledger counts the force-owned packed allocation once rather than
once per axis view. The conservative combined certificate is evaluated before
target payload validation or any Metal allocation; the first adjacent shape
over two GiB therefore rejects without allocating or committing either stage.

On the exact tier-10 shape (`86 x 86 x 132`) with the certified `N_nu=8`
stress coefficients, repeated five-call landing runs measured
42.8205--47.4189 ms device p95 and 66.1993--71.5982 ms completed-call p95. The
conservative
combined certificate was 524,688,024 bytes and the observed combined allocation
ledger was 512,093,336 bytes. The same request is an analytic rest solution:
every published pressure, momentum, and velocity value is exact `+0` and every
periodic seam is the canonical publication copy. Together with the measured
20.7379--20.8456 ms / 35.3388--36.1283 ms tier-10 conservative palindrome,
this leaves material room inside the 200 ms P3 step allocation and keeps the
one-hour tier-10 target
credible before P4 is added.

The projection oracle gate retains the immutable step-3479 checkpoint at SHA-256
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`.
Because the certified and production pressure-open equations intentionally have
different active-set semantics, the shared periodic subset is used: the gate
loads the checkpoint's accepted gas-density and MAC-velocity bytes, applies a
fixed analytic periodic pressure perturbation, and runs the production fp32
fixed-work projection and the independent certified fp64 adaptive projection.
The production residual fell from exact `0x1.3ap-11` =
`5.98907470703125e-4 s^-1` to exact `0x1.48p-15` =
`3.910064697265625e-5 s^-1`. The fp64 oracle began at
`6.035506397530279e-4 s^-1` and finished at
`6.708440414004929e-5 s^-1`, so the production/oracle residual ratio was
0.5828574834 and the ratio of their post/pre reduction factors was
0.5873762212. The maximum projected-velocity difference was
`1.2527614002610932e-5 m/s`, below the already pinned `3e-5 m/s` P2 comparator,
and the production residual was far below the independently evaluated
`1.1504455730808001e-2 s^-1` validation band. Thus the measured fp32 floor fits
the validation contract; no tolerance change is authorized or needed.

r97 changes the device ownership seam and evidence only. It changes no force or
projection arithmetic, case identity, validation tolerance, or oracle bytes.

### 7.15 Resident ownership and bidirectional transfer closure (r98)

The resident P2 seam accepts only full-grid `MTLStorageModePrivate` resources.
Density and divergence-target buffers must cover the complete cell payload;
all three momentum views must name one packed allocation, use the canonical
axis byte offsets, and fit wholly inside that allocation. Density, target, and
packed momentum are distinct owners. These checks run before projection
context creation, command allocation, or invocation accounting. Shared,
short, aliased, or noncanonical inputs therefore fail with a default result
rather than silently weakening residency or the two-GiB ledger.

Transfer classification is phase-scoped, not supplied as a label by each copy
call. Upload and terminal-staging scopes are explicit; every other force or
projection blit is structurally interstage. During that interval, a copy is a
forbidden full-grid transfer whenever exactly one endpoint is host-visible,
so both Private-to-Shared staging and Shared-to-Private re-upload are observed.
Independent injected blits in both directions fail atomically. Allocation
wrappers likewise verify the requested storage mode and record every created
buffer's actual `allocatedSize`. At publication, the recorded allocation count
and byte sum must reproduce the complete independently enumerated resident,
upload, borrowed, and terminal-stage ledger. This prevents either a new hidden
allocation or an omitted ledger operand from preserving a false certificate.
Every named force and projection work role is also checked against its required
Private or Shared mode, rather than inferring residency from the allocation
factory's name. Source gates admit raw Metal allocation primitives only inside
the observing factories, and the composed force owner repeats its exact
count/byte reconciliation immediately before result publication.

The repaired exact tier-10 N=8 transaction measured 42.8285 ms device p95 and
66.8613 ms completed-call p95, with the unchanged 512,093,336-byte observed
ledger below the unchanged 524,688,024-byte conservative certificate. The
standalone resident projection measured 10.5484 ms device p95 and 25.1084 ms
completed-call p95. r98 changes only ownership, observation, and failure
evidence; arithmetic, accepted bytes, r96 comparison bounds, validation
tolerances, checkpoint identity, and the 200 ms/two-GiB budgets are unchanged.

### 7.16 Golden divergence steady-state floor (r99)

Before resident transport composition, the immutable step-3479 golden state is
reprojected through sixteen complete resident force--projection transactions.
The divergence target, gas density, accepted MAC state, dt, and periodic
geometry are the same bytes as the r97 oracle fixture. The force leg is
intentionally neutral (`nu_mol=0`, `Cv=0`, and `g=0`), so it still exercises
the resident force handoff and its single P2 invocation without conflating the
projection recurrence with an evolving physical RHS. Every cycle selects one
viscous substep, observes zero interstage transfers, invokes P2 exactly once,
and passes monitored validation.

The exact fp32 residual sequence is

`{0x1.46ep-15, 0x1.48p-15, 0x1.0p-15, 0x1.ea8p-16,
0x1.ea48p-16, 0x1.48p-16, 0x1.47p-16, 0x1.ea5p-16,
0x1.48p-16, 0x1.ea4p-16, 0x1.48p-16, 0x1.47p-16,
0x1.ea4p-16, 0x1.47p-16, 0x1.47p-16, 0x1.48p-16}` s^-1.

The maximum is therefore `0x1.48p-15 = 3.910064697265625e-5 s^-1`,
equal to the already accepted first-pass r97 floor, and the sixteenth cycle is
`0x1.48p-16 = 1.9550323486328125e-5 s^-1`. The recurrence settles into a
bounded deterministic oscillatory floor rather than accumulating divergence.
The fixture pins the complete sequence, the maximum, and the final ceiling;
an upward creep is a contract failure and cannot trigger dt changes or extra
projections. r99 adds evidence only and changes no operator or validation band.

### 7.17 Resident transport and full-step ownership (r100)

The production step order is `force -> advection -> sources -> projection`.
The force leg advances the beginning MAC momentum through its selected frozen-
viscosity substeps and one relative-gravity addition. The cell palindrome then
advects the beginning nine-channel conservative tuple, while the dual-grid
palindrome advects the force-updated momentum. Both remaps use the beginning
accepted MAC velocity as their frozen carrier for all five submaps
`x/2,y/2,z,y/2,x/2`; force does not silently change the carrier inside the
step. This preserves the settled semi-implicit transport contract while making
the owner-directed operator order executable.

The dual-grid auxiliary face density remains a limiter companion only. After
transport, source maps add exactly once to the cell tuple and MAC momentum, and
the sole r86 projection reconstructs its authoritative arithmetic face density
from the post-source cell gas density. No independently transported face
density enters the projection. The resident source seam accepts explicit fp32
cell and face increments. During this milestone they are exact zero for the
discarded zero-source shadow; the following thermo/source-map milestone owns
their nonzero construction. Zero is therefore an authored comparison operand,
not an omitted production stage.

All full-grid state, frozen carriers, line scratch, dual tuples, source maps,
and projection operands are Private Metal resources between the initial upload
and terminal oracle/diagnostic tap. The resident cell and dual remaps may commit
commands, but may not stage, dereference, or re-upload a full grid. A scoped
transfer/allocation ledger observes both host-visible directions, every command
commit, every terminal tap, and the exact live high-water allocation set. The
combined two-GiB certificate is checked before payload access or Metal work.
Standalone wrappers retain one upload and one terminal staging event solely to
compare the resident kernels with the strict CPU production oracle.

Transport comparison changes from ULP evidence against the certified solver to
physics evidence because its FCT operator is intentionally different. The
golden zero-source shadow must preserve each conservative integral within a
predeclared fp32 reduction bound, remain within the certified local envelope at
the pilot-ring and plume-edge steep-front probes, and report velocity/scalar
deviations against bands generated from the certified solver's thread and
adjacent-refinement variability under section 5.1. No production output may be
inspected before those bands are frozen. The complete resident step is then
measured over the fixed eight slices 3480--3487; any composed bound wider than
the constituent evidence receives its own revision rather than being assumed
to be the naive sum.

Acceptance remains monitored: every deviation is accumulated in step metadata
and evaluated by the validation campaign. It never retries, halves dt, adds a
projection, clips a field, or stalls a run. r100 changes the resident ownership
and composition seam only; it does not change case identity, checkpoint bytes,
the certified oracle, or an existing validation tolerance.

### 7.18 Resident mixed-boundary dual transport (r101)

The general wall/open dual oracle now has an executable resident Metal seam.
At the step boundary, the nine frozen `(transported component,sweep axis)`
carrier arrays and their distinct lower/upper line-resolved ambient tuples are
packed once and uploaded to Private storage. The resident interval accepts the
force leg's canonical packed Private face-density and momentum allocations,
prescribes component-normal wall planes, then executes all fifteen dual
submaps in one ordered command. Generic gather/scatter kernels implement the
same owned-line coordinates as the 54-role CPU oracle; open endpoints retain
their transverse ownership and periodic components receive one terminal
canonical seam copy. The output is one packed Private momentum allocation with
the r98 canonical offsets, so P2 needs no repack or upload.

The standalone wrapper observes one resident command, zero resident host
accesses, deterministic repeated bytes, and agreement with the independent
mixed wall/open strict-fp32 oracle under the existing production Metal
comparator. Its working-set certificate counts 27 frozen carrier/ambient
buffers, two borrowed and two atomically published packed face allocations,
seven maximum-line scratch allocations, and every command-retained parameter
buffer with 16-KiB outward rounding. The exact-cap witness is
`80 x 195 x 1000`, `2,147,483,648` bytes; the adjacent `z=1001` witness is
`2,149,646,336` bytes and is rejected before payload access or Metal work.
This revision changes residency and publication ownership only; the r89 dual
operator, r92 side tuples, fp32 arithmetic, case identity, and validation bands
are unchanged.

### 7.19 First full resident step and measured budget (r102)

The first full resident owner now executes frozen force, the nine-channel cell
palindrome, mixed-boundary dual momentum, explicit source operands, and exactly
one r98 projection in that order. Source operands are Private full-grid buffers
and are required to be exact positive zero for this isolated-shadow milestone;
nonzero thermo/source maps remain the next numbered stage. The source command
adds both cell and packed-face operands, then forms the sole P2 gas-density
input by summing record-ordered gas constituent channels CH4 through CO in
strict component order. `rho_tot Z`, carbon aerosol, and sensible enthalpy are
not density aliases. Observed child diagnostics, rather than authored zeros,
must report five cell submaps, fifteen dual submaps, one source command, one P2
invocation, and zero interstage full-grid transfer. Cell and transported-dual
oracle taps occur only after P2's terminal step-boundary publication.

On the exact `86 x 86 x 132`, nine-channel, N=8 workload, resident device p95
is `73.8315 ms`. The validation wrapper, which deliberately rebuilds frozen
line tuples and stages full oracle outputs at both step boundaries, measures
`242.727 ms` wall p95. Thus the production interval clears the 200 ms P3
allocation, and even the instrumented wrapper remains below the 300 ms/step
one-hour tier-10 envelope. Its conservative combined certificate is
`1,235,662,396` bytes; the independently summed observed upper bound is
`975,303,556` bytes. A mixed wall/open fixture independently composes the CPU
force, cell remap, dual remap, zero source, and P2 operators and gates the full
resident result. r102 changes no numerical kernel, validation tolerance, case,
or checkpoint.

### 7.20 Authoritative gas-density extraction (r103)

The first golden-state packing audit rejected r102's provisional use of
component zero as projection density. The authoritative nine-channel tuple is
`rho_tot Z`, seven record-ordered constituent densities, and sensible
enthalpy; `rho_tot Z` is a mixture-fraction numerator, not gas mass. The source
command therefore first updates all nine channels, then a separately ordered
kernel forms gas density as `CH4 + O2 + N2 + CO2 + H2O + CO` from components
one through six. Carbon aerosol, `rho_tot Z`, and enthalpy never enter P2's
density operand.

The owner validates the same strict-order sum against the force leg's beginning
gas-density bytes before Metal work. The resident RED uses deliberately
different `rho_tot Z` and gas-density values; both the independent CPU
composition and Metal P2 consume the six-constituent result. On the corrected
exact tier-10 tuple the full resident device p95 is `72.6851 ms`, the staged
validation wall p95 is `243.495 ms`, and the existing resource certificate is
unchanged. r103 repairs operand identity only; it changes no transport,
projection, tolerance, case, or checkpoint byte.

### 7.21 Pressure-open multigrid stabilization and golden floor (r104)

The first SHA-bound golden composition slice exposed a fixed-schedule failure,
not an fp32 floor: the r86 pressure-open solve entered with a
`37.872447967529297 s^-1` residual and twelve undamped cycles published
`0.4867178201675415 s^-1`, `42.663082095` times its unchanged
`0.011408407366927702 s^-1` validation band. An exact one-through-sixteen stop
sweep found the best undamped published iterate at cycle four
(`0.15998798608779907 s^-1`) and strict worsening thereafter. Adding further
undamped work is therefore rejected.

Pressure-open hierarchies now damp each rediscretized coarse correction by the
stored binary32 `0.75f` and execute exactly sixteen cycles. The measured
published sequence at 8/12/16 cycles is
`0.087220191955566406`, `0.019500255584716797`, and
`0.0051319599151611328 s^-1`; it is monotone through the fixed stop and crosses
the existing band without changing tolerance, adding a projection, or reading
the residual at runtime. Periodic/wall-only hierarchies retain twelve undamped
cycles: the full manufactured suite preserves its `1.15506` fp32/fp64 oracle
ratio and prior CPU/Metal deltas. A rejected global-damping experiment degraded
that periodic ratio to `1.4718`, so damping is an operator-class rule rather
than a case-wide knob.

The fixed step-3480--3487 beginning-state chain is SHA-bound independently.
Its exact post-projection trace is
`{0x1.5054p-8,0x1.4704p-8,0x1.4cd4p-8,0x1.4af4p-8,`
`0x1.49bp-8,0x1.4b28p-8,0x1.49c8p-8,0x1.4954p-8} s^-1`;
the maximum is `0.0051319599151611328 s^-1`, every slice passes its authored
band, and no upward creep appears. This is the r104 golden pressure-open floor.
It is projection evidence only: transport/scalar/velocity tolerances remain
unaccepted until the section-5.1 calibration package is independently sealed.

The exact tier-10 N=8 composed interval remains `73.0817 ms` on device. The
corrected allocation observer now includes the full-step owner's buffers and
reports `1,233,957,860` bytes below the `1,235,662,396`-byte certificate. The
public oracle wrapper, which uploads and terminal-stages the complete state on
every call, measured `303.829 ms` in the qualification run and `402.264 ms` in
a later cold/contended review run. It is diagnostic-only and has no acceptance
cap because it does **not** certify the `300 ms/step` one-hour production wall budget.
That wall contract belongs to the persistent multi-step driver, where ordinary
steps do not perform the full oracle tap. The 200 ms resident-device budget is
green; the one-hour end-to-end wall gate remains open and is not widened.

### 7.22 Full resident composition evidence and calibration stop (r105)

The full owner now performs its combined `2 GiB` admission before payload
inspection, validates the complete frozen-force request before the first Metal
command, observes every owner and child allocation, and validates every staged
cell scalar, auxiliary face density, and dual momentum before atomic
publication. The exact tier-10 certificate remains `1,235,662,396` bytes and
the independently bound observed allocation remains `1,233,957,860` bytes.

The SHA-bound steps 3480--3487 campaign executes the complete resident shadow
with one projection and zero interstage full-grid transfers on every slice.
The independently recomputed production post-projection residual trace is
`{0.0051404458276261911, 0.0049875522159004513, 0.0050794154282387217,`
`0.0050499014713932307, 0.0050310902116719207, 0.0050479588015629375,`
`0.0050315443766649108, 0.0050194765847817882} s^-1`. There are zero
validation misses and no upward creep. The oracle and production enter with
nearly identical `37.85--38.00 s^-1` residuals; production's reduction factor
is `14.37360898839597--14.750826431310701` times weaker than the oracle's,
while remaining inside the unchanged production validation band.

The two fixed steep-front probes retain exact nonzero beginning-state local
contrasts `0.30206703454394657` (plume edge) and `363818.26731442177 J/m^3`
(pilot ring), with zero local overshoot and no new density undershoot. Across
the eight slices, the observed production/oracle differences are scalar
absolute `833.57847605185816`, scalar relative `0.0021660226012641007`,
velocity absolute `0.078576087059462285 m/s`, and conservation-ledger relative
`1.8370760700895922e-7`.

Those physics differences are **not accepted tolerances**. The available
oracle-only adjacent-time diagnostic is scalar absolute `4.9907348694323446`,
scalar relative `1.2966848729312654e-5`, velocity absolute
`0.0015257825718035956 m/s`, and ledger relative `1.1475158518490884e-8`.
On the same slice, the production differences are respectively `83.51`,
`83.51`, `27.73`, and `8.67` times the `1.25 Delta_dt` terms. The required adjacent-grid tier-6
package and independently enumerated `B_fp32` terms do not yet exist, so
section 5.1 cannot choose a band without post-hoc tuning. The validation
fixture therefore records all eight monitored deviations and deliberately
returns a calibration-blocked result at the end. r105 stops here rather than
blessing production output or widening a ceiling. The immutable checkpoint
remains SHA-256 `1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`.

### 7.23 Calibration protocol freeze (r106)

Before producing new calibration evidence, r106 replaces section 5.1's old
maximum-of-terms rule with the additive triangle bound. The input manifest is
sealed separately from all solver output. It contains the exact tier-5/6/7
shapes and spacings, centered common-support geometry, `dt`, `dt/2`, `dt/4`,
boundary/source/`S_div` bytes, metric registry and units, formal orders,
operation-topology and solver-source digests, strict-fp/compiler requirements,
and all eight golden beginning hashes. Result-like fields are forbidden.

The tier calibration states are fresh, case-bound certified tier-5/6/7 states
at the sealed physical time `0.32 s`, just after the adopted pilot ignition
window begins to produce both annular and plume-edge gradients. Their
ceil-rounded domains differ, so x/y are
aligned by domain center, z by the common floor, cells use exact overlap
volumes, MAC values use area weights plus normal interpolation, and comparisons
use only the exact mutual physical support. Whole-box inventories from the
three unequal domains are not refinement evidence. The original tier-10
checkpoint remains read-only and is hashed before and after every phase. A
pre-extracted and hashed divergence target is reused by both solvers; the
production request cannot consume a target emitted by the oracle run under
comparison.

The production fp64 mirror is generated from the strict CPU production
arithmetic bodies and is source-digest bound. It executes the same five cell
submaps, fifteen dual submaps, frozen one-to-eight viscous substeps, explicit
source order, and one fixed sixteen-cycle pressure-open projection. Stored
fp32 inputs are promoted exactly. The float instantiation must reproduce the
existing CPU comparators byte-for-byte before the double instantiation can be
evidence.

The operation trace/topology machinery described normatively in r89/r90 was
not present when r105 stopped; r106 treats that as an implementation gate, not
as prior evidence. Derivation runs in a process that cannot dispatch Metal and
atomically seals an input-and-bound manifest. Confirmation runs in a separate
process that may execute fp32 Metal but cannot alter or complete the manifest.

`B_fp32` is derived without reading the fp32-minus-fp64 result. For every
kernel, an independent topology walk and an execution trace must agree on
operation kinds, reduction depths, operand envelopes, denominator lower
bounds, and discontinuous-branch margins. With unit roundoff `u=2^-24`, each
stage supplies an outward local term `beta_k` and condition amplification
`kappa_k`; the composed recurrence is

`B_(k+1) = nextUp(kappa_k B_k + beta_k)`.

The generator rejects `n*u>=1`, a trace/topology disagreement, an unresolved
branch margin, nonfinite outward arithmetic, or a bound above its scientific
ceiling. The resulting parametric certificate is confirmed on identical
tier-6 fp32 Metal/fp64 mirror inputs, then instantiated independently for every
SHA-bound tier-10 short-horizon input. A tier-6 number is never copied onto a
tier-10 topology.

Generalized three-grid Richardson uses
`D56=||U5-U6||`, `D67=||U6-U7||`, solves
`D56/D67=(h5^p-h6^p)/(h6^p-h7^p)`, and estimates the tier-6 distance as
`E_h=D67/(1-(h7/h6)^p)`. Three equal-horizon temporal levels estimate the
baseline `dt` distance. The mutual-limit premise is itself gated:
signed/restricted production and oracle extrapolants must overlap within their
separately estimated unresolved remainders. The final tolerance is the
outward sum of the four scheme-distance terms and analytic `B_fp32`; no safety
factor or production-derived constant is permitted.

Finally, the eight-step short gate chains each solver from step 3479 and stores
`[step][channel/quantity]` evidence. The earlier r105 campaign is retained and
renamed a local-slice ensemble; its cumulative mixed-unit scalar maximum is
diagnostic only. If any chained quantity exceeds the derived model, stage
probes partition cell transport, dual transport, frozen force, sources, and
projection before either solver or the contract is changed. Long-horizon
pointwise comparison remains rejected; V-gates, tier-6 empirical rows, and
prefix integrals own that chaotic-flow regime.

### 7.24 Calibration state-family diagnostic (r107, superseded as a gate)

The exact-common-support beginning-state comparison remains useful evidence,
but it is not a calibration gate. r107 initially treated its non-asymptotic
ratios as a stop. Fresh review found that this was not the r106 contract:
section 5.1 defines `D56` and `D67` on each solver's evolved outputs, and a
contractive operator can map a non-asymptotic input family to an admissible
output family. r108 therefore withdraws the r107 stop without discarding its
diagnostic data.

The state-family manifest SHA-256 is
`338d7c66ee83c1c43e8f12d311389261af320335b70476203c42ff85dc66c3f5`.
It and the three exact tier checkpoints are preserved under
`rendered/fire_production_calibration/r107_state_family/`; obsolete v1/v2
partial manifests remain absent. The command
`./bin/tests/FireSequenceTest --fire-production-calibration-diagnose-input-family
rendered/fire_production_calibration/r107_state_family` prints the prior
ratios and exits zero, explicitly labeling them diagnostic-only.

### 7.25 Certified-oracle output spatial stop (r108)

r108 performs the missing evolved-output test before deriving `B_fp32` or
dispatching calibration Metal. The dedicated strict executable
`bin/tests/FireProductionCalibrationOracle` is linked without the four
production Metal objects or `Metal.framework`; its build rejects either the
framework or a production Metal implementation symbol. That no-Metal process
advances each exact tier state from `0.32 s` to `0.322 s` as four fixed
`0.0005 s` certified steps with
zero source packets and the fuel-bed overlay disabled. It atomically seals all
twelve resulting `S_div` arrays before the comparison process runs. The scoped
oracle spatial manifest SHA-256 is
`a2bb4c834af7f9c839c3e0114dcaa64cac43f915969fce2ab55e0395fe1b20e2`;
the tier-5/6/7 `S_div` payload hashes are respectively `b629dcde...`,
`399d7833...`, and `f98ea7f...`. The manifest binds the state family, exact
time schedule, boundaries, gravity, zero sources, worker count, fuel and
transport record ids, oracle source digests, metric, support, and formal order.
The read-only comparison reruns the oracle, byte-compares every `S_div` value
to the sealed payload, and only then evaluates the evolved conservative state.

The actual oracle-output spatial evidence is:

| conservative channel | `D56` | `D67` | ratio | classification |
|---|---:|---:|---:|---|
| `rho_total_Z` | `6.031592721568220e-6` | `5.292733054040505e-6` | `1.139598891533678` | no positive order |
| `CH4` | `5.645733995242471e-6` | `4.846863411353551e-6` | `1.164822177991978` | no positive order |
| `O2` | `1.025031344813252e-4` | `5.518918039891239e-5` | `1.857304887306230` | capped `p=2` |
| `N2` | `3.319487331943980e-4` | `1.749206346029661e-4` | `1.897710547116717` | capped `p=2` |
| `CO2` | `1.549001496383576e-6` | `1.633829716721150e-6` | `0.948080133768278` | fine pair grows |
| `H2O` | `1.268166903862522e-6` | `1.337615733832568e-6` | `0.948080133768269` | fine pair grows |
| `CO` | `5.909935429537276e-19` | `7.878527971317004e-19` | `0.750131934677811` | near-zero/unidentifiable |
| `C(gr)` | `2.227913137406351e-19` | `3.007735710307910e-19` | `0.740727694182370` | near-zero/unidentifiable |
| sensible enthalpy | `135.7301690853277` | `71.25923090371239` | `1.904738057989012` | capped `p=2` |

For the actual spacings, positive order in `0<p<=2` requires a ratio in
`[1.18274896,1.65846154]`. The evolved certified outputs therefore leave the
required oracle spatial terms undefined for `rho_total_Z`, `CH4`, `CO2`, and
`H2O`. One missing addend is sufficient to block the triangle tolerance, so
production refinement, `B_fp32`, Metal confirmation, velocity/ledger terms,
and eight-slice readmission do not run. The canonical command is
`./bin/tests/FireProductionCalibrationOracle --fire-production-calibration-check-oracle-spatial
rendered/fire_production_calibration/r108_oracle_spatial
a2bb4c834af7f9c839c3e0114dcaa64cac43f915969fce2ab55e0395fe1b20e2`;
exit `162` is the monitored calibration stop. The next admissible work is a
cause probe of the evolved tier family or an owner-approved continuous-input
calibration-family ruling. No solver, tolerance, or ceiling changed;
thermo/source maps remain downstream. The tier-10 checkpoint remains
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`.

### 7.26 Dyadic smooth filtered calibration instrument (r109 protocol freeze)

r108 remains valid evidence about the rejected adjacent-tier instrument, not
about the oracle. Before any r109 output is produced, the replacement study is
frozen as follows.

- The common periodic box is exactly `4 D* x 4 D* x 6 D*`. Tier `r` has
  dimensions `(4r,4r,6r)` and `h=D*/r`, so `{5,10}` and `{6,12}` are exact
  factor-two pairs with identical physical domains. The capstone methane
  `D*=0.24474492853928811 m` is derived from the unchanged case record.
- Every tier samples the same analytic thermochemical state: a smooth periodic
  convex ambient/injected mixture, smooth temperature, and a divergence-free
  Taylor--Green MAC velocity. Sources are exact positive zero and all six
  sides are periodic. This is an instrument state, not a replacement capstone
  initial condition.
- The comparison horizon is `H=t_ft/64=0.014810434142203257 s`, below the
  `0.1 t_ft` transition/puffing class and commensurate with the eight-slice
  contract. Exactly eight fixed steps use `dt=t_ft/512`.
- Field observables are mollified before comparison by the nonnegative,
  partition-of-unity tensor cubic cardinal B-spline. Its physical scale is
  `w=h5=0.04894898570785762 m`, support radius `2w`, and every weight is the
  exact polynomial cell integral. All tiers are sampled on the tier-5 lattice.
  Unfiltered values and conservative integrals remain diagnostics; exact donor
  envelopes and signed minima remain structural gates.
- For each metric and pair, `D=||U_h-U_(h/2)||` and the V2-qualified lower
  order is `p=1.8`. The fine-grid Richardson radius is
  `R=D/(2^p-1)`. The two independently extrapolated limit fields must differ
  by at most `R_5,10+R_6,12`, and each fine field must be closer than its
  coarse partner to the other pair's extrapolate. Failure of either condition
  is a monitored oracle-regime stop. If they pass, the tier-6 spatial term is
  `max(D_6,12/(1-2^-p),(h6/h5)^p D_5,10/(1-2^-p))`, rounded outward.

The no-Metal derivation process seals the analytic-state, schedule, filter,
metric, record, source, and decision-rule bytes before a comparison process
may inspect output. If any required channel fails, production refinement,
`B_fp32`, Metal confirmation, and eight-slice readmission do not run. If all
pass, the remaining r106 triangle campaign resumes. No solver, ceiling, or
tolerance changes as part of this instrument redesign.

### 7.27 All-channel analytic excitation (r110 protocol freeze)

The first r109 run was rejected as an instrument, before judging the oracle.
Its ambient/injected convex blend left CO2, H2O, CO, and `C(gr)` algebraically
unexcited. Their independent-limit checks failed only at `1.08e-21--7.48e-21`
absolute scale while all five excited channels passed. Treating those trace
roundoff values as a continuum-regime failure would contradict the purpose of
the redesign.

r110 changes only the sealed analytic composition. It uses smooth, strictly
positive mass fractions in every channel: CH4 `0.025+0.003 m0`, O2 `0.215`,
CO2 `0.012+0.001 m1`, H2O `0.009+0.001 m2`, CO `0.0015+0.0002 m0`, and
`C(gr)` `0.0005+0.0001 m1`; N2 is the exact remainder. Here
`m0=sin X sin Y sin Z`, `m1=cos X sin Y cos Z`, and
`m2=sin X cos Y cos Z`. The mixture tag is `0.04+0.01 m0` and temperature,
velocity, tiers, domain, horizon, filter, verified order, and decision rule are
unchanged. This all-channel excitation is frozen before r110 output. The r109
artifacts and numbers remain a diagnostic record and cannot be used for a
tolerance.

### 7.28 Affine-admissible all-channel state (r111 protocol freeze)

r110's direct species authoring was rejected before its first advance by the
certified elemental-affine rows; it produced no output evidence. r111 repairs
the instrument formulation rather than loosening that gate. Each cell starts
on the exact ambient/injected mixture line at `Z=0.04+0.01 m0`, applies `0.2`
of the limiting primary methane reaction direction from the immutable record,
then partitions `0.02` of CO2 into the reverse CO-oxidation direction and
`0.01` of the remainder into the reverse soot-oxidation direction. Those two
record-molecular-weight constructions preserve mass and C/H/O/N rows while
making CO and `C(gr)` strictly positive. Sensible energy is recomputed at the
same smooth temperature. All r109 geometry, horizon, filter, order, and
decision bytes remain unchanged. This v3 analytic state is sealed before any
r111 advance.

### 7.29 Expansion-compatible open instrument (r112 protocol freeze)

r111's periodic first advance was structurally rejected because its smooth
thermochemical evolution has nonzero mean `S_div`; a periodic pressure solve
correctly requires a compatible zero mean. No r111 output was produced. r112
uses pressure-open on all six sides and the canonical open-MAC endpoint
storage. The analytic field, dyadic grids, horizon, and decision rule do not
change. To keep the fixed mollifier independent of boundary extension, the
tier-5 observation lattice is the exact interior index box
`[2,nx-3] x [2,ny-3] x [2,nz-3]`, dimensions `(16,16,26)`; its cubic
B-spline support lies wholly inside the physical domain, so only physical cell
integrals contribute. This boundary/window choice is sealed before any r112
advance.

### 7.30 Complete dyadic oracle acceptance and fp32 state-admissibility stop (r113/r114)

The durable r112 directory is
`rendered/fire_production_calibration/r112_dyadic_smooth_open/`. Its protocol
manifest SHA-256 is
`42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed`;
its eight-step target manifest SHA-256 is
`d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b`.
The dedicated no-Metal comparison byte-verifies every sealed `S_div` payload
and exact-binds all 27 scalar evidence values (`D_5,10`, `D_6,12`, and the
extrapolated-limit difference). A fresh boundary review found that the first
r113 gate had not replay-checked the analytic-state digests and had not judged
the velocity or inventory channels. Before inspecting those missing values,
r114 sealed `dyadic_metrics.v1` with SHA-256
`86369b69d37fabc9aaa1dfe24d054b543a35f66f48fa268acc99bb203659ba09`.
It fixes velocity as the arithmetic MAC-to-cell average followed by the same
physical tensor cubic B-spline, its norm as volume-RMS vector L2, and each
inventory ledger as the Kahan-reduced final component inventory per physical
volume. The checker now reconstructs and SHA-checks all four analytic states.

Every conservative, velocity, and inventory channel passes both the
independent-limit-ball and cross-pair-approach rules. The filtered velocity
distances are `0.0039631780862326585` and `0.0032641652172793294`, with
extrapolated-limit difference `0.00040722205192891077`. Inventory distances
range from `5.7907549732782678e-9 / 4.2319238834949294e-9` for `C(gr)` to
`3.0335881874780171 / 2.1990585236198967` for sensible enthalpy. The direct
tier-6 filtered-scalar distance terms range from `4.0730194870657798e-8` for
`C(gr)` through `27.496545640537025` for sensible enthalpy. All 57
load-bearing scalar, velocity, and inventory values are exact-pinned; drift cannot pass merely by preserving
the acceptance class. The redesigned dyadic, short-horizon, mollified oracle
instrument is therefore accepted for its complete declared observable class.

Production refinement is not yet admissible. The first tier-5 resident step
uses the sealed target, exact positive-zero sources, sixteen fixed projection
cycles, the shape-derived exact smoother count, and zero interstage full-grid
transfers. Its raw conservative payload is finite and gas-positive, with
SHA-256 `03faf5aad21ef47b5127213dde0e744e92d0d89f4a2a5345b7bd373979564e50`.
Nevertheless the payload has maximum certified-row residual
`5.2451771873310863e-8` (row 2, cell 4915). That is `0.8799947063` binary32
unit roundoff but `57,671.3331` times the record's current fp64 admissibility
envelope `9.0949470177292824e-13`. The next step cannot invert temperature or
derive molecular viscosity without either an unruled state projection or a
new analytically derived fp32 feasibility envelope. The canonical command

`./bin/tests/FireSequenceTest --fire-production-calibration-check-dyadic-production rendered/fire_production_calibration/r112_dyadic_smooth_open 42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b`

returns exact monitored stop `190`. This is not a failure of the newly
accepted oracle regime and is not a license to infer `B_fp32` from the
measurement. The next design ruling must state how strict-binary32 resident
transport outputs become accepted thermochemical beginnings, derive its bound
before confirmation, and preserve conservative ledgers. Until then the
production Richardson, `B_fp32`, Metal confirmation, eight-slice, and source-
map campaigns remain blocked. The tier-10 checkpoint remains byte-untouched.

The host arithmetic that creates the packed resident gas field is now compiled
with `-fno-fast-math -ffp-contract=off` on make and on both Xcode source phases.
This is load-bearing: the former inherited fast-math path changed the ownership
sum by one binary32 ULP. A source-local test binds both Xcode entries, including
the shipping Opto configuration.

## 8. Rejected directions and future work

- Per-step porting of the fp64 certificate stack: cannot meet the target and
  duplicates the oracle instead of using it.
- CPU-first production execution: the measured reference path is about 23.1
  s/step at 16.5% parallel efficiency and cannot approach one hour.
- Multiple projections per step: consumes the dominant GPU budget.
- MacCormack/BFECC plus clipping: nonconservative and obscures validation.
- Runtime tolerance widening, dt-halving acceptance, or hidden state repair:
  recreates the stalls this architecture is meant to remove.
- Altering the reference multigrid's measured 41,600 smoother sweeps/step:
  remains a separate class-B oracle campaign and is not part of production.
