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
difference is never used to estimate either term.  Under the r137 amendment,
the composed full-step `B_fp32` is the pre-registered subdominance allowance
`2^-3 E_prod`, confirmed independently on every sealed slice by the resident
fp32 Metal path against the strict fp64 same-scheme mirror.  Streaming-stage
rounding envelopes remain analytic diagnostics, but their nonlinear
composition is not a contract term.  The measured fp32/fp64 difference never
selects or widens `B_fp32`. A short-horizon metric passes only when its observed
difference is at most `min(T_m,C_m)`.

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
transfers. Before the thermochemical stop is considered, the gate requires the
projection validation contract to pass and byte-pins its published evidence:
pre-residual `0.18607060611248016`, post-residual
`6.8208464654162526e-7`, open complementarity `0.011058024130761623`, and
removed mean `+0`. Its raw conservative payload is finite and gas-positive, with
SHA-256 `03faf5aad21ef47b5127213dde0e744e92d0d89f4a2a5345b7bd373979564e50`.
Nevertheless the payload has maximum certified-row residual
`5.2451771873310863e-8` (row 2, cell 4915). That is `0.8799947063` binary32
ULP, equivalently `0.4399973532 epsilon32` under r60's
`numeric_limits<float>::epsilon()` convention, but `57,671.3331` times the
record's current fp64 admissibility
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

### 7.31 Precision-class accepted-state feasibility and the next monitored stop (r115/r116)

r115 extends r60's one accepted-state predicate by producer precision. It does
not add a second predicate and does not change the accumulation-owned mass or
energy scale. A state's producer metadata selects exactly one unit system:

`bound_r = kappa_precision * epsilon_precision * scale_r`.

The immutable v1 record continues to own `kappa64=4096`, so every binary64
oracle state sees exactly the former r60 envelope. The strict binary32 resident
result publishes `Binary32` producer metadata; default/failure results publish
`Unknown` and are never admissible. The binary32 extension is derived from the
union of already certified resident producers. Remap contributes `256 eps32`
because `3e-5/eps32 < 252`; the r96 composed force result contributes `64
eps32` and subsumes r93's per-kernel `8` ULP result; the one projection
contributes another `256 eps32`. The pre-thermo source operand is exact positive
zero and contributes no term. Thus the union is `576`, and r60's
next-power-of-two rule gives `kappa32=1024`. Nonzero thermo/source maps must add
their own derived producer term before they may publish a binary32 state. Until
that ruling lands, the cell, 1-D FCT, and owning periodic/open 3-D application
seams accept only bit-exact positive-zero source packets in the Binary32 class;
nonzero and negative-zero inputs fail closed before publication.

The original tier-5 observation is confirmation only: its maximum scaled
affine excursion is `5.2451771873310863e-8`, or `0.8799947063` ULP and
`0.4399973532 epsilon32` under r60's convention, at cell 4915 row 2. The
derived envelope is `0.0001220703125` at unit scale, a
`2327.2867272213789` margin. A mutation above `kappa32*eps32*scale_r` remains a
structural rejection. Temperature inversion, EOS evaluation, positive-part
property availability, and molecular-viscosity reconstruction all enter the
same metadata-selected predicate; no stored conservative byte is repaired.
Temperature inversion certifies the signed mixture-energy bracket, including
the fp32-envelope endpoint; EOS, rate and finite-increment divergence,
source-expansion, and radiation evaluate the positive gas subset after the same
gate; positive-part availability maps an envelope-negative trace constituent to
zero only for property evaluation; and molecular transport uses those
positive-part gas fractions and remains finite and positive. The 1-D and 3-D
FCT paths, divergence identity, and open-boundary stage propagate the same
producer precision through their raw-vector configuration or parameter. Run
checkpoint v9 persists one homogeneous producer class across every accepted
cell and rejects mixed-class publication; resume derives the composed owner
configuration from that class. Historical v5--v8 checkpoints decode as
`Binary64`, so the immutable v8 golden checkpoint and its bytes remain unchanged.

The ruling explicitly rejects three alternatives: projecting or clamping each
resident state would mutate conservation ledgers and hide producer defects;
promoting the resident state to binary64 would defeat the resident architecture;
and widening `kappa64` would confuse two unit systems and weaken the certified
oracle. This feasibility envelope is categorically distinct from `B_fp32`: it
decides whether a state has defined downstream physics, while `B_fp32` is an
accuracy term in the calibrated comparison contract.

With r115, the former exit `190` chains successfully and 31 more resident
steps pass their projection validation contracts. r116 then stops at a new,
independent gate: tier 12, step 7, cell 2256 reconstructs a finite temperature
`348.53712185868289 K` and a positive molecular viscosity, but its EOS residual
is `0.0011434014099940271`, above the pre-existing `0.001` accepted-state EOS
limit. The payload digest is
`e5a8cdfd54772cc58c8d58e3a0c32d650a71f9f428cd27c1be3e52e6a60c5b70`;
its maximum affine excursion `2.1925594524305645e-7` remains far inside the
binary32 feasibility envelope, so the EOS miss cannot be charged to r115 or
absorbed into it. Exact monitored exit `191` records this stop. The four-tier
production campaign is incomplete and admits no Richardson distance; `B_fp32`
and all later calibration stages remain unrun until the EOS drift is instrumented
under the budget-probe discipline. Neither solver nor the `0.001` EOS contract
was changed.

### 7.32 EOS-drift diagnostic contract (r117, pre-evidence)

The exit-191 diagnosis is a read-only resident campaign. It records the signed
EOS-volume deviation `d=V(Q)-1` at tier-12 cell 2256 and the independently
searched field maximum for all eight transitions (campaign indices 0--7; index
7 is exit 191). Linear monotone growth means missing or
ineffective absolute restoration; a fast plateau means per-step production
exceeds the single projection's drain capacity. The target audit recognizes
r69 only when the production target itself contains `(V(Q^n)-1)/dt`, and r70
only when the target closes the production transported candidate's advective
volume anomaly. Reusing an oracle target does not establish either property.

Drain uses the frozen r117 counterfactual: campaign step 6 (displayed transition
7) adds the per-cell fp32 term
`float(d_n/float(dt))` to the existing target, requires byte-identical transported
conservative output, carries only the changed projected MAC state into campaign
step 7 (the failing displayed transition 8), and measures
`r=(d_8-d_8^restored)/d_6`. With `G=d_8-d_7`, the predicted
steady deviation is `G/r`. The probe must fail for nonfinite or zero diagnostic
denominators, any residency miss, or any changed step-6 conservative byte. Its
primary implementation reading additionally requires both projections to
validate; a failed validation leaves only the explicitly provisional capacity
reading below. No measured value may alter the EOS gate or select a fix
retroactively.

If the full-gain counterfactual projection misses validation, the primary probe
is invalid and implementation is barred. A secondary capacity-only readout may
carry its provisional MAC field through one step-7 transport and evaluate the
same `G/r` formula, provided it labels both validation bits and preserves every
residency and conservative-byte check. Such a readout measures the fixed
schedule's drain response; it is not accepted production evidence.

A distinct eight-step shadow applies the same absolute term from the analytic
tier-12 beginning rather than injecting it only at step 6. It records every
projection-validation bit/residual, residency count, probe deviation, and field
maximum. It can qualify r69 for implementation only if all eight projections
validate, downstream state reconstruction stays total, and the observed
steady deviation agrees with `G/r` below the unchanged EOS ceiling. Continuing
a provisional shadow after a validation miss is diagnostic only.

The binding replay is
`RISE_FIRE_EOS_DRIFT_PROBE=1 ./bin/tests/FireSequenceTest
--fire-production-calibration-check-dyadic-production
rendered/fire_production_calibration/r112_dyadic_smooth_open
42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed
d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b`.
It returns `215` only after every r117 pin matches. A malformed nonempty
activation returns `216`; without activation the certification campaign keeps
its original exit `191`.

The exact r117 result classifies the original path as secular accumulation.
At cell 2256 the eight outputs are `1.62435e-4, 2.90527e-4, 4.22101e-4,
5.57589e-4, 6.97411e-4, 8.41544e-4, 9.90210e-4, 1.14340e-3`; it becomes the
field maximum on transition 3 and remains so through failure. The production
request merely copies the pre-extracted oracle target: it has neither a
production-state r69 absolute reference nor a production-candidate r70
advective-anomaly closure.

The earlier transition-7 capacity values are retired because they did not
measure the failing configuration. At transition 8 the corrected capacity-only
readout is `G=1.5319163029481331e-4`, `r=1.0030530061213276`, and
`G/r=1.5272535883939468e-4`, or `0.15272535883939467` of the EOS ceiling.
Its full-gain projection and the carried-state projection are both invalid:
their exact post residuals are respectively `0x1.ac2p-12` and
`0x1.acbf72p-12`. The eight-step r69 shadow agrees in state space: its probe
stays `1.04e-4--1.93e-4` and its absolute field maximum
`1.56e-4--3.37e-4`. But the fixed single projection validates only transitions
2, 5, and 8; the other five post residuals plateau at
`4.045e-4--4.100e-4`.

Thus restoration would put the EOS plateau below the ceiling, but the existing
single-projection schedule cannot admit the restoration targets. This is an
architecture-capacity finding, not authorization to land r69. Restoration
gain, a manifold-residual-only second projection, or a new derived production
ceiling have different contract consequences and need an owner ruling. No
solver or gate changes in r117. Exact evidence and its plot are stored under
`rendered/fire_production_calibration/r117_eos_drift/` with SHA-256
`ea7943ef...848c` and `c8a2677d...a1a0`. Production Richardson and subsequent
calibration stages remain stopped.

### 7.33 Resident manifold-restoration projection (r118, pre-evidence)

r118 resolves the r117 capacity finding with a second resident projection.
The first projection remains the physical projection against the sealed
`S_div` field. Its Private momentum result is not staged: it is borrowed by a
second invocation of the same Metal projection implementation. The second
invocation is a correction solve whose only authored divergence increment is

`R_n = float((V(Q^n)-1)/float(dt))`.

It therefore preserves the physical divergence already achieved by the first
pass; it is not an absolute projection to `R_n` and it never receives the
physical target. Open-boundary pressure correction is homogeneous in this
pass. The resident API binds the expected Private restoration-target buffer by
identity. Passing the physical target, a Shared target, a short buffer, or an
aliased buffer fails before the restoration command is created. Only the final
restoration result is staged at the step boundary. Scalar projection
diagnostics may be read between passes; no full-grid device/host transfer is
permitted.

The two passes own different validation contracts. The physical pass retains
the existing `0.005 U/L` criterion. The restoration pass derives its scale
from its own authored divergence increment and requires
`max |div(u_after)-div(u_before)-R_n| <= 0.005 max |R_n|`. A zero restoration
field therefore requires an exact-zero incremental residual. No physical
velocity, physical target, or physical tolerance is borrowed by the second
criterion. The first restored successor exposes a new physical-input class:
the former 16-cycle physical schedule leaves about `4.09e-4 s^-1` and misses
its unchanged band, while one additional V-cycle is sufficient. The composed
resident owner consequently fixes 17 pressure-open cycles for its first pass
and 16 for the restoration pass; standalone and removed-restoration physical
projection schedules remain unchanged. This is extra solver work, not a
tolerance change or a gain.

Restoration is deadbeat (`gamma=1`); no gain constant exists. A gain is
rejected because a value inferred in one density/boundary/target window has no
derived transfer across the production gamma-window regimes and would turn a
feasibility correction into a fitted knob. A derived wider EOS ceiling is
rejected for the same regime-dependence reason: the measured `G/r` diagnoses
this producer/solver pair, while the `1.0e-3` EOS ceiling governs every
consumer and producer. Moving that ceiling would mask producer defects outside
the measured window. Production intentionally omits r70's Picard
advective-anomaly closure. Its omission is explicit: the measured per-step
generation is `G`, while deadbeat absolute restoration predicts
`G/r=1.5272535883939468e-4`, 15.2725% of the unchanged ceiling and a 6.55-fold
margin.

Before r118 evidence is inspected, the long resident protocol is fixed as
follows. It starts from the immutable analytic tier-12 calibration beginning,
uses exact positive-zero sources, and advances 104 steps: thirteen complete
phase-balanced repetitions of the already sealed eight-slice physical-target
schedule. This is a controlled restoration-capacity trajectory, not new
long-horizon oracle evidence. Every step records the cell-2256 signed EOS
deviation, the independently searched signed field maximum and cell, both
projection pre/post residuals and validation bits, invocation/transfer counts,
the accepted-state envelope excursion, and the conservative payload digest.
The calibrating plateau statistic is the maximum absolute cell-2256 deviation
over the final 32 steps; the final-32 field maximum is recorded separately and
must remain below `1.0e-3`. Both projections must validate on all 104 calls.
After eight warm-up calls, the remaining 96 completed calls define device and
wall p95; the maximum certified and observed allocation totals are also pinned.

Three RED classes are frozen. `removed` retains the former one-projection
schedule and must reproduce all eight exact r117 secular-blowthrough values,
including exit 191. `full-target` substitutes the physical target at the
second-pass ownership seam and must fail before the second projection command.
The resident transfer/allocation REDs are re-run for both passes, and the fp32
producer union is re-derived as remap `256` + force `64` + two projection
terms `2*256 = 512`, total `832 epsilon32`; the r60 power-of-two rule therefore
keeps `kappa32=1024`. This feasibility envelope remains distinct from the
future accuracy term `B_fp32`.

The sealed r118 run admits the architecture. All 104 physical and all 104
restoration projections validate, with zero full-grid interstage transfers.
The r127 rerun's final-32 cell-2256 maximum absolute deviation is
`1.5439012582030287e-4`, 1.01090 times the independently predicted `G/r` and
15.4390% of the unchanged EOS ceiling (6.4771-fold margin). The independently
searched final-32 field maximum is `6.5237316812827295e-4`, also below the
ceiling. The final signed probe/field observations are respectively
`1.1211235847818912e-4` and `-4.0622240705245893e-4`. The exact 104-step
diagnostic/payload trace hashes to
`719ee45e254a65dc7b6a37ece81720d27213cd69d761312348d102a78f5f68cb`;
the final state hashes to
`d9a1a0ea021f38792ff9fa3c1446c239ab66d3981f4154e519e568d0030c9dc2`.
The maximum certified/observed allocation upper bounds are 248,479,780 and
229,518,420 bytes. Over 96 post-warmup calls, combined force/transport/two-P2
device p95 is 28.2015 ms and completed-call wall p95 is 70.4273 ms, 35.21% of
the 200 ms production wall allocation.

The binding replay is
`RISE_FIRE_EOS_RESTORATION_PROBE=1 ./bin/tests/FireSequenceTest
--fire-production-calibration-check-dyadic-production
rendered/fire_production_calibration/r112_dyadic_smooth_open
42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed
d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b`.
It returns `228` only after the exact deterministic evidence and the 200 ms
wall gate match. Malformed activation returns `226`; simultaneous r117/r118
activation returns `227`. Durable evidence is
`rendered/fire_production_calibration/r118_restoration/restoration_evidence.v1`
(r127 rerun SHA-256 `2752dc2075002911f8bec9bf909617fe8b4f641b9de3cbf20399475e0afaf451`).

### 7.34 Production dyadic spatial term (r119)

With r118 admitted, the exact eight-step resident campaign completes on all
four dyadic tiers. The production observable uses the already sealed smooth
state, horizon, filter, order `p=1.8`, and `{5,10}` / `{6,12}` decision rule;
no metric or decision rule changed after output inspection. Every physical and
restoration projection validates, and every interstage full-grid transfer
count is zero. All nine filtered scalar channels, filtered MAC velocity, and
all nine inventory channels have overlapping independent Richardson limit
balls and approach the opposite pair's limit from coarse to fine.

The exact `D5_10`, `D6_12`, and independent-limit differences are bound in
`ExpectedProduction*Evidence` and in the durable artifact
`rendered/fire_production_calibration/r119_production_spatial/production_spatial_evidence.v1`
(r127 rerun SHA-256 `0c481de835c8dbf51044b7246000668fe39e4cde9832f36a95797f3b717eb8de`).
This admits the production **spatial** scheme-distance term only. It is not a
temporal term, not `B_fp32`, and not an eight-slice production-versus-oracle
acceptance band. The fp32 feasibility envelope remains a consumer-totality
condition and contributes no accuracy allowance.

### 7.35 Two-projection roundoff certificate protocol (r120, pre-evidence)

The r106 `B_fp32` protocol is instantiated for the r118 graph before any
fp32-versus-fp64 value is inspected. Its ordered stages are frozen force with
the selected one-to-eight substeps, five cell palindrome maps, fifteen dual
maps, explicit source addition, the 17-cycle physical pressure-open solve, and
the 16-cycle correction-only restoration solve. The restoration target is an
input operand derived from the beginning state; its feasibility factor is not
an accuracy allowance. The physical and restoration projections have distinct
topology records and distinct local roundoff terms.

The derivation process cannot link Metal. A test-only arithmetic trace records
operation kind, dependency depth, absolute operands, denominator/domain lower
bounds, and branch margin for every executed kernel. A separately authored
topology walker must reproduce its counts and depths from shape, boundary roles,
substep count, multigrid hierarchy, and fixed schedules without reading trace
counts. Any mismatch, unresolved limiter/open-inflow/floor branch, `n*u>=1`,
or nonfinite outward recurrence fails before a Metal result exists. Local
`(kappa_k,beta_k)` terms compose only through
`B_(k+1)=nextUp(kappa_k*B_k+beta_k)`.

The emitted radii are reduced into the same nine filtered scalar L1 channels,
filtered MAC velocity L2, and nine inventory channels used by r112/r119. The
sealed tier-6 confirmation then promotes the identical fp32 request bytes into
the generated binary64 mirror, requires identical substep and branch topology,
and checks fp32 Metal minus fp64 same-scheme against the already sealed analytic
radii. Measurements cannot change a radius. Tier-10 golden slices receive
separate state/topology instantiations; no tier-6 number is copied to them.

### 7.36 Burning-state restoration prediction (r121, pre-source evidence)

The first nonzero-source capacity check is fixed before thermo/source-map
implementation. It begins from the immutable tier-10 checkpoint
`rendered/fire_methane_capstone/tier10.run.checkpoint`, SHA-256
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`,
at accepted step 3480. The production source maps will be evaluated from that
beginning state, then frozen and repeated only for this controlled capacity
diagnostic; the result is not a long-horizon physical trajectory.

Let `G_0` be the already measured source-free per-step EOS generation, let
`G_src` be the signed increment obtained by subtracting an otherwise identical
exact-`+0` source call from the new-source call before restoration, and let
`r_burn` be the beginning-deviation drain fraction measured by the same
one-step self-donor construction at this checkpoint. With deadbeat restoration
the pre-registered plateau prediction is

`d_plateau = (G_0 + G_src) / r_burn`.

The sign and per-cell values are retained; an absolute maximum is taken only
after the signed prediction is formed. The source-free calibrating values
remain `G_0=1.5319163029481331e-4` and `r_0=1.0030530061213276`; they are not
assumed for `G_src` or `r_burn`. The future 104-step frozen-burning campaign
must validate both projections on every call, preserve zero interstage
full-grid transfers, plateau below the unchanged `1.0e-3` EOS ceiling, and
agree with the independently formed `(G_0+G_src)/r_burn` prediction. A source
term that makes the predicted plateau exceed the ceiling is an architecture
finding, not permission to change restoration gain or the ceiling.

### 7.37 Independent roundoff derivation refusal (r122)

The first no-Metal execution of the frozen r120 arithmetic trace stops before
`B_fp32` exists. The exact 24-stage diagnostic trace digest is
`8f3e709af17fdf9b22f271b37791d8287243f1054c5df530437cf963e8fbf93e`;
it binds all four generated source/header pairs plus the generator, every per-operation
count, operand maximum and radius maximum, depth, branch/domain evidence, and
published-output enclosure.
The unresolved-branch bitmap is `0xdffffe`: all five cell maps, all fifteen
dual maps, and both projection solves cross at least one executed comparison
surface. The invalid-denominator bitmap is `0x1ffffe`: every transport map has
an interval denominator whose lower enclosure crosses zero. Force and the
exact-`+0` source stage are branch-resolved and domain-valid.

This is a structural pre-measurement refusal, not a measured rounding miss and
not a numerical allowance. A separately authored fail-fast walker repacks the
public SoA bytes and reconstructs the first x-half-step PPM edge DAG without
calling production or traced arithmetic. At line 1, cell 6, component 3 it
derives the executed `quadratic!=0` comparison as
`-7.7486038219110043e-7 +/- 1.2337798327030971e-6` against exact zero; the
intervals overlap although the rounded comparison is true. Halving the derived
radius makes the intervals separate, which is the operation-undercount RED.
The later 24-stage trace bitmaps are retained as diagnostics, not as a substitute
for the independent refusal. The physical and restoration projection
denominators remain strictly positive, but each projection also reports an
unresolved branch.

The walker is fail-fast in execution order: once an independently reconstructed
executed branch is unresolved, the protocol forbids a composed radius, so no
claim about later-stage topology reconciliation is needed or made. A successful
future derivation must still walk and reconcile the complete graph.

Under the frozen r120 rule, no Metal/fp64 measurement may run and no analytic
radius may be emitted while branch topology is unresolved. Production temporal
refinement, eight-slice readmission, and thermo/source maps therefore remain
stopped. Continuing requires an explicit, pre-evidence ruling that supplies
either a branch-stable arithmetic formulation or an independently proved
branch-equivalence certificate; measurement cannot select between branches or
inflate a bound. The durable refusal artifact is
`rendered/fire_production_calibration/r122_roundoff_derivation/roundoff_derivation_stop.v1`,
SHA-256 `939e95f8ff916fff6c168d2b6bd30186b50a9b6e7963255b10641481c1b5d5ac`.

### 7.38 Branch obligations and equivalence envelopes (r123, ruling)

r123 amends the frozen r120 protocol only at its branch-obstruction rule. The
lineage is r59: a discontinuous derived selector is not made certifiable by
requiring the selector itself to converge. Instead, certification reasons
directly about every admissible output selected at the switching surface. The
same principle now governs certified arithmetic.

Every executed comparison whose independently propagated predicate interval
crosses zero emits a branch obligation with stage, executed comparison ordinal,
site class, rounded decision, predicate center/radius, ambiguity width, and the
two successor envelopes. An obligation is discharged only by one of:

1. an equivalence certificate proving the two branches coincide on the exact
   switching surface and supplying an independently derived Lipschitz/divergence
   term over the entire ambiguity interval; the composed recurrence consumes
   the outward hull of both successor paths plus that term; or
2. a recorded branch-stable reformulation, mandated when surface continuity or
   a finite transferable divergence bound cannot be proved.

An equivalence certificate never picks the rounded branch, suppresses the other
path, or tunes a tolerance from fp32/fp64 measurements. It proves an envelope
over both paths before Metal runs. An undischarged obligation preserves exit
`237`; source, topology, certificate-kind, and outward-bound bytes are identity
bound. The complete resident-step derivation must report the total obligation
census and discharge every entry before `B_fp32` exists. A successful walk then
resumes the frozen sequence: same-scheme Metal confirmation, temporal
refinement, eight-slice readmission, and finally thermo/source maps with their
separate fp32 producer term.

The first required certificate is the PPM `quadratic!=0` site found by r122.
At exact quadratic zero the reconstruction polynomial is linear, so its extrema
are its endpoints; the nonzero path is equivalent only if any stationary point
admitted over the ambiguity interval contributes an extrema enlargement bounded
by an independently derived Lipschitz term. If that statement fails for any
reachable interval, the limiter site must be reformulated branch-free and all
affected r118/r119-class evidence rerun. No empirical branch frequency or
measured fp32 difference may discharge it.

### 7.39 Branch-obligation evidence and shared-limiter stop (r123)

The amended no-Metal walk discharges the first PPM obligation. With endpoint
deviations `l,r` and `q=3(l+r)`, the quadratic reconstruction differs from its
endpoint chord by exactly `-q s(1-s)`. Both paths therefore coincide at `q=0`,
and `max_[0,1] |q s(1-s)|=|q|/4`. For the r122 witness the outward ambiguity
width is `2.008640214894198e-6`, giving the independently derived divergence
term `5.021600537235496e-7` for the exact polynomial. Separately rounded `q`,
linear coefficient, stationary point, and Horner arithmetic contribute the
independent outward `gamma_4`/underflow residual
`2.6783670818887366e-6`; the certified total is
`3.1805271356122864e-6`. No observed successor value enters this bound. The
trace consumes the total envelope rather than selecting its rounded nonzero
branch. All executed PPM quadratic and stationary-point obligations, and all
continuous `min`/`max` selections, are discharged by the same
identity-plus-hull discipline.

The complete 24-stage census contains 4,161,080 dynamic obligation instances:
468,869 discharged and 3,692,211 still pending. This count is diagnostic
evidence, not a bound. Propagating the finite PPM envelope also exposes
conditional division/domain intervals in all twenty transport stages
(`invalid_bitmap=0x1ffffe`); they remain pending proof obligations and are not
silently treated as arithmetic bounds. The first site proved to require the
mandatory reformulation fallback occurs in dual-remap stage
7 at executed-comparison ordinal 5,957. Resolved selectors count in the
ordinal even though only interval-crossing selectors emit obligations. The
negative-deviation predicate is
`-4.1921933491284591e-6 +/- 5.7555189193234835e-6`; its zero-headroom proof has
lower numerator `-4.9406564584124654e-324` but requires
`10.986253083780237` to leave the current shared limiter unchanged. The two
executed successor values are alpha `1` and alpha `0`.

This is not the PPM zero-branch problem in another spelling. A single shared
tuple alpha that is (i) maximally inactive when a component imposes no
constraint and (ii) strictly monotone when that component has zero headroom is
discontinuous: for positive reconstruction direction `d`, zero headroom
requires `alpha=0`, while at `d=0` that component is inactive and another tuple
component may retain `alpha=1`. The one-sided limit and switching-surface value
do not coincide. A local flat-stencil rewrite cannot repair the observed
non-plateau sites and was therefore not admitted. A branch-stable replacement
must explicitly change one of the coupled limiter semantics--per-component
limiting (losing the common affine-row alpha), a deliberately non-maximal common
limiter (adding dissipation), or loss of strict component monotonicity. That is
an operator-contract decision, not an arithmetic tolerance choice.

The exact no-Metal replay remains exit `237` after proving the PPM certificate
and binding the first non-equivalent limiter paths. `B_fp32`, Metal
confirmation, temporal refinement, eight-slice readmission, and thermo/source
maps did not run. The durable census is
`rendered/fire_production_calibration/r123_branch_obligations/branch_obligation_stop.v1`,
SHA-256 `2743b6347b2c456530949d2d01df182efa5b1c637f5107ebfd6f794bd4cb3971`.
The untouched golden checkpoint remains outside this diagnostic.

### 7.40 Continuous shared-alpha transition (r124, ruling)

r124 applies the r59 admissible-interval theorem to the first genuinely
non-equivalent r123 site.  The theorem fixes the invariant triage.  A single
tuple alpha remains shared by every component because that is the conservation
coupling in section 3.7, and every component remains inside its donor envelope
because monotonicity is physical admissibility.  Maximal inactivity is only an
optimality property: any pointwise smaller alpha remains in the already-proved
admissible interval.  It therefore yields inside the certified ambiguity
neighbourhood.  Relaxing shared alpha, relaxing monotonicity, or carving the
old binary branch out of certification are rejected respectively as ledger
breakage, physical inadmissibility, and restoration of an uncertifiable
topology.

The width is derived from the frozen r123 predicate enclosures, before any
reformulated production result is inspected.  For each positive or negative
limiter predicate let

```
s = max(FLT_MIN, abs(center), abs(envelope), abs(signed deviation))
u32 = 2^-24
```

over the exact rounded operands at that site.  The largest outward ambiguity
widths are `9292.2824737527444 u32*s` for the positive site class and
`8645.4622206683161 u32*s` for the negative class.  The next power of two is
`2^14`; the pre-registered transition half-width is consequently

```
w = 2^14 u32 s = 2^-10 s.
```

No Metal/fp64 difference selects this coefficient.  The independent walker
must verify every instantiated limiter obligation is contained by its local
`[-w,+w]` before the reformulation is certified.

Write `d` for signed envelope consumption (positive means the reconstruction
consumes headroom) and `Q>=0` for available headroom.  The component cap is the
continuous function

```
d <= -w :  c = 1
-w < d < w : c = min(1, (Q + max(-d,0))/w)
d >=  w :  c = min(1, Q/d).
```

The shared tuple alpha remains the minimum of its previous value and every
component cap.  At `d=-w` both adjacent definitions equal one; at `d=+w` both
equal `min(1,Q/w)`; and both one-sided limits at zero equal `min(1,Q/w)`.
For every `d>0`, `c*d<=Q`, so strict monotonicity is unchanged.  For
`d<=-w` the component is exactly inactive, and for `d>=w` the arithmetic and
result are the legacy limiter.  Thus only the smallest independently certified
neighbourhood sacrifices maximal inactivity, while common-alpha conservation
and the r59 admissible interval remain untouched.

Every comparison in this piecewise evaluation is itself a branch obligation.
The `-w`, zero, and `+w` joins close by the equalities above and an outward
envelope over both adjacent paths; no rounded branch is selected as proof.
The affected CPU and Metal evidence must prove byte identity outside the local
width and rerun the r118/r119-class gates.  The remaining r123 obligations are
then discharged by site class: one independent equivalence certificate or one
recorded continuous reformulation per source site, instantiated over every
cell/component.  An incomplete class census retains exit `237` and blocks
`B_fp32`.

### 7.41 Continuous-limiter evidence and complete branch census (r124)

The production CPU and Metal implementations use the algebraically compact
form of the pre-registered transition,

```
min(alpha, min(1, (Q + max(-d,0)) / max(d,w))) .
```

The independent walker proves the three joins, positive-consumption
monotonicity, containment of both frozen predicate classes by `w`, and exact
legacy behavior outside the width. A half-width mutant fails. Exact production
fixtures bind the ramp at `-w,-w/2,0,w/2,w`; CPU and Metal use the same
operation association. The 104-step r118 and four-tier/eight-step r119
campaigns were rerun. Their classifications do not move: the restoration probe
plateaus at `1.5439012582030287e-4`, the field maximum at
`6.5237316812827295e-4`, and all r119 scalar, velocity, and inventory limit
balls remain accepted. Only the expected strict-fp32 evidence bytes were
re-pinned.

Fresh boundary review rejected the first attempted bulk discharge because one
generic `4*M*ambiguity` expression did not independently model six distinct
branch topologies. The trace therefore does not use it. The fail-closed census
contains `4,340,821` dynamic obligations: the independently modeled PPM and
continuous-selection families discharge `652,411`; `3,688,410` remain pending.
The pending classes are floor partition `373,245`, flat integral `620,491`,
remaining-length `1,623,168`, Courant sign `346,080`, fractional tail
`371,760`, inflow sign `353,664`, and two projection nonnegative-reduction
guards. The invalid-domain bitmap is zero, but the unresolved bitmap remains
`0xdffffe`. The trace identity is
`a4ae55166d376dcffedb4aaa9ab4d62ed1261fee797ab2210456bfca9850a2d9`.

This is the intended effect of fresh review: the production reformulation and
its r118/r119 evidence remain admitted, while an asserted site label cannot
stand in for an independent two-path proof. Canonical exit `237` remains until
each pending source site has its own source-bound alternate-path model,
Lipschitz factor, downstream envelope, and mutation RED. `B_fp32`, Metal/fp64
confirmation, temporal refinement, eight-slice readmission, and source maps did
not run. Durable evidence is
`rendered/fire_production_calibration/r124_branch_discharge/branch_discharge.v1`,
SHA-256 `238195f1c197b6a5abdcdd4c4862f85d4213a803bb192adfd0c873a5938bee96`.

### 7.42 Floor-partition branch envelope (r125)

The first r125 class campaign discharges every executed floor-partition
obligation.  At an integer crossing, the fractional-tail and whole-cell paths
integrate the same PPM polynomial.  Their exact switching-surface difference
is zero; over predicate ambiguity `delta`, the common integral is Lipschitz
with outward bound `2 M delta`, where `M` is the independently walked absolute
profile enclosure.  The factor two also covers the projection prolongation
case, whose adjacent coarse values may have opposite signs.

The alternate rounded path is not hidden inside that exact term.  The walker
counts at most `32+24N` scalar operations for the prefix/full-cell realization,
adds `gamma_(32+24N) M (N+4)`, and separately adds
`(32+24N) FLT_MIN` for FTZ.  Half-ambiguity, missing-rounded-term, and
missing-cycle-path mutants all fail.  The trace consumes the same three-part
envelope and binds its per-stage maximum independently.

This closes all `373,245` floor instances.  The cumulative census is
`1,025,656 / 4,340,821` discharged and `3,315,165` pending; exit `237`
therefore remains mandatory.  The largest floor envelope is
`4.486687686924483e302`, arising inside the long projection interval graph.
It is finite and proves topology, but it is intentionally recorded rather than
interpreted as an acceptable `B_fp32`; the completed composition must still
judge whether its analytic radius fits the validation contract.  Durable
evidence is
`rendered/fire_production_calibration/r125_floor_partition/floor_partition.v1`,
SHA-256 `c1c273ed66f2af22e5982435beb38600f2ad9481b0c5d57069cf1f48e277588c`.

### 7.43 Remaining-positive loop envelope (r126)

At `remaining=0`, taking one more local-integration iteration contributes an
exact zero-width PPM slice.  Over outward ambiguity `delta`, its exact branch
term is `M delta`.  The independently enumerated cell-integral and loop-update
path has forty scalar operations, so the rounded term is
`gamma_40 M (4+delta)` and the separate FTZ term is `40 FLT_MIN`.  Mutants
halving `delta`, omitting the cell-integral topology, or deleting FTZ all fail.

All `1,623,168` remaining-positive instances close.  The cumulative census is
`2,648,824 / 4,340,821` discharged and `1,691,997` pending; the maximum class
envelope is `0.49575328199529184`.  Exit `237` remains mandatory.  Durable
evidence is
`rendered/fire_production_calibration/r126_remaining_positive/remaining_positive.v1`,
SHA-256 `9ad1b1de6b35d290753b3c263d170b6e11bb31dfb8e1764c0029b794ad453f68`.

### 7.44 Continuous pressure-open donor transition (r127)

The pressure-open donor switch is genuinely non-equivalent: at zero normal
velocity its two paths return the nearest interior value and the ambient value.
The r124 fallback therefore applies. Shared-alpha transport coupling is not
involved, donor monotonicity is retained by a convex blend, and only exact
binary donor optimality yields within the smallest stable fp32 neighbourhood.

The frozen pre-reformulation walker measured outward predicate ambiguity
`1.7632415612658968e-38`. Its natural velocity scale is
`22.033558699237727`; normalized by `u32=2^-24` this is only
`1.3426012991064678e-32`. The next power-of-two factor after imposing the
minimum stable one-unit fp32 width is therefore exactly one:

```
s = max(FLT_MIN, abs(velocity), abs(dx/dt))
w = u32 s
```

For signed inward velocity `v`, the donor is nearest for `v<=-w`, ambient for
`v>=w`, and the convex linear interpolation between them inside. Both joins
are continuous, the donor stays in the closed nearest/ambient interval, and
legacy bytes are identical outside `[-w,w]` on both open sides. The independent
two-path envelope is the exact term
`|ambient-nearest| delta/(2 w_lower)`, plus
`gamma_12 (4 max(|nearest|,|ambient|)+|ambient-nearest|)`, plus
`12 FLT_MIN` for FTZ. Half-width, missing donor contrast, missing rounded
term, and restored binary-branch mutants all fail. Strict CPU and MSL source
topology and an interior-ramp full-remap byte comparison bind the implementation.

The reformulation removes all `353,664` old binary inflow-sign obligations;
the new joins are resolved on the frozen tier-6 state. The graph now contains
`3,972,323` obligations, `2,633,990` discharged and `1,338,333` pending.
Exit `237` remains mandatory. The 104-step restoration rerun remains below
the unchanged ceiling (`1.5439012582030287e-4` probe plateau,
`6.5237316812827295e-4` field plateau), and all r119 scalar, velocity, and
inventory limit balls remain accepted after exact evidence re-pinning. Durable
class evidence is
`rendered/fire_production_calibration/r127_inflow_transition/inflow_transition.v1`,
SHA-256 `5d95d4d062ae2dd8ec3533d7c802b0369582dcc2222fdc958bd8fde2ecfba198`.

### 7.45 Courant-orientation equivalence envelope (r128)

Both periodic and pressure-open swept-volume orientations vanish at Courant
zero. Across an ambiguity `delta`, their one-cell donor slopes differ by at
most `2M`, so the independent exact branch term is `2 M delta`. The walker
enumerates the common setup and the longer negative/local successor as 48
scalar operations, producing `gamma_48 M (8+delta)` plus `48 FLT_MIN` for
FTZ. The authored `courant>=0` comparison sends both IEEE signed zeros through
the same positive path. Half-ambiguity, missing-negative-path, missing-FTZ,
and noncanonical-signed-zero mutants all fail.

All `346,080` Courant obligations close. The cumulative census is
`2,980,070 / 3,972,323`, leaving `992,253` pending; exact exit `237` remains
mandatory. The maximum Courant class envelope is `1.1830324528164238`.
This is a topology envelope, not an admitted composed `B_fp32`. Durable
evidence is
`rendered/fire_production_calibration/r128_courant_sign/courant_sign.v1`,
SHA-256 `482b58d038b200bd1a31cb006be76ac99ba64dd3ffa16b6fd68bd68dc1925e7e`.

### 7.46 Fractional-tail equivalence envelope (r129)

At fractional length zero, taking the optional trailing PPM branch adds a
zero-measure slice. Across ambiguity `delta`, the exact omitted contribution
is bounded by `M delta`. The independent trailing-polynomial and accumulation
DAG contains 24 scalar operations, yielding `gamma_24 M (4+delta)` and a
separate `24 FLT_MIN` FTZ term. Half-ambiguity, missing-trailing-integral, and
missing-FTZ mutants all fail.

All `371,760` fractional-tail obligations close. The cumulative census is
`3,351,830 / 3,972,323`; `620,493` remain pending, so exact exit `237`
continues. The maximum class envelope is `0.29684226235298722`, and the
unresolved stage bitmap contracts to `0xdbdef6`. Durable evidence is
`rendered/fire_production_calibration/r129_fractional_tail/fractional_tail.v1`,
SHA-256 `7ed9b0be19306d1448c197d669b206578109a600d13369948ec861a436c6b1db`.

### 7.47 Flat-integral equivalence envelope (r130)

The curved PPM unit-cell integral and its flat shortcut coincide exactly when
the left, center, and right values coincide. If either endpoint lies within
the independently enclosed deviation `D` of the center, direct coefficient
collection bounds the exact two-path difference by `8D`. The independent
curved-polynomial and accumulation DAG contains 32 scalar operations, adding
`gamma_32 M (8+D)` and a separate `32 FLT_MIN` FTZ term. Half-deviation,
missing-curved-polynomial, missing-FTZ, discontinuous-shortcut, and mutated
quadratic-coefficient/source-topology mutants all fail.

All `620,491` flat-integral obligations close. The cumulative census is now
`3,972,321 / 3,972,323`; only the two projection-reduction guards remain, so
exact exit `237` continues. The maximum class envelope is
`128175.45885830303`, a topology certificate term rather than an admitted
`B_fp32` bound. The unresolved bitmap is `0xc00000`. Durable evidence is
`rendered/fire_production_calibration/r130_flat_integral/flat_integral.v1`,
SHA-256 `1468fed5cbab33d7f79282fde50b9a83a5d0bbc7b874ca5a8355ca7fd89db608`.

### 7.48 Projection-reduction guard proof and campaign closure (r131)

Both physical and restoration validation guards consume a reduction initialized
from positive zero and updated only as `max(current, abs(residual))`. Absolute
value and maximum preserve nonnegativity for every finite IEEE binary32 input,
including underflow and FTZ. Therefore the negative alternate is structurally
unreachable: the exact branch, rounded, and FTZ divergence terms are all zero.
An independent graph walk counts every leaf absolute and maximum update and
rejects signed-leaf, negative-seed, subtractive-reduction, and raw-provenance
mutants. Raw arithmetic is explicitly untagged; only the positive-zero seed,
absolute leaves, and maximum nodes may carry the proof. Source gates bind the
production result's `+0` initialization, the post-residual `max(abs())`
reduction, and both validation consumers.

The last two obligations close. The final census is
`3,972,323 / 3,972,323`, pending zero, with unresolved and invalid bitmaps both
`0x000000`. Exit `237` remains reserved for incomplete branch proofs; exact
exit `240` denotes branch-campaign completion. This unlocks derivation of
`B_fp32` but does not define or measure it. Durable evidence is
`rendered/fire_production_calibration/r131_projection_reduction/projection_reduction.v1`,
SHA-256 `d6cdacdbbcd02f9a1d6262553c3098a9abc1cb90001b012840581908bdf7c8a4`.

### 7.49 Fixed-grid projection interpolation proof (r132)

The first post-obligation composition exposed a proof-instrument defect before
any Metal measurement: the generic transport floor certificate had been
applied to the projection prolongation index floor.  It multiplied a fixed
grid-topology decision by the pressure-profile enclosure and contributed
`4.486687686924483e302` to the physical projection.  That number was neither a
kernel excursion nor an admissible `B_fp32` term.

Projection interpolation coordinates are now certified from the exact integer
rational

`((2*i+1)*Nc-Nf)/(2*Nf)`.

The independent walker compares its side of every integer boundary with the
sequential binary32 expression used by production; a certificate is admitted
only when both choose the same side.  The tier-6 `24x24x36` hierarchy has 45
such exact boundaries per V-cycle, hence 765 physical and 720 restoration
instances.  All are stable with zero branch divergence.  Coordinate-shift and
division-reassociation mutants RED, and the generated trace is source-bound to
the interpolation scope.  The physical and restoration output radii collapse
to `1.352840804874779e-7` without changing any production arithmetic.

The complete census remains `3,972,323 / 3,972,323`, pending zero, and exact
exit `240` remains the no-Metal topology-completion result.  This correction
does not define `B_fp32`; it removes a false pressure-dependent branch term so
the full-stage recurrence can be attempted.  Durable evidence is
`rendered/fire_production_calibration/r132_projection_interpolation/projection_interpolation.v1`,
SHA-256 `d8df1a96842b52ff053017014f177096a08e0591307a0d196b0bf14b31549198`.

### 7.50 Metric-level B_fp32 derivation refusal (r133)

The first metric-level recurrence does not produce `B_fp32`.  Before this
attempt, branch-class maxima were added to every stage output and NaN radii
were silently ignored by `std::max`.  r133 makes both behaviors fail closed:
each two-path envelope is attached only to the swept integral that executed
it, metric channels retain mean/RMS/count evidence, and every NaN radius is
canonical positive infinity.

All nine scalar channels remain finite through the five cell maps and the
explicit `+0` source stage.  The fixed-cycle projection interval, however,
becomes unbounded under naive dependency propagation.  Every published
velocity radius is nonfinite: `21600,21600,21312` faces for each of the
physical and restoration solves.  The first physical x-face still has finite
center/rounded values `0.020628967447918926` / `0.0206289645`; this is a proof
instrument failure, not a nonfinite kernel result.  The corresponding
restoration pair is `0.019031353974387526` / `0.0190313533`.

Canonicalizing the hidden NaNs exposes three genuine pending obligations: one
unclassified comparison and both projection-validation predicates.  The
census is therefore `3,972,323 / 3,972,326`, and the projection bits in both
the unresolved and invalid maps are `0xc00000`.  Under the amended r120 rule,
exit `237` is restored.  The required repair is an independent finite
condition/amplification proof for the fixed multigrid schedule, with local
per-sweep rounding terms; a Metal/fp64 measurement may neither define nor
widen that proof.  Consequently Metal confirmation, temporal refinement,
eight-slice readmission, and thermo/source maps remain unrun.

Durable evidence is
`rendered/fire_production_calibration/r133_bfp32_projection_refusal/bfp32_projection_refusal.v1`,
SHA-256 `1512191c5966ad3eb981b2a2e6e205f79d9666f26d3cbfb58a3094c47b5c853e`.
The golden checkpoint is unchanged.

### 7.51 Projection a-posteriori rounding certificate (r134)

The r133 dependency-interval refusal is closed without folding the iterative
pressure history.  For the accepted pressure-open solve, let
`A=G^T rho_f^-1 G` and let `r` be the published divergence residual.  The
independent walker uses the discrete Poincare lower bound
`4/N^2` for an axis with two pressure-open ends and `1/N^2` for an axis with
one open end (from `sin(x)>=2x/pi`).  Loewner ordering with the certified face
density range then gives

`lambda_min(A) >= lambda_0/(rho_max h^2)`.

The velocity-error gain is conservatively
`sqrt(2/(rho_min lambda_min(A)))`; the factor two encloses the doubled
one-sided open-face gradient.  The terminal bound compares the rounded
pressure against the exact-promoted binary64 operator on the same stored
inputs.  The cross-precision divergence defect therefore contains coefficient
and pressure-open RHS perturbations, and the active-set bitmap must agree
exactly.  With `R_64` the binary64 residual gate, the bound is

`B_projection = gain * (R_cross(p32) + R_64) + B_terminal-face`.

`B_terminal-face` is accumulated in unique-face L2 and normalized per cell
before the Hodge estimate is mapped to the published cell-centered RMS.  This
ordering is load-bearing: cell-centering first can hide a divergence-free
endpoint mode.  The direct face term contains density-division and open
boundary-pressure rounding, while `R_cross` contains the induced pressure
response through `A^-1`.

`R_64` is not borrowed from the binary32 tolerance.  The independent walker
derives it from the binary64 residual DAG (7 operations for the physical
residual, 14 for restoration), the binary64 tolerance DAG, and the same
structural velocity gain.  For the physical output-dependent tolerance it
solves the outward self-consistent inequality using
`U_64 <= U_32 + sqrt(N_cell) B_projection`; its feedback factor is
`0.23426947265986475 < 1`.  Restoration's target-scaled tolerance has feedback
`1.5955915929254457e-11`.  This is a structural fixed point, not a measured
binary64 residual.  A separate beginning-velocity division envelope enters
the velocity scale.  The binary64 terminal term is not a gamma-count
surrogate: an independent outward-binary64 interval evaluator walks every
stored-density division, normal/tangential total-head operation, doubled open
gradient, momentum update, and terminal division for every unique face.  Its
L2 envelope is added outside `A^-1`; neither a beginning error,
boundary-pressure rounding, nor a divergence-free endpoint mode can be hidden
in the residual anchor.

No fitted or measured constant enters this expression.  On the frozen tier-6
state, the outward exact-promoted face-density interval is
`[0.97449028796070902,1.1348451536709214]`,
`lambda_0=0.016975308641975297`, `lambda_min(A)>=8.9899266871598034`,
`||A^-1||<=0.11123561234690459`, and the velocity gain is
`0.47780222212961759`.  The physical projection has residual
`8.9943569037131965e-7`, residual-evaluation envelope
`1.3748435749320591e-7`, cross-precision residual upper
`1.9319781954175433e-6`, binary64 residual-gate upper
`0.00034336556400244998`, terminal face term
`9.7212486067771285e-9`, and derived velocity RMS bound
`0.00016499365420669603`.  The restoration cross residual, binary64 gate,
face term, and final bound are `2.3628878941959103e-5`,
`0.00046519335364055106`, `1.1190657711221316e-8`, and
`0.00023357153961206769`.

Both validation predicates now carry their own two-path certificates.  The
residual upper envelope is strictly below the tolerance lower envelope by
`0.00026143109675737545` (physical) and `0.00044094270347925889`
(restoration), so binary32 predicate rounding cannot invalidate the
a-posteriori anchor.  The formerly unknown comparison is the
positive-zero-seeded `max(abs(velocity)) < 0` guard and is discharged by the
r131 nonnegative-reduction provenance proof.  The census is again
`3,972,326 / 3,972,326`, pending zero, with both bitmaps zero.  Raw pressure
dependency radii remain infinite and recorded; they are not used by the
certificate.

Exact exit `241` denotes projection-local certification.  It does not yet
define the composed `B_fp32`: upstream force/transport/source contributions
must still be folded before any Metal/fp64 measurement.  Durable evidence is
`rendered/fire_production_calibration/r134_projection_aposteriori/projection_aposteriori.v1`,
SHA-256 `18f0115216fb4654eb4b7a37466ded1788158d9e4b55eacf15811e986b651ec2`.
The golden checkpoint is unchanged.

### 7.52 Full-step analytic B_fp32 candidate (r135; rejected by r136)

The frozen r120 trace was provisionally composed across the complete resident step before
any Metal comparison.  Each shared-alpha monotone conservative FCT submap is
substochastic in both the one- and infinity-norm; therefore it is
nonexpansive in L1 and, by `||A||2 <= sqrt(||A||1 ||A||inf)`, in L2.  The
independent walker outward-sums the five cell submap envelopes plus the source
publication for each conservative channel.  The fixed physical mollifier is a
positive unit-sum operator, so it cannot enlarge component L1; the same raw
mean bound controls each inventory per physical volume.

For momentum the walker accumulates the force publication and the five dual
submaps of each transported component in unique-face L2 per cell.  Conversion
to provisional velocity includes density division rounding and the composed
gas-density RMS interaction.  The physical projection then includes the
structural `sqrt(rho_max/rho_min)` feedthrough, coefficient perturbation of the
measured correction norm, the derived pressure-open total-head interaction,
and the r134 local certificate.  Restoration repeats the structural
feedthrough from the physical output, adds its coefficient interaction, and
finally adds its distinct r134 local certificate.  Thus the second projection
is not treated as an independent addend.

The derived filtered-scalar and inventory bounds are
`[3.2719950722423746e-4,2.6175964690034235e-4,1.3965273494462376e-3,`
`4.6634001371138678e-3,1.7417247803291333e-4,1.4697468215851725e-4,`
`2.28515847259952e-6,4.8013754918650386e-7,5.2181563701838843e2]`.
The momentum component bounds are
`[1.3505921107054692e-2,1.2696844714689289e-3,1.1935802483893404e-4]`.
The gas-density RMS and relative coefficient terms are
`3.5444612811895516e-2` and `3.6372463891938378e-2`; provisional velocity is
`1.6724925504348544e-2`, the pressure-open interaction is
`5.5034168358969525e-4`, the composed physical output is
`1.8948201018992382e-2`, and the final two-projection velocity B_fp32 is
`2.1145425678289562e-2 m/s`.

This entry records the pre-review candidate, not an accepted bound.  The fresh
r136 boundary review disproved its propagation assumptions, so none of these
numbers enters `B_fp32` or the additive contract.  The fp64/Metal comparison
did not run and did not select any term.  Historical exact exit `242`, trace digest
`086c6b06d0e2d9e5087c294eb95b750bf8225f553fc99f085273ac023786c284`,
and durable evidence
`rendered/fire_production_calibration/r135_full_step_bfp32/full_step_bfp32_derivation.v1`
bind the result.  Measurement, temporal refinement, and eight-slice
readmission remain downstream gates.

### 7.53 Full-step composition proof refusal (r136)

Fresh review rejected r135 before Metal measurement.  A nonlinear shared-alpha
FCT submap is not a fixed substochastic matrix: alpha is the minimum across the
whole tuple, so an error in one component changes every other component, and a
positive conservative compressive map can have L2 gain `sqrt(2)>1`.  Therefore
the stage resets cannot discard propagated and cross-component radii.  The
projection composition also used density RMS where a pointwise coefficient
envelope and a variable-density resolvent bound are required.  Its product
rule was invalid: on four equal-volume cells, `a=b=(2,0,0,0)` has
`RMS(a)=RMS(b)=1` but `RMS(a*b)=2`.  The same defect appears in the
pressure-open quadratic term for localized velocity error.

The executable refusal additionally binds four evidence-surface omissions:
source-stage momentum and gas-reduction rounding were not metric-observed; only
the first of eight frozen divergence targets was instantiated; metric
cardinalities were trusted from the trace instead of independently derived
from shape; and the durable artifact did not bind the fixture assembling the
24-stage DAG.  These eight gaps are the exact `0xff` proof bitmap.  The rejected
`2.1145425678289562e-2 m/s` candidate is retained only to make the failure
reproducible.  `B_fp32` remains undefined, no additive contract is formed, and
no Metal measurement, temporal refinement, or eight-slice readmission is
permitted.  Canonical exit is again `237`; evidence is
`rendered/fire_production_calibration/r136_full_step_refusal/full_step_bfp32_refusal.v1`
(SHA-256 `19732a3864fbcbe43a4fa872d4c4311732820248934bfc9bc1627b0ae0ff33ad`).

### 7.54 A-posteriori full-step B_fp32 by subdominance (r137 amendment)

Owner ruling amends only the composed full-step part of the frozen r120
protocol.  The streaming-stage walker envelopes remain finite diagnostics,
but they are not composed through nonlinear FCT or the variable-density
projection resolvent.  Instead, each of the eight independently restarted
golden slices must execute the same production scheme twice: the resident
binary32 Metal path and the strict binary64 CPU mirror with identical
stencils, force substeps, five cell remaps, fifteen dual remaps, physical
projection, and restoration projection.  The oracle is forbidden from this
precision comparison.

For every filtered scalar, filtered velocity, and inventory quantity `q`,
precision certifies only when

`|P32_q - P64_q| <= 2^-3 E_P,q`,

where `E_P,q` is the already-derived tier-6 production distance to the mutual
limit.  It is the outward maximum of the `{5,10}` coarse distance rescaled by
`(5/6)^1.8` and the `{6,12}` coarse distance, with each coarse distance
`D/(1-2^-1.8)`.  Separately, scheme fidelity requires

`|P64_q - O_q| <= E_P,q + E_O,q`.

These are independent gates: passing the scheme triangle cannot excuse a
precision miss, and passing subdominance cannot excuse a scheme miss.  The
pre-registered tier-6 velocity terms are `E_P=5.890459160549289e-3 m/s` and
subdominance `7.363073950686612e-4 m/s`.  The old `3e-5 m/s` preliminary guard
is retained until measurement; if the new derived rule supersedes it, the
measurement entry must report both numbers.  Source-map production remains
restricted to bit-exact `+0` until a same-scheme binary64 source mirror passes
this mechanism.  First-light is authorized only after Metal source maps run,
as an uncertified monitored-acceptance `preview_primary`, `sequence_backed`
artifact alongside—not as evidence for—the burning-state campaign.

This amendment is sealed before measurement in
`rendered/fire_production_calibration/r137_subdominance_protocol/subdominance_protocol.v1`
(SHA-256 `833137b54fbd507fc3b6bdcc960a23b89be60ee835f7b1ca177f93cc57d63922`).
Its rationale is the exact r136 `0xff` review: useful a-priori composition is
blocked by nonlinear shared-alpha gains, coefficient/resolvent interactions,
and the six evidence-surface gaps.  The rejected `2.1145e-2 m/s` candidate is
diagnostic only; no empirical value selects any bound in this amendment.

### 7.55 Same-scheme subdominance pilot and golden repair (r138/r140)

The tier-6 analytic pilot is green on all eight frozen target payloads.
Each slice restarts from the same SHA-bound smooth beginning, receives one of
the eight frozen divergence-target payloads, and executes two copies of the
same production step: resident Metal binary32 and the strict CPU binary64
mirror.  Both copies use the Metal-selected force schedule and the identical
force, five cell-remap, fifteen dual-remap, exact-`+0` source, physical-P2,
and restoration-P2 topology.  The oracle does not participate.  Both
projections validate on all eight slices, and the resident path reports zero
interstage full-grid transfers.

The maximum measured filtered-velocity difference is
`1.1165273069908068e-9 m/s`, inside the derived
`7.363073950686612e-4 m/s` subdominance bound by a factor of
`659462.05745125038`.  It is also inside the old preliminary `3e-5 m/s`
guard by `26869.024888297707`, but the wrong-state pilot cannot supersede that
guard; supersession remains pending the golden-slice measurement.  Across the nine filtered scalar channels, the smallest
bound/measurement margin is `1542.2120195226826`; across the nine inventory
channels it is `2361.071848300599`.  All `8*(9+1+9)=152` precision gates are
green.  These margins are calibrating observations only: none selects or
widens a bound.

The exact pilot trace is `f90a2508...551cebf`, canonical success is `243`,
and durable evidence is
`rendered/fire_production_calibration/r138_subdominance_measurement/subdominance_measurement.v1`
(original SHA-256 `ffeeaa6e...1e50e0e`).  Fresh review correctly rejected
this as certification of r137's `shared_golden_beginning_per_slice` class:
the pilot reused one smooth tier-6 beginning and therefore remains diagnostic.

Before golden evidence, r138a seals the root plus all seven r95 continuation
hashes.  The corrected campaign then fails closed on slice zero before its
precision result may publish.  The physical projection validates
(`37.872448 -> 0.00374865532 s^-1`), but the 16-cycle restoration projection
ends at `9.97165444e-6 s^-1` against its own unchanged
`1.06855828e-6 s^-1` band, a `9.331877` miss.  No fp64/Metal precision result
is admitted or recorded after that failed prerequisite.  Exact exit `244`
is emitted only after the slice index, projection topology, all six diagnostic
float values, and the post-run golden hash match; the ordinary macOS suite
executes this exact refusal.  No full-step `B_fp32`,
temporal term, or additive contract is admitted; changing the independently
derived restoration criterion is an owner-level contract ruling.  The golden
checkpoint is unchanged.

### 7.55a Plateau-derived restoration validation (r141, pre-evidence)

The owner ruling records r118's `0.005 max|R_n|` band as a derivation error at
the design level.  The restoration pass exists to keep the manifold deviation
plateau below its EOS ceiling; borrowing the physical projection's fractional
residual rule was an over-literal interpretation of “derived the same way.”
The replacement is frozen before measurement.  With EOS ceiling `C=1e-3` and
power-of-two headroom `h=2^-2`, the allowed field plateau is
`C(1-h)=7.5e-4`.  Separately for cold calibration and burning golden regimes,
the removed-restoration counterfactual measures

`G_field=max_cell |d_removed^(n+1)-d_beginning^n|`, where `d=V(Q)-1`.

The required drain is `r_req=G_field/(C(1-h))`; the corresponding mechanism
criterion is `residual <= (1-r_req) max|R_n|`.  Runs at restoration cycle
counts 1 through 16 reuse identical beginning, target, physical pass, and
source bytes.  Their residual curve determines the minimum cycle count if 16
does not meet the derived fraction.  No empirical tolerance or chosen cycle
count is permitted after inspecting the curve.  The function-level RED is the
104-step golden long shadow: the maximum field deviation over its final 32
steps must remain `<=7.5e-4`; removed restoration must reproduce secular
blowthrough, and substituting the physical target must still fail the resident
ownership seam.  The pre-evidence artifact records no measurement and changes
no production behavior.

### 7.55b Burning-regime capacity result (r142)

The exact preregistered campaign finds a capacity failure before a replacement
validation band can be admitted.  At golden slice zero, removing restoration
produces

`G_field=2.5328069638265172e-3`

at cell `3227` (beginning deviation `-1.1871614802316799e-12`, output
deviation `-2.5328069650136786e-3`).  The corresponding cold tier-12 result is
`1.2031080315666465e-4`, so burning generation is
`21.052198949485707` times the cold value.  The frozen headroom allows only
`C(1-h)=7.5e-4`; therefore the burning requirement is
`r_req=3.3770759517686897 > 1`.  This violates a necessary condition even for
perfect deadbeat restoration.

The mechanism curve confirms that work count is not the limiting variable.
Sixteen cycles drain `0.9533406144549903` of the initial restoration residual,
ending at `9.971654435503297e-6`; cycles 4 through 16 are effectively flat at
that endpoint.  Cold requires drain `0.1604144042088862` and receives
`0.99562928290235475`.  More burning cycles cannot compensate for
`G_field>C(1-h)`, so no derived cycle count exists and the 104-step long-shadow
gate cannot possibly satisfy its necessary field-generation bound.  Calling
this a projection residual floor would be incorrect: the stop is caused by
per-step manifold generation exceeding the ruled plateau capacity.

The exact replay returns `253` only after byte-pinning both residual curves,
their sweep topologies, the signed field witnesses, the cold/burning ratio,
and the post-run golden digest.  The removed-restoration counterfactual uses
the same 17-cycle physical arithmetic as the production path and must match
its physical pre/post diagnostics on every cycle-curve invocation; the pinned
physical sweep counts are `1156` burning and `1054` cold.  Durable evidence is
`rendered/fire_production_calibration/r142_burning_plateau_capacity/restoration_capacity_evidence.v1`.
No replacement validation band is installed, no production arithmetic is
changed, and precision measurement, temporal refinement, additive-contract
formation, source maps, and first light remain blocked pending an explicit
architecture ruling.

### 7.55c Manifold timestep protocol (r143, pre-evidence)

The r142 capacity finding is resolved in the existing section-3.9 pin-4
timestep class.  `G_field` is the per-step advective dose at the transported
burning front; the measured `21.052198949485707` burning/cold ratio is physical
contrast, not a producer defect.  Every accepted step atomically records its
represented timestep `dt_prev`, field generation `G_prev`, and delivered
restoration drain `r_prev`.  The next selector forms

`dt_manifold = dt_prev * ((1-h) * C * r_prev) / G_prev`,

where `C=1e-3` and `h=2^-2`, and takes the minimum with the advective,
buoyant, explicit-diffusion, and `1.1*dt_prev` growth limits.  Exact positive
zero generation contributes no limit.  Missing metadata on the first step of
a run is intentional: that step uses only the CFL-family and growth rules.
Partial, nonfinite, or nonphysical metadata on a resumed accepted trajectory
fails closed rather than silently disabling the limiter.

The current step's scalar transport determines `G_field` before restoration.
It therefore also determines the mechanism criterion
`r_req=G_field/(C*(1-h))` and
`post_residual <= (1-r_req)*pre_residual`.  `G_field>C*(1-h)` is impossible for
that step and fails acceptance.  Independently, terminal accepted scalars must
satisfy `max |V(Q)-1|<=C*(1-h)`.  This function-level gate detects a burning
regime change that grows faster than the previous-step predictor.  Only scalar
reductions may cross the diagnostic seam; no full grid may leave residency.

Using r142's represented inputs predicts `dt_manifold=
1.589201814710624e-5 s`, a `3.542360307075882x` tightening from
`5.629525428363875e-5 s`.  This is explicitly not evidence.  The frozen Metal
campaign must reproduce the selector, realized plateau, device/wall p95,
tier-10 times 25-second projection, and at least 100 resident steps before the
protocol can unlock golden-slice precision work.  The pre-evidence artifact is
`rendered/fire_production_calibration/r143_manifold_timestep_protocol/manifold_timestep_protocol.v1`.

Rejected alternatives remain explicit.  A restoration gain would introduce a
regime-dependent gamma window and abandon deadbeat semantics.  Widening `C`
mixes solver capacity with the r60 admissibility unit system.  State repair
mutates conservation ledgers and masks producer defects.  Production's omitted
r70 Picard advective-anomaly closure remains a follow-up only if the two-pass
cost threatens the certified budget; it is not the current remedy.

### 7.55d Manifold predictor result (r144)

The selector and fail-closed plateau mechanism are implemented, but the frozen
burning measurement rejects the previous-step proportional predictor.  The
r142 tuple selects `1.589201814710624e-5 s`, represented as
`0x1.0a9fb2p-16` (`1.5892017472651787e-5 s`), versus the golden CFL step
`5.6295254283638751e-5 s`: a `3.5423604574130363x` tightening.

The actual resident step generates `G_field=2.5081625752932935e-3`, or
`0.9902699302058174` of r142's generation.  It therefore does not exhibit the
assumed linear-in-dt reduction at this burning front.  Its terminal field
maximum is `2.5081625764804549e-3`, `3.3442167686406066` times the ruled
`7.5e-4` allowance.  The restoration solve itself drains
`0.97489008508207653` (`7.5704371556639671e-4 ->
1.9009303287020884e-5 s^-1`), but the realized generation requires drain
`3.3442167670577247`; consequently no nonnegative per-application residual
band exists.

The terminal EOS calculation is performed only at the existing step-boundary
staging seam and is independently recomputed by the fixture.  No full-grid
interstage transfer is added.  With the diagnostic disabled, the owner refuses
the step before publication and resets every public result field.  Exact exit
`254` is reserved for the pinned miss.  Five observed trials give
`74.077416793443263 ms` device p95 and `711.14358400000003 ms` wall p95;
at the represented step, tier-10 times 25 s projects to `32.37011955186228`
device-hours and `310.75331496518044` completed-call hours.  These timing values
are observations, not acceptance constants.

This is the ruling's explicit fast-regime-change detector firing, not grounds
to alter the ceiling, gain, or band.  The 100-step shadow, golden `B_fp32`,
guard supersession, temporal refinement, eight-slice readmission, source maps,
and first light remain blocked.  Evidence is
`rendered/fire_production_calibration/r144_manifold_predictor_stop/manifold_predictor_evidence.v1`;
the golden checkpoint remains byte-identical.

### 7.55e Review closure and accepted-observation lifecycle (r145)

Review did not change the r144 physical result.  It strengthened the boundary
around it.  The resident terminal EOS observable now validates the complete
nine-component Binary32 state through the single r60 admissibility predicate
before temperature/volume reconstruction.  Gross energy underflow/overflow
and affine-row violations therefore fail rather than being converted into an
endpoint temperature.  A malformed plateau-evidence environment value fails
before Metal, and the normal plateau rejection is checked against the complete
default public result, not a selected subset.

The next-step manifold observation has one publication seam.  It requires a
finite positive accepted step, Binary32 producer metadata, validated physical
and restoration projections, exactly two resident projection invocations,
zero interstage full-grid transfers, a passing function-level plateau, and
finite `G`, required/delivered drain, and mechanism-band diagnostics.  Only
then may `(dt,G,r)` enter the selector.  Checkpoint format 11 persists this
tuple; legacy formats 5 through 9 explicitly restore it as unavailable, which
is the first-step/CFL state rather than an inferred zero-generation sample.

The exact closure replay retains r138 exit `243` and trace
`f90a2508...551cebf`, r142 exit `253`, and r144 exit `254` with the same
`3.5423604574130363x` tightening, `G=2.5081625752932935e-3`, field ratio
`3.3442167686406066`, and delivered drain `0.97489008508207653`.  r136's
source-bound trace becomes `c917ea32...ccfcf94` because the r143 force API is
now present in the generated trace manifest; its arithmetic pins, topology,
and `0xff` refusal are unchanged.  Five new timing trials observe
`70.079583441838622 ms` device and `600.47158400000001 ms` wall p95,
projecting to `30.623158748670253` device-hours and
`262.3922080838077` completed-call hours for tier-10 times 25 s.  Timing is
still diagnostic, not an acceptance constant.  Durable closure evidence is
`rendered/fire_production_calibration/r145_manifold_predictor_closure/manifold_predictor_closure.v1`.

The strengthened lifecycle does not rescue the predictor: required drain is
still `3.3442167670577247>1`.  The long shadow, golden `B_fp32`, temporal
refinement, eight-slice readmission, source maps, and first light therefore
remain unrun and blocked at the same function-level detector.

### 7.55f Authoritative accepted-observation lifecycle (r146)

Fresh review found that r145's serialized tuple was not yet authoritative.
The publication seam now recomputes required drain, delivered drain, and the
mechanism band from the accepted result and rejects any caller-authored
disagreement.  Checkpoint v11 requires the observation timestep to equal both
`previousStepS` and `lastAcceptedStepS`; v5--v9 decoding resets the complete
tuple before reading, so a reused destination cannot inherit stale capacity
state.  The retained r118 resident owner now performs the actual lifecycle:
successful Binary32 result -> validated observation publication -> next-step
selection -> checkpoint state.  The binary64 capstone/oracle loop remains a
different producer and intentionally uses its own CFL selector.

Publication is lifecycle logic, not same-scheme arithmetic, and is therefore
mechanically removed from both generated numeric mirrors.  The current r136
source-bound trace is `4cb7e6cf...75ef3bcb`; its 24-stage values, complete
branch census, `0xff` proof-gap bitmap, and exact refusal are unchanged.
Retained r119 accepts all scalar/velocity/inventory channels, r138 reproduces
`f90a2508...551cebf`, and r142/r144 reproduce exact `253/254`.

The function result remains a stop.  At represented
`1.5892017472651787e-5 s`, realized field deviation is
`2.5081625764804549e-3` against `7.5e-4`; required drain is
`3.3442167670577247` while delivered drain is `0.97489008508207653`.
The latest five-trial r144 observation is `71.6713 ms` device and
`633.3460 ms` wall p95, approximately `31.32/276.76 h` for tier-10 times
25 s.  The retained r118 lifecycle replay is numerically byte-identical and
device-stable (`27.6619 ms` p95), but its `329.4053 ms` wall p95 does not
re-certify the historical `200 ms` wall budget.  A measured parallel host-EOS
experiment changed that to only `328.0007 ms` and was reverted.  No budget or
physics bound moved.  Evidence is
`rendered/fire_production_calibration/r146_accepted_manifold_lifecycle/accepted_manifold_lifecycle.v1`.

### 7.55g Producer-owned observation authority (r147)

Fresh review found three remaining authority gaps in r146.  The public resident
result could be published with a caller-selected timestep, a coordinated rewrite
of `G`, field deviation, drain, and band could remain internally consistent, and
an unavailable v5--v9 observation could bypass the production limiter after a
resume.  A second review also found that a bearer token and mutable published
tuple would merely move those authority gaps.  The resident owner now records
the represented binary32 timestep and creates a private, copy-clearing token
only after both projections validate and both the mechanism and function gates
pass.  The token binds the physical residual evidence and a field-tagged,
length-delimited digest of the complete resident payload, as well as all
plateau diagnostics.  Publication requires exact agreement, consumes the token,
and returns an opaque observation whose fields are read-only outside the
producer and complete, library-owned validated checkpoint codec.  Its persisted
record carries the producer seal; replay can restore an authentic tuple, while
changing any finite `(dt,G,r)` value with the same seal rejects.  The
observation retains the payload digest, so the state-application seam rejects
mutation after publication as well.
Public diagnostics remain observable, but they no longer confer authority.  An
unavailable observation is accepted only with `previousStepS=0`; legacy
production resume therefore fails closed instead of silently reverting to CFL.
The binary64 oracle remains on its independent five-argument CFL/growth selector;
the missing-observation rule is production-only rather than a mixed lifecycle.

The lifecycle gate is now executable rather than format-only.  It advances two
real tier-12 resident steps.  Before the first publication it proves that token
copy, wrong caller timestep, scalar and vector-boundary payload mutation,
physical-validation evidence mutation, coherent diagnostic forgery, and field
mutation all reject; successful publication consumes the token, immediate
replay rejects, and post-publication velocity mutation cannot be applied.  It
then updates the represented timing fields,
writes and reloads checkpoint v11, and passes the reloaded observation to the
second selector.  Separate REDs reject both writer-side timing mismatches and a
checksum-valid malformed-v11 reload.  Exact exit `255` binds first-step
`G=1.2031080315688669e-4`, drain `0.99562928290235475`, and resumed selection
`0.0018513042677754073 s` (`advective_CFL`).  The cold manifold candidate is
valid but subdominant to CFL; the separately bound selector RED proves that an
accepted burning observation can own the active `manifold_plateau` limit.

Retained r118 physics is unchanged: probe plateau
`1.5439012582030287e-4`, field plateau `6.5237316812827295e-4`, trace
`719ee45e...f68cb`, and final state `d9a1a0ea...c9dc2`.  Its latest timing is
`27.7856 ms` device and `325.3877 ms` wall p95, so the historical `200 ms`
wall acceptance remains false.  r119 still accepts every spatial scalar,
velocity, and inventory channel; r138 retains trace `f90a2508...551cebf`; and
r142/r144 retain exact `253/254`.  r144's rejected burning result has no token.
The source-bound r136 trace moves to `26b12e46...c7fd9` solely because this
owner API is in the generated source manifest; the arithmetic census, `0xff`
proof gap, and exit `237` are unchanged.
The function-level stop is therefore unchanged: field deviation is still
`2.5081625764804549e-3`, required drain `3.3442167670577247`, and delivered
drain `0.97489008508207653`.  No long burning shadow, golden `B_fp32`, temporal
refinement, readmission, source map, or preview runs.  Evidence is
`rendered/fire_production_calibration/r147_producer_authority_lifecycle/producer_authority_lifecycle.v1`.

### 7.55h Checkpoint-state authority closure (r148)

Fresh boundary review correctly rejected r147's persisted tuple seal.  It was
an unkeyed digest of `(dt,G,r)`: useful corruption detection, but neither
producer authority nor a binding to the state resumed by the solver.  Format
13 removes the raw tuple-restoration API.  The resident owner now issues a
second, domain-separated digest over the normalized producer-consumed Binary32
state: represented shape and cell width, component-major conservative fields,
and terminal restoration momentum and velocity, all with field tags and lengths.
Persisted temperature is required to equal the canonical Binary32 inversion of
those conservative fields, and production selection uses the represented
Binary32 cell width, so neither stored double is an independent state coordinate.
Publication retains that private digest.  The checkpoint writer independently
reconstructs it from the applied state and refuses a transplanted observation;
the library-owned reader reopens the complete checksum-verified payload,
checks the trailing record's full-prefix integrity binding, consumes a complete
normalized state/lifecycle view rather than a caller-authored digest, reconstructs
the same state digest, and only then restores the opaque observation.  The prefix
binding is explicitly an integrity device, not a secret MAC.  Authority comes
from the private producer-issued state digest and the absence of any supported
raw tuple factory.  The selector likewise consumes the current normalized state
view and recomputes its digest; it never accepts a caller-supplied digest scalar.

The lifecycle invariants are now explicit.  A Binary32 checkpoint with accepted
history must carry an available observation whose timestep equals both stored
accepted-step fields and the history tail; count equals history length and the
history duration exactly reconstructs simulation time.  A zero-step state in any
precision is not a legal checkpoint class: production begins in memory from the
byte-identified Binary64 analytic owner, and only an accepted result may persist.  Formats
9--11 cannot resume any Binary32 production state.  Modern Binary64 formats 9--12
also cannot resume because they predate producer-origin authority; historical
Binary64 formats 5--8 remain grandfathered by their pre-resident lineage.
REDs reject an accepted-history/zero-timestep first-step alias on both writer and
checksum-valid loader paths; formats 9, 10, and 11 are exercised independently.
They also reject temperature and terminal-velocity transplants, plus passing a
genuine observation to the selector with a different current-state view.  Clearing
only an accepted state's observation, changing a non-tail history term, or changing
simulation time all fail at the owning selector/writer and checksum-valid loader.
Coordinated clearing of observation, count, time, and history also rejects because
it cannot reclassify accepted Binary32 bytes as an initial checkpoint: the live
owner rebuilds the inferred tier's canonical analytic state and requires an exact
state digest match at step zero.  Retagging the cleared accepted bytes Binary64
therefore also rejects.  Both the v13 writer and checksum-valid v13 loader reject
every zero-step state regardless precision.  The v9/v10/v11 all-zero matrix is
instantiated separately for Binary32 and Binary64 at both writer and checksum-valid
loader.  Both sides
also reject Binary32 formats 9--11, rather than publishing an unloadable legacy file.
The accepted timeline rule is shared by the live selector, writer, and loader.
Format 13 carries a private, payload-bound Binary64 origin authority issued by the
Binary64 owner.  The r60 Binary64 envelope and canonical temperature reconstruction
remain feasibility checks, not substitutes for origin.  Current writers and loaders
require that authority; an intact accepted Binary32 state retagged Binary64 fails at
the live owner, and a last-step-only mutation fails at owner, writer, and loader.
The Binary64 authority digest is not a parallel, hand-maintained state summary: it
runs the canonical format-13 checkpoint writer in digest-only mode over the complete
serialized resume prefix.  It therefore binds case/build identity, dimensions and
cell width, every state and MAC value, every `SolverFrameValues` field and vector,
the accumulated centerline/plane fields, statistics duration, migration and active-
set history, and the accepted timeline with the exact same field order and encodings
used on disk.  Independent frame-statistics, accumulated-field, and duration mutants
must fail at both ordinary publication and checksum-valid reload.

The other missing behavioral boundary is also executable rather than inferred.
An exact diagnostic runs the real two-projection Metal owner, preserves all
payload bytes and restoration evidence, forces only the physical projection's
validation result false before token eligibility, and proves that no token can
be minted or published.  Malformed activation fails before Metal work.  The
normal twin remains byte-identical and exact exit `255` retains
`G=1.2031080315688669e-4`, drain `0.99562928290235475`, and resumed CFL step
`0.0018513042677754071 s` after canonical Binary32 cell-width promotion.

These authority changes move the source-bound r136 trace to
`727a9b39...00ff2ed` without changing its arithmetic census, `0xff` proof gap,
or exact exit `237`.  The burning capacity verdict is unchanged: r144 remains
exact exit `254`, field deviation `2.5081625764804549e-3`, required drain
`3.3442167670577247`, and delivered drain `0.97489008508207653`.  The golden
checkpoint remains `1b944176...4947`; long shadow, golden `B_fp32`, temporal
refinement, readmission, source maps, and first light remain blocked.

### 7.55i Manifold stage-budget protocol (r149, pre-evidence)

r145 falsified the timestep-proportional generation premise at the burning
checkpoint: the represented step became `3.5423604574` times smaller while
the realized field generation retained `0.9902699302` of its prior value.
The manifold timestep limit remains part of the production selector, but is
not treated as the capacity remedy for this regime.

Before another architectural ruling, r149 freezes two measurements.  First,
the accepted-step field-generation map and its nonnegative maximum reduction
move onto Metal.  Only the scalar reduction may return before the existing
terminal publication, and the predictor path must retain zero interstage
full-grid transfers.  An independent Binary64 host reconstruction remains a
pinned diagnostic oracle, not the production implementation.  Second, the
golden slice is replayed at CFL, CFL/2, and CFL/4.  For each request the exact
stage order is remap/advection, physical projection, restoration projection;
stage generation is `max_cell |V_after - V_before|`, and adjacent scaling is
reported as `log2(G(dt)/G(dt/2))`.

The response is pre-decided.  A dominant, timestep-invariant remap term
requires manifold-consistent reconstruction: reconstruct temperature and
composition and rebuild sensible energy, or equivalently project reconstructed
face states onto the EOS manifold, without relaxing shared alpha or
monotonicity.  Dominant restoration self-generation requires the already-noted
two-pass anomaly-aware target.  If both remedies still leave the realized field
above `7.5e-4`, the campaign stops for a contract-level production-ceiling
ruling derived from thermodynamic validity and oracle comparison.  No observed
value may alter this decision tree.  The sealed protocol is
`rendered/fire_production_calibration/r149_manifold_stage_budget_protocol/`
`manifold_stage_budget_protocol.v1`.

### 7.55j Burning stage budget and reconstruction falsification (r150)

The frozen campaign identifies the burning capacity term without ambiguity.
At CFL, CFL/2, and CFL/4 the remap contribution is respectively
`2.5327801704406738e-3`, `2.5155544281005859e-3`, and
`2.5067925453186035e-3`; its adjacent scaling exponents are only
`0.009845460522765278` and `0.005033797039351196`.  The physical and
restoration projections contribute exact zero to the scalar EOS-volume map in
all three runs because neither projection changes the conservative scalar
state.  Remap therefore owns 100% of the directly observed generation and the
term is per-step-finite, not proportional to timestep.
The same device evaluator gives cold-state `G=1.2048172357026488e-4`, versus
the earlier Binary64-host observation `1.2031080315688669e-4`; the device-domain
burning/cold contrast is `21.0221110x`.

The pre-registered remap branch was executed next through a retained,
diagnostic-only exact-`245` comparator.  The real strict-binary32 remap
transported a shared-alpha ten-tuple containing `rho*T` beside the original
nine conservative components, then rebuilt sensible energy from reconstructed
temperature and positive-part composition under the certified endpoint rules.
The diagnostic deliberately used Binary64 thermochemistry before the final
binary32 energy publication, a favorable reconstruction rather than an fp32
rounding penalty.
It preserved shared-alpha conservation coupling and monotonicity.  Its three
generation maxima were `2.5328069638265172e-3`,
`2.5155729299433105e-3`, and `2.5068855498342479e-3`: the independent host-audit
values, slightly above rather than bit-identical to the baseline device values.
The trial therefore provided no reduction and was kept out of production.  The
restoration-anomaly branch was not activated: its precondition is restoration-dominant
self-generation, whereas both projection-stage scalar contributions are exact
zero.  The frozen rule therefore reaches its contract-level production-ceiling
stop.  The current `7.5e-4` allowance is exceeded by `3.3770759518x` at the CFL
burning state, and long shadow, `B_fp32`, guard supersession, temporal
refinement, readmission, source maps, and first light remain unrun pending that
ruling.

The production measurement itself is now resident.  A private two-float map per
cell holds beginning/terminal deviation; an exact nonnegative binary32 atomic
max publishes only the two scalars through the already-required terminal read.
The independent Binary64 reconstruction remains a diagnostic audit.  There is
one scalar read and zero new full-grid device-to-host transfers.  The exact-
`254` timing path predeclares one warmup and five samples; its p95 is the
maximum order statistic and fails unless device p95 is at most `75 ms` and wall
p95 improves on the prior `600.471584 ms`.  The calibrating observation's warm
device p95 is `65.7983333 ms`, meeting the requested approximately-70-ms device path,
but warm wall p95 is still `196.349959 ms`; the earlier `600.471584 ms` wall
observation is reduced by `67.3007%`, not restored to 70 ms.  At the represented
limited step this changes the tier-10 x 25 s wall projection from `262.3922 h`
to `85.8004 h` (device-only projection `28.7524 h`).  The remaining wall/device
gap is retained as an explicit performance finding, not hidden in the capacity
verdict.  Exact evidence is
`rendered/fire_production_calibration/r150_manifold_stage_budget/`
`manifold_stage_budget_evidence.v1`; the golden checkpoint is unchanged.
The source-bound r136 digest moves to `a0e42abe...b85989e` solely because the
resident result schema is mirrored; its arithmetic census, `0xff` refusal, and
exact exit `237` are unchanged.

### 7.55k Accepted-map reconstruction and capacity stop (r151)

Review invalidated only r150's reconstruction proxy, not its stage budget.
Transporting `rho_total*T` does not preserve the fixed-pressure manifold when
the gas molecular weight changes across a front: `V=(R/P)*(q/rho_total)*n`
still depends on `n/rho_total`.  The retained exact-`245` diagnostic is
therefore replaced by a direct projection through the authoritative accepted
map.  It runs the actual nine-component strict-binary32 shared-alpha remap,
keeps components 0 through 7 unchanged, and searches the sensible-energy
lattice until it finds the adjacent binary32 values bracketing `V=1` under
`AcceptedConservativeVolumeRatio`.  Choosing the closer endpoint is the most
favorable energy-only manifold reconstruction compatible with the existing
r60 endpoint semantics; the trial remains diagnostic-only.

At CFL, CFL/2, and CFL/4 the best reconstructed maxima are
`1.302156695613399e-3`, `1.2866699325340125e-3`, and
`1.2788512525973017e-3`.  These are reductions of 48.59%, 48.85%, and 48.99%
from the independent baseline, but the CFL result is still
`1.7362089275 * 7.5e-4`.  Cell 3478 is limiting.  It begins at deviation
`3.0191404931656507e-12`; its nearest accepted ratios on the two sides are
`0.99869784330740574` and `1.0014969001088954`, so neither representable state
meets the plateau allowance.  Projection-stage scalar generation remains
exactly zero, hence the anomaly-aware restoration branch is not applicable.
The pre-registered r149 decision rule therefore stops for a contract-level
production-ceiling ruling.  No reconstruction, ceiling change, long shadow,
`B_fp32`, guard supersession, temporal refinement, readmission, source map, or
preview is admitted.

The timing gate now uses the actual completed-call budget: after one warmup,
the maximum of five samples must satisfy device `<=75 ms` and wall `<=200 ms`.
The calibrating observation is `66.366791725158691 ms` device and
`199.188458 ms` wall.  This closes the prior `600.471584 ms` regression and
meets the 200-ms production budget, but it does not meet the requested
approximately-70-ms wall target.  At the represented limited timestep, the
tier-10 x 25 s projection is `87.0407538 h` wall (`29.0007546 h` device-only),
down from `262.3922081 h`.  Exact evidence is
`rendered/fire_production_calibration/r151_accepted_map_reconstruction_stop/`
`accepted_map_reconstruction_evidence.v1`; the golden checkpoint is unchanged.

### 7.55l Conservative face reconstruction (r152)

The r151 fixed-composition energy replacement is not an admissible production
remedy: it changes a cell-average primary ledger after remap.  It also cannot
exclude an EOS-projected face reconstruction whose shared alpha and output
composition differ.  r152 retains that observation only as a diagnostic and
executes the legal topology instead.

The reconstruction tuple contains `rhoZ`, all seven constituent densities, and
`n*T=P/R`.  The existing strict-binary32 remap applies one continuous shared
alpha to all nine quantities.  For every swept face slug, the trial computes
`n_flux=sum_(CH4..CO) F_i/M_i`, `T_face=F_(nT)/n_flux`, and
`F_E=sum_i F_i*h_i(T_face)`.  The cell-average sensible-energy ledger is then
updated only as `E_new=E_old-(F_E,right-F_E,left)/dx`.  This is a conservative
face-flux reconstruction through the existing x/2,y/2,z,y/2,x/2 palindrome:
there is no post-remap state repair, while the common limiter and monotonicity
discipline remain intact.  Because energy is replaced by `n*T` in the limited
tuple, both alpha and composition may change relative to production; the trial
does not assume the old off-manifold topology.

Fresh review found that this first trial evolved the nominally algebraic `n*T`
field after the first sweep.  It therefore established fixed pressure only for
the first x half-pass, and its unclamped-topology/capacity conclusion is
retired by r153.  The resulting diagnostic G maxima were `2.5328069638265172e-3`,
`2.5155729299433105e-3`, and `2.5068855498342479e-3` at CFL, CFL/2, and CFL/4.
Thus the historical diagnostic provided no reduction and remained
`3.3770759518x` above the `7.5e-4` allowance at CFL.  Cell 3227 moved from
`-1.1871614802316799e-12` to `-2.5328069650136786e-3`.  Its claimed legal-
topology capacity stop is superseded by r153; only the raw diagnostic values
and production-unchanged status remain historical evidence.

r151's completed-call timing correction remains binding: device `<=75 ms`,
wall `<=200 ms`, with observed `66.366791725158691/199.188458 ms` and an
`87.0407538 h` tier-10 x 25 s wall projection.  Exact evidence is
`rendered/fire_production_calibration/r152_conservative_face_reconstruction_stop/`
`conservative_face_reconstruction_evidence.v1`; this is historical evidence,
not the final capacity conclusion.  The golden checkpoint is unchanged.

### 7.55m Fixed-pressure reconstruction/domain stop (r153)

r153 resets the algebraic auxiliary to `n*T=P/R` before every one of the five
directional sweeps and never scatters its conservative update.  The independent
face integral is `(P/R)*dt_sub*u_face`; the same shared alpha still limits
`rhoZ`, the seven constituents, and the auxiliary.  Reconstruction-specific
field hashes, production-field hashes, shared-alpha/energy-flux trace hashes,
and a global boundary-flux energy ledger make bypassing the trial observable.

The corrected trial exposes a thermochemistry-domain obstruction rather than
an arithmetic tolerance issue.  Conservative compression of the transported
composition requires fixed-pressure face temperatures below the certified
`300 K` lower endpoint.  At CFL/CFL/2/CFL/4, respectively,
`2,307,844`, `2,326,957`, and `2,356,525` face evaluations require endpoint
projection; the largest excursions are `0.3536066003 K`, `0.1761488694 K`,
and `0.08734068083 K`.  Their approximately linear dt scaling identifies a
real conservative-compression/domain conflict.  Shared-alpha relaxation,
monotonicity relaxation, and uncertified thermochemistry extrapolation remain
rejected.

For diagnosis only, r153 applies the favorable endpoint projection and carries
its energy solely through conservative face fluxes.  The reconstructed fields
are byte-distinct from production (maximum energy differences `308.03125`,
`153.9375`, and `76.9375 J/m3`), and the largest relative global energy-ledger
residual is `1.4020231210267571e-10`.  Nevertheless its field-max G is exactly
`2.5328069638265172e-3`, `2.5155729299433105e-3`, and
`2.5068855498342479e-3`: no reduction, with CFL still `3.3770759518x` the
`7.5e-4` allowance.  This endpoint projection is not admitted as production
arithmetic.  Fresh review correctly observes that an out-of-domain high-order
face alone is a limiter obligation: optimality must yield before declaring the
shared-alpha interval empty.  r153's capacity conclusion is therefore retired
by r154, while its field/trace/ledger observations remain historical evidence.

The corrected residency timing remains separately pinned at `66.4652500 ms`
device and `194.687208 ms` completed wall, for a tier-10 x 25 s projection of
`29.0438 h` device and `85.0738 h` wall.  This meets the 75/200-ms budgets but
not the requested approximately-70-ms wall target.  Exact r153 evidence is in
`rendered/fire_production_calibration/r153_fixed_pressure_reconstruction_stop/`
`fixed_pressure_reconstruction_evidence.v1`; the golden checkpoint is
unchanged.

### 7.55n Composed low-order manifold capacity (r154)

r154 was a useful but incomplete attempt to apply the r59/r123 invariant
triage.  Its high-order face cap retained one common alpha, monotonicity, and
the conservative energy ledger, but its capacity inference is retired by
r155 for two independent reasons: the alleged alpha-zero donor inherited
earlier greedy high-order passes, and its endpoint width used
`kappa32*epsilon32*T` rather than r60's composition-dependent
`kappa32*epsilon32*AcceptedStateEnergyScale/Cp_lower`.  The recorded high-order
field, trace, ledger, and G values remain historical diagnostics only.  Exact
historical evidence is in
`rendered/fire_production_calibration/r154_low_order_manifold_capacity_stop/`
`low_order_manifold_capacity_evidence.v1`; the golden checkpoint is unchanged.

### 7.55o Globally low-order manifold reconstruction (r155)

r155 is retired by r156.  It correctly removed r154's greedy predecessor, but
used `tolerance/Cp_lower` as a sufficient inside width.  Since `Cp_lower`
proves an energy-difference lower bound, that quotient can prove exclusion
when exceeded but cannot prove inclusion when satisfied.  The recorded
all-low-order fields, ledgers, and G values remain diagnostics only.  Exact
historical evidence is in
`rendered/fire_production_calibration/r155_all_low_order_reconstruction_stop/`
`all_low_order_reconstruction_evidence.v1`; the golden checkpoint is unchanged.

### 7.55p Coupled-alpha thermochemistry obstruction (r156)

r156 is retired by r157.  It derived a sufficient endpoint width with an independent `Cp_upper`: for
each certified thermochemistry segment it sums the absolute coefficient
polynomial over `[Tmin,Tmax]`, converts by `R/M`, and then sums with absolute
constituent densities.  The maximum sufficient widths are only
`0.06678539755`, `0.06678528278`, and `0.06678524324 K`.  The all-alpha-zero
trajectory first exceeds them in pass 1 at CFL and CFL/2 and in pass 3 at
CFL/4.  The CFL witness is pass 1, line 29, donor 42, cell 3641:
`T=299.92254298172884 K`, while the local sufficient width is
`0.066785198576612095 K`.

The witness is not a greedy artifact.  The all-zero and greedy histories give
the exact same eight-component donor bytes (SHA-256
`2ac03ae9d4937f86861cc0a5e8c5620c6a166dd189c7a3f35d6d266fcc0346d3`).
More strongly, an independent affine interval over both adjacent pass-0 face
corrections allows each alpha to vary separately across its entire low-to-high
range, a superset of the shared-alpha topology.  Its smallest possible donor
molar density is `0.040632479709723168 kmol/m3`, still above the exact
thermochemistry maximum `P/(R*Tmin)=0.040621987915680717 kmol/m3`.  Therefore
no coupled/backtracked alpha admitted by the monotone pass-0 remap can make the
pass-1 face state thermochemically formable.  Optimality may yield, but
shared-alpha coupling, monotonicity, and the accepted thermochemistry domain
cannot.

For diagnosis only, endpoint projection continues the all-low-order ledger
outside that certified domain.  Its three SHA-bound fields retain a maximum
relative ledger residual `1.4158011510e-10`, but G remains exactly
`2.5328069638265172e-3`, `2.5155729299433105e-3`, and
`2.5068855498342479e-3`, or `3.3770759518x` the CFL allowance.  No such
projection is admitted to production.  Physical- and restoration-projection
scalar G remain exactly zero, so the anomaly-aware restoration branch is
inapplicable.  The ruled invariant-preserving reconstruction resists the
coupled route, and even the favorable uncertified relaxation does not hold the
plateau; the pre-registered decision therefore reaches the explicit
production ceiling/thermochemistry contract boundary.

Long shadow, `B_fp32`, guard supersession, temporal refinement, readmission,
source maps, and first light remain blocked pending that ruling.  Production
arithmetic is unchanged.  Fresh residency confirmation remains
`65.8577918/195.833166 ms` device/wall, or `28.7784/85.5747 h` for tier-10 x
25 s.  Exact evidence is in
`rendered/fire_production_calibration/r156_coupled_alpha_thermochemistry_stop/`
`coupled_alpha_thermochemistry_evidence.v1`; the golden checkpoint is unchanged.

### 7.55q r60-admissible reconstruction capacity stop (r157)

Retired by r158.  Fresh review found that r156 used a valid sufficient-inclusion width in the
wrong direction: `deltaT > tolerance/Cp_upper` is inconclusive, not exclusion.
r157 makes no endpoint-exclusion claim and no Cp quotient is load-bearing.

The retained exact-`245` topology is the ruled conservative reconstruction:
`n*T=P/R` is reset before each directional pass, one alpha is shared across
the transported tuple, and energy changes only through face-flux divergence.
Where exact fixed-pressure temperature is outside the authoritative
thermochemistry domain, the precision-boundary completion uses the certified
endpoint enthalpy; no thermochemistry extrapolation occurs.  This is the r59
admissible-interval principle applied at the r60 precision boundary, not a
post-step repair.  Every nonzero reconstructed face and every cell after each
of the five pass boundaries is checked with the single Binary32 r60 predicate:
`4,926,768` face validations and `4,881,360` pass-cell validations at each of
CFL, CFL/2, and CFL/4.  The conservative ledger, field hashes, and limiter-plus-
energy trace hashes remain exact-bound.

The all-low-order shadow retains one local API RED: an executed pass-1 donor at
`299.99999426911722 K` must return the authoritative out-of-domain error.  It
documents why the endpoint completion is needed, but is neither a global-alpha
exclusion nor part of the capacity proof.

The implemented r60-admissible reconstruction yields field-max G of
`2.5328069638265172e-3`, `2.5155729299433105e-3`, and
`2.5068855498342479e-3`, unchanged within the exact pins and still
`3.3770759518x` the `7.5e-4` CFL allowance.  Physical- and restoration-
projection G remain exactly zero, so the anomaly-aware target branch does not
apply.  The pre-registered automatic rule therefore reaches a contract-level
choice among a certified thermochemistry extension, a different reconstruction
scheme, an explicitly ruled non-exact policy, or a separately derived
production ceiling.  No ceiling is widened here.  Production arithmetic is
unchanged; long shadow, golden `B_fp32`, guard supersession, temporal
refinement, readmission, source maps, and first light remain blocked.

The fresh resident timing observation is `66.0780417 ms` device and
`197.288084 ms` completed wall, satisfying the `75/200 ms` gates and projecting
`28.8746/86.2105 h` for tier-10 x 25 s.  Exact evidence is
`rendered/fire_production_calibration/r157_r60_reconstruction_capacity_stop/`
`r60_reconstruction_capacity_evidence.v1`; the golden checkpoint is unchanged.

### 7.55r Producer-rounded reconstruction stop (r158)

Fresh review found that r157 still overclaimed a global-alpha obstruction and
validated pre-round double energy rather than the published Binary32 face
flux.  The alpha inference is retired completely.  The local below-domain API
RED remains diagnostic only and contributes nothing to the capacity verdict.

r158 moves the face predicate after `energyFlux=float(fluxEnergy)` and validates
`energyFlux/sweptLength`, the actual producer-rounded payload.  The exact-`245`
rerun admits all `4,926,768` nonzero faces and `4,881,360` pass-boundary cells
per timestep level under r60.  The same shared-alpha, monotone, conservative
reconstruction then measures G of `2.5328069638265172e-3`,
`2.5155729299433105e-3`, and `2.5068855498342479e-3` at CFL/CFL/2/CFL/4.
The CFL value remains `3.3770759518x` the `7.5e-4` allowance.  Field/trace
hashes and the `1.4020231210e-10` worst relative ledger residual are unchanged.

Physical- and restoration-projection G remain exactly zero, so the anomaly-
aware branch is not selected.  Under the pre-registered decision rule the
actual producer-rounded reconstruction remedy has failed the function-level
plateau gate.  This reaches the contract-level ruling boundary; it does not
derive or widen a production ceiling.  Production arithmetic remains
unchanged and the later sequence stays blocked.  Fresh exact-`254` timing is
`66.0780417 ms` device and `197.288084 ms` wall, projecting
`28.8746/86.2105 h` for tier-10 x 25 s.  Exact evidence is
`rendered/fire_production_calibration/r158_producer_rounded_reconstruction_stop/`
`producer_rounded_reconstruction_evidence.v1`; golden remains unchanged.

### 7.55s Timestep-velocity audit and thermo ceiling (r159)

The immutable golden checkpoint's accepted `5.6295254283638751e-5 s` step is
not a production CFL measurement.  Back-solving `0.5*dx/dt` gives
`217.37616398903009 m/s`, but the exact resident audit measures only
`7.4333348274230957 m/s` in the transport field, `7.371121883392334 m/s`
after the physical projection, and `7.371121883392334 m/s` after restoration.
The largest restoration correction is only `1.7818529158830643e-6 m/s`
(`1.0030986299568027e-10 m/s*s` at the preserved step), or
`2.4173429012179363e-7` of the physical maximum.  The old step instead belongs
to the preserved
binary64 oracle's retry history: its console log records repeated R0/R1
augmented-active-set cycles before r80/r81 applied r59's two-class acceptance
to that Zeno topology.  The checkpoint remains the immutable pre-fix root, but
its physical velocity bytes are healthy.

The production selector is therefore audited with the transport velocity, not
the retry-limited oracle step and not a finite-per-step projection correction.
This is the r70 death-spiral boundary: a `1/dt`-mode correction impulse may be
validated as part of the accepted step, but its velocity quotient does not
become the next step's advective speed.  The selector consumes
`7.4333348274230957 m/s`.  The complete selector also consumes the measured
golden-state reduced-gravity maximum `48.944695265891369 m/s^2` and active
diffusivity maximum `0.0030345390611787094 m^2/s`; their respective candidates
are `0.015812081290725255 s` and `0.024674291069445888 s`, so the selected
limit is `advective_CFL` at `1.6462660045688639e-3 s` (represented as
`1.6462659696117043e-3 s`).  The physical-projection maximum independently
gives `1.6601606404766957e-3 s`.  Each frozen slice restarts from the common
golden beginning, so no previous accepted production observation or growth
cap participates in this single-step selector audit.

The ceiling is then derived under the r72 function-owner rule.  The production
plateau gate protects the r60 thermo/temperature-inversion domain, whose
dimensionless pressure-deviation tolerance is `1e-3`.  Applying the pinned
headroom `h=2^-2` gives

`C_manifold=(1-h)*1e-3=7.5e-4`.

The former inheritance of `7.5e-4` from the physical projection is a design-
level derivation error even though the corrected function derivation yields
the same number.  The measured producer-rounded burning plateau is
`2.5328069638265172e-3`, or `3.3770759517686897x C_manifold`.  The ceiling is
not widened to admit it.  This is a thermo-domain finding, so the sequence
stops before long shadow, golden `B_fp32`, guard supersession, temporal
refinement, readmission, source maps, or first light.  Physical-fidelity
judgment remains a separate oracle-comparison contract.

The predictor path remains resident: one exact per-cell Binary32 manifold map,
one max reduction, one scalar readback, and no full-grid transfer.  The largest
host cost was instead serial construction of nine independent dual-remap
layouts.  Dispatching those packs through the topology-aware global thread
pool leaves arithmetic unchanged.  Under the required benchmark policy
`render_thread_reserve_count 0`, and at the actual selected step with its one
force substep, the controlled serial-pack p95 is
`73.046958423219621/202.659166 ms` device/wall and the parallel-pack p95 is
`73.379833251237869/154.74187499999999 ms`.  This is a `23.644275235989082%` wall
reduction.  At the physical CFL the 25.0324805-s tier-10 stepping work projects
to `0.3099403336721022 h` device and `0.6535957666507461 h` completed wall.
The remaining wall/device ratio is `2.1087793218362156`, with
`81.36204174876212 ms` of host orchestration still attributable to
preflight/layout, uploads, command submission/waits, and postprocessing.  The
`200 ms` gate is a ceiling, not a performance target; no claim that production
is faster than the oracle in validated practice is made while the thermo stop
is active.

The timed resident result must report the represented `1.6462659696117043e-3 s`
step and the one force substep must carry that same represented duration.
Every warmup and measured serial/parallel execution is accepted only when its
complete resident payload digest and every non-timing schedule/diagnostic field
are identical to the first serial baseline.
The legacy `force_all_threads_low_priority` mode routes these nine packs
serially, because that pool mode deliberately forbids nested `ParallelFor`
waiting under worker saturation.

The exact audit exits `247` only after binding all velocity values, owning
faces, projection counts/cycles, transfer counts, and golden SHA.  Malformed
activation exits `222` before Metal.  Exact evidence is
`rendered/fire_production_calibration/r159_timestep_velocity_ceiling_stop/`
`timestep_velocity_ceiling_evidence.v1`.

### 7.55t Production low-Mach gate and audited-step refusal (r160)

Fresh source tracing corrects r159's last inherited number.  The `1e-3`
comparison in `tools/fire_simulator_core.h` is the binary64 oracle's
accepted-state EOS validity detector; it is not a thermochemistry-table or
temperature-inversion domain boundary.  Production consumers were enumerated
at the code boundary: the Binary32 accepted-state predicate checks conservative
component and energy envelopes, temperature inversion brackets the certified
table range, molecular transport consumes the accepted temperature and
composition, conservative remap/source kernels consume the accepted ledger,
projection consumes density and divergence targets, the resident manifold map
reports `|V(Q)-1|`, and checkpoint publication persists that report.  None
consumes a pressure-deviation domain limit.  The r159 derivation
`(1-2^-2)*1e-3` is therefore retired as an r72 ownership error, not widened.

The production gates now follow their actual functions.  Restoration's
per-application mechanism must be residual-nonamplifying and records its
delivered drain for the retained manifold-timestep predictor.  Structural
plateau acceptance is regime evidence: an evolving long shadow must be
non-secular, and any secular window fails closed.  Independently, the
low-Mach formulation owns the exact dimensionless ceiling `2^-5=0.03125`:
`|V(Q)-1|` must remain much less than one, in the `O(M^2)` class.  The ceiling
is fixed before the rerun and is never adjusted to observed data.  Fidelity
remains the oracle/additive-contract decision; the existing manifold floor is
pre-registered as a readmission risk (about four times the physical dynamic-
pressure scale), with the two-pass anomaly closure retained only as a
candidate if the slice evidence attributes a failure to that floor.

At the old retry-limited step, the calibrating field maximum
`2.5328069638265172e-3` is `12.3380898925x` below `2^-5`.  The required next
gate, however, is the audited physical CFL step
`1.6462659696117043e-3 s`.  Its first evolving Metal application measures
`G=0.085895776748657227` and the same realized field maximum, with delivered
restoration drain `0.66657990322152694`.  This is
`2.7486648559570312x` above the pre-registered low-Mach ceiling and
`33.9132740771x` the old-step observation.  The result is diagnostically
published only long enough to bind those numbers: no accepted-state token is
minted, no next state is applied, and exact exit `252` records the refusal.
Consequently the 104-step non-secular window cannot begin.

This is the formulation-validity finding the ruling required us to stop on,
not grounds to widen the ceiling.  Golden `B_fp32`, guard supersession,
temporal refinement, eight-slice readmission, source maps, and first light
remain blocked.  r159's measured `81.36204174876212 ms` host residual remains
named device-bound performance work, outside this correctness boundary.  The
golden checkpoint stays byte-identical.  Durable evidence is in
`rendered/fire_production_calibration/r160_low_mach_audited_step_refusal/`
`low_mach_audited_step_refusal.v1`.

### 7.55u Resident advective-anomaly corrector (r161)

The r160 refusal is the disabled-control branch for the final pre-registered
remedy.  Burning-state generation follows two observable regimes of
`G=G_floor+k*dt`: r158's short-step sweep gives
`G_floor=0.0024982685328926446` and `k=0.6137015145338649 s^-1`, while the
audited physical-CFL branch gives `k=50.65858722417441 s^-1`.  The apparent
timestep-invariant floor and the large-step dose are therefore compatible;
neither measurement is discarded.

The production step now measures the predictor remap's per-cell volume-ratio
anomaly on-device and adds

`(V_predictor - V_beginning) / dt`

to the existing restoration target.  The restoration projection retains its
Private pressure, density, momentum, and velocity buffers.  A second five-pass
cell palindrome then remaps the original beginning scalar ledger with that
corrected velocity, reapplies the same explicit source operand, and only then
allows terminal staging.  No full grid crosses the device boundary between
the predictor, either projection, and the corrector.  A zero anomaly makes the
target fold exactly zero and skips the second palindrome; missing, half, and
sign-reversed ramp mutants are rejected by the independent scalar derivation.
The zero case is also run end-to-end on Metal: a device-reconstructed uniform
beginning is evaluated once with closure disabled and once with closure active;
the complete accepted payload digests are byte-identical, while the active
diagnostics record one predictor pass and five—not ten—cell submaps.

The audited-CFL correction reduces G from `0.085895776748657227` to
`0.066569089889526367`, but this is still above `2^-5`.  One warmup plus five
byte-stable samples measure `92.4832500750199/179.9965 ms` device/wall p95,
or `0.39012213338718343/0.75927931301360085 h` for tier-10 x 25 s.  The
audited-CFL control is therefore within the two-hour wall rule even though its
field is inadmissible.  With headroom `h=2^-2`, the retained predictor uses the allowance
`(1-h)*2^-5=0.0234375` and derives the represented step
`0.00057953997747972608 s`.  The external physical Heun target must be
re-derived at that represented step.  The binary64 oracle target generator's
frozen R0 open conservative Picard solve then fails to
converge: first residual `7.41824`, last/minimum `1.44776`, target `0.561256`,
mass `1.44776`, coefficient `0.017278`, active set `1`, and tolerance
`0.000479545`.  The earlier limited G/timing observation, which had reused the
audited-CFL target, is rejected and carries no contract weight.

Exact exit 219 therefore stops at target-schedule instantiation before a
limited production request exists.  It is not evidence of a production remap
or reconstruction-class failure.  No limited G, plateau, timing, or tier-10
wall projection is reported, so the approximately two-hour rule is not
evaluated.  The long shadow and all later arithmetic/readmission/source-map/
preview milestones remain blocked pending a target-generation/fixed-point
topology ruling; neither ceiling nor timing budget is widened.

### 7.55v Equal-time reference composition and limited-step stop (r162)

The owner ruling retires equal-step comparison at the golden-composition
boundary.  A coupled Picard map owns a timestep-dependent contraction radius;
requiring the production step and the binary64 oracle to share one `dt`
therefore assumed a common step-operator domain that neither scheme promised.
The oracle is the reference flow.  It may reach the production endpoint with
its own converged substeps, while production retains one limiter-selected step.

The pre-measurement contraction sweep tests the represented limiter duration
and its exact binary halves.  `dt`, `dt/2`, and `dt/4` fail in proved
two-class active-set cycles.  The failing stage moves from R0 to R1 and back to
R0; the relevant terminal residuals are `1.4477584866157618`,
`0.72567590358972345`, and `0.56139851539581598 s^-1`.  This is not smooth
non-contraction.  At `dt/8=7.244249718496576e-5 s`, the existing r59/r80
two-class selector resolves the one remaining R1 switch and all R0/R1/R2
stages converge, ending at `2.5305532581487711e-4`,
`4.6488187337700992e-4`, and `6.0149754458578642e-5 s^-1`.  The complete
diagnostic trace is `900a7acc...a051c`.

The equal-time reference consequently executes eight binary64 Heun substeps
from the exact golden beginning.  Their durations sum bit-exactly to the
production duration `0.00057953997747972608 s`; the terminal physical target
is tagged with that same endpoint and is the only target supplied to the
single production step.  Parallel and serial reference schedules have the
same digest `e4472da7...c97e0`.  A mismatched accumulated endpoint and a
terminal target tagged with a stale substep time are independent REDs.  The
producer-rounded terminal target is separately bound by `d198eaaa...351ec`;
substituting the actual penultimate target while retaining the current endpoint
is RED.  The
reference costs minutes per slice (eight substeps here, close to the
pre-registered estimate of ten); that is an acceptable fixture cost and is
not a production optimization target.

Under this amendment, the per-quantity additive contract is evaluated at one
physical time as scheme distance plus oracle temporal distance plus production
temporal distance plus subdominance.  The two temporal distances remain
Richardson-derived under their own formal orders.  Temporal refinement is now
part of the equal-time protocol rather than a later correction to an
equal-step comparison.  No temporal number is admitted by this entry because
the limited production prerequisite fails first.

The equal-time target removes r161's protocol blocker and reaches Metal.  The
limited step measures predictor `G=0.020501971244812012` and corrected
`G=field_max=0.024358630180358887`.  The hard low-Mach ceiling remains
`2^-5=0.03125`, but its 25%-headroom allowance is `0.0234375`; the realized
field is above that allowance, so no accepted-state token is minted.  The
backstop derives the next manifold step as `0.00055743221913055079 s`.
One warmup plus five stable samples measure `89.583708089776337 ms` device and
`175.805542 ms` wall p95.  At the current (larger) limited step those
already project to `1.073453270061155/2.1066222640131049 h` for tier-10 x
25 s.  A smaller backstop step can only increase the fixed-work step count, so
the pre-registered approximately two-hour wall rule cannot be met by further
timestep reduction.

Exact exit `213` is therefore the requested remap-scheme finding: equal-time
composition succeeds, but closure plus its automatic limiter cannot
simultaneously hold the headroom allowance and the wall budget.  No ceiling or
budget is widened.  The 104-step shadow, golden `B_fp32`, guard supersession,
temporal evidence, readmission, source maps, and first light remain blocked
pending a reconstruction-class ruling.  Durable evidence is
`rendered/fire_production_calibration/r162_equal_time_composition_stop/`
`equal_time_composition_evidence.v1`; the golden checkpoint is unchanged.

### 7.55w Predictive first-step initialization (r163 refusal)

The owner rejects a remap-scheme change and classifies the r162 state as the
designed limiter operating class.  The oracle contraction boundary is now a
permanent protocol property rather than a production defect: at steep states,
the tested `dt`, `dt/2`, and `dt/4` values exhibit proved two-class active-set
cycling.  `1.4488499436993152e-4 s` is the smallest tested cycling value and
`7.244249718496576e-5 s` the largest tested convergent value; the experiment
does not claim a continuous monotone boundary between them.  The converged
`dt/8` schedule is sealed independently for every slice and composes the
binary64 flow to production's physical endpoint.

The SHA-bound golden-composition fixture owns the sealed calibration tuple
`(dt_audit,G_audit)` and derives

`dt0 = dt_audit * ((1-2^-2)*2^-5) / G_audit`,

publishes the represented float timestep in the production request, and emits
the tuple plus derived double in its evidence record.  The generic resident
API does not accept or authenticate a caller-authored calibration tuple; this
avoids creating another mutable bearer-authority surface.  Later accepted
steps continue to use the drain-aware manifold limiter, and no checkpoint-
authority rule is weakened.

For the frozen observation `dt_audit=0.00057953997747972608 s` and
`G_audit=0.024358630180358887`, the formula gives
`0.0005576244690940563 s`, represented as
`0.00055762444389984012 s`.  Its equal-time reference uses eight substeps of
`6.9703055487480015e-5 s` (schedule `1c7944dd...531d`, terminal target
`52234712...f833`).  The production corrector measures
`G=field_max=0.023458600044250488`, which is
`0.000021100044250488281` above the `0.0234375` headroom allowance.  The hard
`0.03125` ceiling still passes, but headroom is load-bearing: the accepted
token is withheld and the automatic backstop reports
`0.00055692791475544124 s`.

Exact exit 215 records that the mandated proportional initialization is close
but not sufficient to make the first accepted step legal.  The old CFL path
still reproduces exact 252.  Timing (`89.7549167/178.69375 ms`, projecting
`1.1177739/2.2253845 h`) is diagnostic only because the step is rejected.
The owner ordered host-residual work after first-step legality, so that
campaign does not start in this entry.  Diagnostic profiling of rejected,
byte-stable replays would be technically valid; it is the requested ordering,
not accepted-state authority, that defers it.  The 104-step shadow and
subsequent accepted-state arithmetic/readmission milestones remain blocked.
This is not grounds to widen the allowance or quietly replace the prescribed
formula with the observed backstop.  Evidence is
`rendered/fire_production_calibration/r163_predictive_initial_step_refusal/`
`predictive_initial_step_evidence.v1`.

### 7.55x Drain-aware plateau retry and accepted host profile (r164)

The ordinary plateau-refusal path now consumes the already-derived manifold
backstop.  `AttemptFireProductionResidentStepMetal` returns false with a
tokenless, diagnostic result containing the derived next step; the public
`Advance` wrapper returns false with its result reset, so a caller cannot apply
the refused payload accidentally.  The predictive initializer remains
candidate zero; the owning retry loop may retry only at a finite, positive,
strictly smaller drain-aware suggestion and only inside the same shared
`FireStepRejectionRetryCap=20` used by the capstone rejection loop.
`ClassifyFireProductionResidentStepAttempt` is the production-owned
accepted/retry/rejected disposition: it advances every refused candidate index,
rejects the cap boundary, and accepts an authorized plateau pass at any
candidate. Candidate range is checked before acceptance or increment, and an
accepted disposition revalidates the producer token against the current
diagnostics and complete payload, so unsigned wrap and post-attempt mutation
both reject.  The payload RED mutates and restores the authentic token-bearing
result in place, and a no-Metal candidate-1 refusal RED traverses the same
classify/branch/recursive-continuation seam as the real owner and requires
candidate 2, so later-candidate continuation is live.
The equal-time protocol owner remains responsible for rebuilding the
independently sealed target before invoking the next attempt.
This is more informed than blind halving and does not alter the hard ceiling or
headroom allowance.  The r162 wording that step 1 is “already legal” meant
that initialization removes a separate CFL-transient class, not that a
fail-closed refusal is forbidden.  r163's residual is only
`0.09002685546875%` of the allowance, so the ruled one-retry mechanism is used
without a floor-aware second-order predictor.

The exact path first reproduces candidate zero at
`0.00055762444389984012 s`: terminal target `52234712...f833`, field
`0.023458600044250488`, no token, suggestion
`0.00055692791475544124 s`.  Candidate one represents that suggestion as
`0.00055692793102934957 s` and owns a fresh eight-substep equal-time schedule
`0db10079...b08b` plus terminal target `cf67f48c...8bae`.  It measures predictor
`G=0.019709885120391846`, corrected `G=field=0.023429989814758301`, delivered
drain `0.99964541417058472`, and a `7.510185241699219e-6` margin below the
`0.0234375` allowance.  Both projections validate, the hard `0.03125` ceiling
passes, and the accepted token is minted.  Exact exit `206` binds the refusal,
retry, and acceptance; exact `252` remains the old-CFL RED.

Host profiling is performed on the accepted candidate under
`render_thread_reserve_count 0`.  The earlier `323 ms` observation was an
uncalibrated pilot and is not used as the speedup denominator.  The controlled
same-binary serial baseline samples are
`229.521625,229.379792,228.151208,228.878583,226.056917 ms`; production-parallel
samples are `149.6125,150.65625,151.756666,151.894375,151.023333 ms`.
The optimization preserves field-tagged and length-delimited complete-payload
authority while computing independent field digests in parallel, overlaps
dual-static/force/cell preparation, and overlaps the independent corrector and
restoration-publication branches.  Legacy low-priority mode takes the serial
route.  Kernel arithmetic is unchanged, while queue scheduling intentionally
changes; exact-247 complete-payload equivalence is the invariant.

After overlap, the device metric is the earliest-GPU-start to latest-GPU-end
queue-DAG span, not the invalid sum of overlapping command durations.  Final
accepted p95 is `117.86270828451961 ms` device span and `151.894375 ms` wall;
the paired residual p95 is `34.631833361461759 ms`.  For 25 s at the operating
step, the projections are `1.4696534042399729 h` device-span and
`1.89400098260743 h` wall.  The controlled wall reduction is
`33.821322936346412%`.  The raw replay lines are retained and SHA-bound beside
the certificate. This clears the prior two-hour stop class but does not
meet the approximately `1.3 h` target, so the residual remains named work
toward device-bound execution.  Source maps are expected to increase device
time.  No performance number changes the physics acceptance.  Durable evidence is
`rendered/fire_production_calibration/r164_drain_aware_retry_acceptance/`
`drain_aware_retry_acceptance.v1`; the golden checkpoint is unchanged.

### 7.55y Validation operating-point audit and long-shadow refusal (r165)

The owner accepted r164's one-slice cost for starting validation, with the
approximately one-hour device target deferred until thermo/source maps.  r165
then began the ordered 104-step shadow and stopped after three accepted steps.
Those raw observations remain useful: the third candidate began at
`5.756302853114903e-4 s`, reached field `0.062683582305908203`, and required
six tokenless refusals before an attempt at `1.7358525656163692e-4 s` reached
field `0.023434281349182129`.  Both projections validated on the three
published diagnostic states.

Fresh contract review invalidated the attempted operating-point conclusion.
The value called the pre-corrector advective dose is computed cellwise as
`abs(terminalDeviation-beginningDeviation)`.  It is Eulerian: translating an
already nonuniform bounded plateau produces a nonzero value even without new
donor-mixing anomaly.  It therefore cannot mint the next accepted timestep
observation until a material/transported baseline is defined.  The owner now
permits that Eulerian observable to mint authority only when the beginning
deviation field is bit-exact zero or the complete transport velocity is
bit-exact at rest, where translation contributes identically zero.  A moving
nonuniform plateau is tokenless even when both projections and the field
ceiling pass; ordinary `Advance` rejects it atomically.  The
three-step sequence is calibrating evidence, not a certified trajectory.

The earlier `4.7152/6.0767 h` extrapolation is also withdrawn.  It multiplied
only the final accepted step by r164's single-attempt p95 and omitted the six
refused attempts.  The measured final attempt gives the purely conditional
linear extrapolation `4.131755301914921/7.162223294085453 h` device/wall.  It
is not a lower bound: future timesteps and attempt costs vary, total retry cost
was not recorded, and per-attempt equal-time schedule and terminal-target
digests were absent from that raw transcript.  No tier-10 cost is claimed.

The independent physical-projection ownership defect is closed and retained.
A one-step RED begins at 12 open-boundary V-cycles, refuses at 12 and 13,
derives 13 and then 14 from the measured contraction, and validates at 14
before classifying the preserved tokenless manifold refusal.  Exact exit 209
binds both transitions and the unchanged golden SHA.  The fp64 mirror and
roundoff trace adapters also propagate a request-owned non-default 19-cycle
physical schedule instead of silently falling back to 17.

The 104-step non-secular shadow, B_fp32, guard supersession, readmission,
source maps, and first light remain unexecuted.  Their blocker is now stated
narrowly: no material advective-generation observable has yet been derived for
the evolving plateau, and the equal-time/cost record for the invalidated run
was incomplete.

### 7.55z Motion-invariant distribution shadow (r166)

The shadow does not construct a transported copy of the EOS-deviation field.
That proposal would introduce a new advected quantity, limiter path, and
working-set obligation merely to distinguish translation from growth.  Spatial
quantiles already have that invariance: advection of a fixed discrete pattern
permutes its distribution while leaving maximum, p95, and p50 unchanged.

The terminal manifold kernel therefore emits three device-reduced scalars per
accepted step: `max(abs(V-1))`, nearest-rank p95, and lower-median p50.  The
maximum is the existing atomic maximum.  The quantiles are exact over the
nonnegative Binary32 field: a 65,536-bin high-word histogram selects each
rank's high word, a second pair of low-word histograms resolves the remaining
16 bits, and a final single-thread device reduction publishes the exact float
bits.  No full-grid readback is introduced.  The working-set certificate owns
the three histograms and control words.

Accepted-state authority and timestep-generation authority are distinct.  A
moving nonuniform field may mint the sealed accepted-state token after the
allowance, hard ceiling, and both projections validate.  Its Eulerian
terminal-minus-beginning `G` is diagnostic only and the accepted observation
publishes `G=0`, so it cannot activate the manifold timestep term.  Exact-zero
beginning and exact-stationary transport fixtures retain the original G
authority.  Ordinary plateau refusal and its drain-aware retry remain the
automatic backstop.

For the 104-step campaign, the r160 classifier runs separately on max, p95,
and p50; all three must be non-secular and remain below `2^-5`.  A synthetic
p95 ramp under a flat maximum is the binding RED, while a translated fixed
distribution is the binding GREEN.  Per-step allowance/ceiling enforcement
and both projection validations are unchanged.  Performance remains outside
this rung: the legacy 45 ms remap check is deferred to the single complete-step
campaign after thermo/source maps.

The first campaign did not certify or reject non-secularity.  It accepted
seven production steps, then the equal-time Binary64 reference failed to
instantiate the slice-7 terminal target.  Refining the schedule from 8 to 16,
32, and 64 substeps did not close R1; the 64-substep attempt failed at substep
8 with residual 0.0191989 versus tolerance 0.000479545.  Because production
must consume the terminal target at the identical end time, no production
attempt for slice 7 is admissible and the 104-value trajectories are
incomplete.  r166 is therefore an exact reference-schedule capacity refusal,
not a plateau classification.  B_fp32 and all downstream milestones remain
unrun.

### 7.56 Tier-6 temporal-refinement protocol (r139, pre-evidence)

Temporal refinement uses the r112 smooth tier-6 beginning and the same fixed
physical filter/metrics.  To eliminate horizon drift between fp32-authored
requests, the baseline step is the exactly promoted binary32 value
`0x1.e54eeep-10 s`; the next levels are exact binary halves
`0x1.e54eeep-11` and `0x1.e54eeep-12`.  Step counts `8,16,32` therefore all
land at the exact represented horizon `0x1.e54eeep-7 s`
(`0.01481043454259634 s`).

Production temporal distance is measured with the strict binary64 same-scheme
mirror at formal order one; `B_fp32` remains blocked by r142.  Oracle
temporal distance uses the certified Heun advance at formal order two.  A
capability-isolated no-Metal process writes the three divergence-target
schedules before either solver-output comparison; the read-only evaluator
hash-checks and replays them.  For every scalar, velocity, and inventory
quantity, `D_coarse>D_fine>0` is mandatory,
`p=min(p_formal,log2(D_coarse/D_fine))`, and
`E_dt=D_coarse/(1-2^-p)`.  Exact zero requires a separately proved algebraic
identity.  No post-observation fallback constant exists.

The complete pre-evidence manifest is
`rendered/fire_production_calibration/r139_temporal_protocol/temporal_protocol.v1`.
It records no temporal result and dispatches no Metal.  Golden SHA remains
`1b944176...4947`.

### 7.56a Closure-convergence architecture finding (r167)

r166 completes the empirical generation model as
`G = dose(delta_t) + feedback(deviation)`.  Across its six evolved pairs, a
least-squares regression of the next Eulerian `G` on the preceding field
maximum gives slope `0.7925875134206254`, intercept
`0.0225796196872328`, and Pearson correlation `0.7129565317645592`.
This confirms a substantial near-unit feedback branch without overstating it
as an exact unit-gain law.  The same interpretation explains the seven-step
hover while represented delta-t collapsed by `148.6430318897442x`, and is
consistent with r145's measured retained fraction `0.9902699302058174`.
The operating-point ruling was therefore wrong: seeding the trajectory at the
plateau allowance placed it directly in the feedback basin, while the
subdominance scale should have defined acceptance.

The deciding experiment fixes the immutable golden beginning and the audited
CFL step `0.0016462659696117043 s`, then executes one through eight closure
passes.  Each pass count is an exact pre-Metal identity and the topology is
bound as five cell submaps, one source commit, and one scalar measurement per
pass.  The first pass uses the ordinary restoration target; each later pass
remeasures the advective anomaly, rebuilds that target from the inherited
target, reruns restoration from the same physical projection, and remaps the
original beginning with the corrected velocity.  The fp64 mirror consumes the
same sealed divergence target and executes those same remap/projection
operators.  Its inherited golden payload retains Binary32 admissibility while
all subsequent arithmetic and reported volume ratios are binary64.
The r167 evidence directory binds both the raw transcript and a log-scale SVG
of the complete fp32/fp64 curve.

The subdominance-derived diagnostic tolerance is
`0.00065 * 2^-3 = 8.125e-5`.  The curve does not converge geometrically.  Its
best point is pass 2 (`G32=0.06656908988952637`,
`G64=0.06656946601494673`); pass 3 grows, and pass 8 reaches
`0.16477346420288086/0.16477412949642256`.  fp32 and fp64 agree within
`5.7e-6` relative at the sampled points, so this is not an fp32 floor.  It is
an oscillatory/divergent fixed-point topology more than 2,027x above the
derived tolerance.  No multi-pass acceptance rule is adopted, no hover state
is accepted under a replacement policy, and the limiter remains unchanged.
The next design action is the pre-registered hover-policy/reconstruction-class
decision using this curve, not another closure iteration or tolerance change.

One warmup plus five paired Metal measurements make the cost record explicit.
Pass 2 costs `122.15441651642323/141.788333 ms` device-span/wall p95;
pass 8 costs `417.5576251000166/438.83425 ms`.  Their conditional CFL
tier-10 x 25 s projections are `0.5152840274903517/0.5981057857990776 h`
and `1.761383508732288/1.8511347046572506 h`, respectively.  These are
diagnostic costs, not accepted-production projections.  The long shadow and
every later milestone remain blocked by the architecture finding.

### 7.56b Monitored-manifold production charter (r168)

r167 closes the enforcement experiment rather than extending it.  The closure
map is non-contractive in both Binary32 and Binary64; r162 independently puts
the steep-state oracle target generator inside an approximately `1e-4 s`
contraction class; and the r166/r167 regression measures feedback slope
`0.7925875134206254` across the hover corridor.  Those are three views of the
same cost: forcing absolute `P0` consistency makes production inherit
oracle-scale iteration or oracle-scale timesteps.  That is precisely the work
the two-tier architecture assigns to the reference, not to the production
step.

The r160 consumer audit supplies the functional boundary.  The Binary32
admissibility consumer needs the r60 component/energy and affine-row envelope;
temperature inversion needs the certified table bracket; molecular transport
needs accepted temperature/composition; remap and sources need the
conservative ledger; and projection needs density plus the sealed tangent
divergence target.  None consumes an absolute pressure-deviation/P0-consistency
limit.  The checkpoint stores authenticated accepted state, while the manifold
map is an observer.  No production-domain precondition is weakened when
absolute manifold deviation moves from acceptance to monitored fidelity.

The default resident request now measures the terminal absolute-deviation
distribution on Metal and publishes exact Binary32 maximum, p95, and p50.
Crossings of the historical 25%-headroom allowance and `2^-5` low-Mach marker
are identity-bearing events in the run transcript, but do not stall or alter
the step.  Absolute-reference restoration, advective-anomaly closure, and the
manifold timestep limiter are disabled by default and retained behind the
explicit enforcement/instrumentation request.  Monitored accepted observations
publish zero manifold timestep authority, so the ordinary selector remains the
CFL/buoyancy/diffusion/growth selector.  `S_div` is unchanged: the physical
projection still consumes the complete sealed tangent expansion target.

The fail-closed boundary is narrower and stronger for being explicit.
Conservation ledgers, r60 Binary32 affine feasibility, physical-projection
validation, CFL selection, nonfinite detection, and accepted-state
admissibility remain atomic rejection gates.  A monitored result receives an
accepted-state token only after those gates, one physical projection, the
complete payload digest, and all three finite ordered distribution scalars are
revalidated.  The old enforced path remains executable evidence; turning off
monitoring while requesting enforcement fails before Metal.

The one-step CFL GREEN deliberately crosses both diagnostic markers:
`max=G_eulerian=0.08589577674865723`, `p95=0.0007141828536987305`, and
`p50=1.1920928955078125e-7` at represented
`dt=0.0016462659696117043 s`.  It validates the physical projection, executes
zero restoration passes, emits one scalar read, authenticates and applies the
state, and leaves the limiter unavailable.  Exact exit `195` is the retained
behavioral boundary.  Equal-time oracle comparison remains a separate
short-horizon contract fixture; its reference-generator time is never charged
to the monitored production-step timing.

Long-horizon validation returns to the original statistical charter.  The
completed 104-step CFL shadow SHA-binds the max/p95/p50 trajectories, threshold
events, projection-validation count, production device/wall samples, final
state, and golden beginning.  It makes no non-secular pre-commitment: growth,
saturation, or decay is attribution evidence.  A separately accumulated oracle
state is never permitted to become a stale production target.  Instead, each
accepted production beginning owns an instantaneous tangent target built from
the same bounded temperature inversion, molecular transport, open-boundary
flux, and full thermal-expansion `S_div` kernels.  This target is not an oracle
step or a fidelity reference; equal-time oracle composition remains exclusive
to the short-horizon contract.  The synthetic policy RED proves that disabling
the oracle's absolute-pressure detector admits a pressure-only monitored target
while a certified-affine-row mutation still refuses.

All 104 physical projections validated; restoration ran zero times and every
accepted result minted and applied an authenticated token.  The distribution
is strongly localized: max peaks at `1.423297643661499` and ends at
`0.8262996673583984`, while p95 peaks at `0.0011827945709228516` and ends at
`0.00028055906295776367`, and p50 peaks at
`0.00002574920654296875` and ends at `9.238719940185547e-6`.  Both historical
markers cross on every step; those 104+104 events are diagnostics, not stalls.
The trajectory and every per-step target/schedule digest are sealed by trace
`d468729944d82973293ff2afa3f8248be10be3f16f875f77e292805702500d8c`;
the final accepted state is
`5382e567f20be2d7cc15f5ec731441f66b81c90a4153a304cc7fdb58d3ca07ee`.

The CFL/buoyancy/diffusion/growth selector is re-evaluated at every accepted
beginning.  It starts at `0.0016462659696117043 s`, averages
`0.0007690361974779919 s`, and ends at `0.00006441490404540673 s` as the
physical maximum velocity reaches `219.97698974609375 m/s`.  Consequently the
measured `91.62970818579197/179.505958 ms` device/wall p95 projects to
`0.8274219341607761/1.6209499069950497 h` for tier-10 x 25 s; the hoped-for
`0.4-0.6 h` wall class is not met.  Performance remains decoupled from this
milestone, but the physical-CFL collapse is now the named cost attribution,
not hidden host overhead.

Equal-time golden slices,
Binary32/Binary64 subdominance, empirical rows, filtered fields, and tier-10
prefix integrals remain the fidelity contract.  If those later contracts fail
and the monitored trajectory attributes the failure to manifold drift, the
enforcement question reopens against those numbers; that conditional is not a
hidden production stall.

### 7.56c Outlier-bounded monitored manifold (r169)

r168 exposed the production coupling omitted by the r160 consumer enumeration.
Absolute `P0` consistency still has no production consumer, but an order-one
EOS volume deviation corrupts gas density, and gas density is consumed twice:
face velocity is `M/rho_g`, and buoyancy is density dependent.  The r168 chain
is therefore causal rather than merely correlative: localized order-one
deviation precedes density corruption, velocity reaches
`219.97698974609375 m/s`, and the physical CFL step collapses to
`6.441490404540673e-5 s`.  The missed channel is immaterial for small monitored
deviations and material at order one.

The r169 policy retains monitored bulk behavior and bounds only its tail.
Before Metal, the resident owner independently derives a target for every cell
with `|V-1| > 2^-3`: the target opposes the signed excess
`|V-1|-2^-3`, so the intended endpoint is the threshold, not zero.  Cells at
or below `2^-3` receive exact positive zero.  A second, existing restoration
projection is scheduled only when the tail count is nonzero.  The diagnostic
record binds count, excess sum, and exchanged volume
`dx^3 sum(|V-1|-2^-3)` into the accepted token.  The ordinary manifold limiter
and global absolute-reference restoration remain off.  `2^-3` is the
stability engagement class: it bounds the direct density/velocity/buoyancy
error near 12.5%.  `2^-2` is the fail-closed dynamics-validity bound: a
beginning beyond it refuses before Metal, and a realized terminal crossing
refuses without an accepted token; the ordinary API publishes its default
result.

The r168 transcript supplies the first RED.  Its first `2^-3` crossing is step
16 (`max=0.12784385681152344`) at only `8.471941947937012 m/s`; velocity first
exceeds 100 m/s at step 81 (`104.89600372314453 m/s`).  In r169 the conditional
projection therefore first engages on step 17 at `8.753913879394531 m/s`, well
before the old blow-up.  The threshold-zero mutant is the already sealed r166
global-restoration topology: it restores the whole field and reproduces the
`0.7925875134206254` feedback/hover and timestep-collapse signature.  This
pair binds the surgical scope and the reason bulk restoration stays retired.

The requested 104-step GREEN did not result.  Both projections validate and
velocity remains plume-scale, but the localized tail is not bounded by a
single conditional projection.  Thirty-three steps are accepted; restoration
runs on 16.  Accepted maximum velocity peaks at
`10.694913864135742 m/s`, accepted deviation peaks at
`0.23147010803222656`, tail population peaks at 102 cells, and accumulated
exchanged volume is `0.00023257764777146186 m^3`.  Step 33 realizes
`max=0.25728172063827515` at beginning velocity
`10.222167015075684 m/s`; physical and restoration projections both validate,
but the `2^-2` dynamics gate withholds the token and the ordinary API rejects
atomically.  The inherited positive target sign was also tested and amplified
the tail more quickly, refusing at step 24; opposing the excess delays but does
not remove the feedback.

The accepted prefix measures `110.67970842123032/146.903083 ms` device/wall
p95, conditionally `0.4895552021256551/0.6497773577180106 h` at its mean step.
Those are not a 104-step projection because the state cannot pass step 33.
This is exactly the pre-registered genuine-finding branch.  The shadow is not
claimed, and golden `B_fp32`, guard supersession, readmission, thermo/source
maps, and first light remain blocked pending a new owner ruling on the
localized tail mechanism.

### 7.56d Two-dose tail margin and ordinary hard-bound retry (r170)

r170 corrects two margins without changing the outlier-bounded policy.  The
single engagement threshold is now `theta=2^-4=0.0625`; every cell above it is
drained toward `theta`, and every other cell remains untouched.  The derivation
reserves at least two measured worst-case local doses beneath the `2^-2`
dynamics bound: `2^-2 - 2*0.09 = 0.07`, whose stability-class dyadic choice is
`2^-4`.  This is a deeper pre-emptive margin, not a relaxed acceptance bound.
The hard bound remains exactly `2^-2`.

The expanded population remains tail-only.  Restoration first engages at step
1 with one cell, peaks at 9,698 of the tier-10 cells, and runs on 100 of the 104
steps.  The peak per-step excess sum is `137.31766988594759`; the peak and total
exchanged volumes are `0.0020131099705025537 m^3` and
`0.063814808515304383 m^3`.  Exchange is proportional only to
`sum_tail(|deviation|-2^-4) dx^3`; there is still no bulk target.  That is the
feedback-safety boundary relative to r167's global restoration of every cell.

A terminal hard-bound crossing is an ordinary tokenless refusal with a
device-reduced timestep suggestion.  For each violating cell, the reduction
uses its measured local dose and remaining headroom,
`scale=(|d_terminal|-|d_begin|)/(2^-2-|d_begin|)`, and chooses the next lower
representable `dt/scale`.  The shared 20-attempt classifier then rebuilds the
tangent target and retries from the unchanged accepted beginning.  This is
valid because the local advective dose is timestep-scaled.  It is explicitly
different from the retired global manifold limiter: that limiter attempted to
control a measured `G` with a timestep-invariant feedback component, so
reducing `dt` could not remove its floor.

The sealed r169 state proves both sides.  With the retired `2^-3` threshold,
step 33 again refuses at `dt=0.0011971283238381147 s` and
`max=0.25728172063827515`, then suggests
`0.0011418721405789256 s`.  The ordinary retry at that duration accepts at
`max=0.24757766723632812`; both projections validate and an authenticated token
is minted.  The dedicated retired-threshold mutant stops on the original
refusal, so the old r169 finding remains a live RED rather than overwritten
history.

With `theta=2^-4`, the requested shadow completes without needing a hard-bound
retry.  All 104 physical projections validate; all 100 conditional restoration
projections validate.  Step 33 accepts on candidate zero at
`dt=0.0011745213996618986 s` with `max=0.14402782917022705`.  Across the shadow,
maximum deviation peaks at `0.15430498123168945`, p95 at
`0.0021685957908630371`, p50 at `2.0384788513183594e-5`, and physical velocity
at `10.871506690979004 m/s`.  The CFL family remains plume-scale and the final
step is `0.0011549204355105758 s`; there are zero hard-bound retries.

The accepted trajectory simulates `0.13594708242453635 s` with average step
`0.001307183484851311 s`.  The final serialized campaign measures
`112.8718750551343/149.257125 ms` device/wall p95.  Counting complete accepted
step cost, tier-10 times 25 seconds projects to
`0.5996346149904227/0.7929321510804586 h` by p95 and
`0.5658843734913572/0.7588896044270401 h` by measured mean rate.  The result is
inside the hour on device but not at the earlier 0.65 wall-hour expectation;
the actual number is recorded rather than normalized to the forecast.  Trace
`e3273037f56068efb2c067b8b70ec9524cfbd4edcf742c4ac51084b8bde507fa`
binds every accepted dt, distribution statistic, tail population, exchanged
volume, retry count, selector class, and target schedule; the final state is
`b885eec0d01a1c3ddc96a70b797af2cba61769772543c78e9997aa53b79bce3b`.

The 104-step shadow is therefore claimed.  The next authorized rung is the
golden-slice Binary32/Binary64 subdominance measurement; guard supersession,
eight-slice readmission, thermo/source maps, and first light still depend on
their own evidence and are not inferred from this shadow.

### 7.56e Golden-slice B_fp32 and guard supersession (r171)

r171 closes the last additive-contract term on eight independently restarted
golden beginnings.  The historical r138a continuation payloads were not
present in the workspace; their hashes remain immutable historical evidence
and were not rewritten.  Instead, a new identity-bearing generator command
replays the canonical current-build continuation from the unchanged root,
then requires exact SHA-256 for each beginning's canonical physical state and
lifecycle before measurement.  Whole-checkpoint bytes are not the slice
identity because they include the executable digest and therefore change on a
clean relink without a physical-state change; the unchanged root still keeps
its independent whole-file SHA gate.  The current set was sealed before the
admitted eight-slice run.  A root-only
diagnostic observed before that seal was discarded and cannot select an input
or enter the result.

The monitored-policy golden states have no `2^-4` tail, so the current
production scheme executes one terminal physical projection, not the retired
global-restoration pair.  The strict binary64 mirror was corrected to follow
that topology exactly: it uses the same force schedule, five cell remaps,
fifteen dual remaps, exact `+0` source, and 16-cycle terminal projection.  If a
tail is present, the same adapter derives the identical thresholded target and
uses physical plus restoration projections.  Projection topology and cycle
counts are compared before a precision value is admitted.

For each slice and each of nine filtered scalars, one filtered-velocity metric,
and nine inventory quantities, the frozen rule remains
`|P32-P64| <= 2^-3 E_P`.  All `8*(9+1+9)=152` gates pass.  Maximum filtered
velocity difference is `3.2429213131399156e-9 m/s` against
`7.363073950686612e-4 m/s`, a minimum velocity margin of
`227050.65093168768`.  The minimum filtered-scalar and inventory margins are
`7972.9650118056461` and `8894.785377013457`.  Every per-slice
value/bound/margin triple is retained in the raw evidence; the aggregate trace
is `1e48343aa5589cded65f2345e74d2ba103508bdf07fb9ab56e5ea7351cd00b61`.
No oracle value participates in these precision inequalities.

This certifies the composed `B_fp32` terms as the pre-registered one-eighth
production distances and completes the additive contract's precision term.
It also supersedes the preliminary velocity guard: the retired guard is
`3e-5 m/s`; the derived, now-measured term is
`7.363073950686612e-4 m/s`.  The replacement is larger because it is derived
from the scheme's independently measured distance to its limit, not widened
from the observed `3.24e-9 m/s` difference.  Guard retirement therefore
changes no measurement and consumes none of its margin.

Durable evidence is under
`rendered/fire_production_calibration/r171_golden_subdominance`.  The next
authorized rung is the eight-slice equal-time readmission verdict; source maps
and first light remain unclaimed.

### 7.56f Tier-6 temporal-refinement stop (r172)

r172 executes the three-level temporal instrument frozen by r139 before the
eight-slice readmission.  The tier-6 analytic beginning, represented baseline
`dt=0.0018513043178245425 s`, dyadic halves, step counts `8/16/32`, common
horizon `0.01481043454259634 s`, filters, metrics, and formal orders are
unchanged.  A capability-isolated `FireProductionCalibrationOracle` process
with no Metal framework generates all three binary64 target schedules into
write-once payloads.  Their manifest and payload SHA-256 digests are checked
before the Metal-capable evaluator opens them for const replay; the evaluator
also rejects an actual penultimate-target substitution before any dispatch.
The analytic beginning is checked against the r139 SHA plus the consumed
all-cell Binary64 producer class, with a Binary32 producer-class mutation RED.
Only after those boundaries close is the generated binary64 production mirror evaluated.  The
production mirror performs binary64 stage arithmetic, publishes each
intermediate request through the production binary32 state boundary, and
measures each level's final pre-publication binary64 output.  This keeps
`B_fp32` separate while preserving the actual multi-step request lifecycle.

Eighteen of nineteen quantity classes satisfy `D_coarse>D_fine>0` and obtain
their pre-registered Richardson term.  Production sensible energy is first
order with `D=1.3664113219736267/0.68316914382060645` and
`E_dt=2.7328226439472538`.  Velocity obtains production/oracle temporal terms
`1.5098888236479335e-5` and `4.1666621096787029e-5 m/s`; all nine ledgers and
the other eight scalar components also contract.

The oracle sensible-energy observable does not.  Its coarse and fine
differences are `0.0012312438866646748` and `0.001273209006325096`, so the
ratio is `0.9670398815497335 < 1`; no positive temporal order or finite
Richardson distance may be emitted.  This is not replaced by a fallback
constant and no tolerance is learned from the golden production difference.
It is the sole refusal in the complete matrix (`production=0`, `oracle=1`),
not an alias for another noncontracting row.  Consequently the additive contract is incomplete and the eight-slice
readmission is not run.  The manifold-floor attribution flag is likewise not
evaluated because no admissible contract exists against which to judge it.
Thermo/source maps and first light remain blocked at this measured temporal
prerequisite.  Exact exit `193`, all quantity rows, and target schedule hashes
are retained under
`rendered/fire_production_calibration/r172_temporal_refinement_stop`; the
golden checkpoint remains unchanged.

### 7.56g Filtered temporal-floor amendment (r173 protocol)

r173 corrects the r172 decision rule without changing the operator, filter, or
numeric constants.  Temporal observables inherit r112 exactly: the nonnegative
partition-of-unity tensor cubic cardinal B-spline is applied before every
difference at physical width `0.04894898570785762 m` and support radius twice
that width.  Scalars use filtered volume-L1, velocity uses filtered volume-RMS,
and inventories retain their exact global reductions.  The kernel owner and
r112 protocol/metric artifacts are SHA-bound before the rerun.

The r172 sensible-energy ratio `0.9670398815497335` came from near-equal
already-filtered differences, so it is diagnosed as a dt-independent front
branch-flip floor in the r123 census lineage, not as an unresolved temporal
slope.  V2's formal-order gates independently establish temporal consistency.
The pre-registered outcomes are therefore exhaustive for the measured r172
tuple: a contracting channel uses its registered Richardson slope and
distance; only oracle sensible energy at the exact evidence pair
`(0.0012312438866646748, 0.001273209006325096)` may use
`nextUp(max(D_coarse,D_fine))` as the measured filtered-floor upper bound.
Every other noncontracting pair refuses.  The authorized quantity is a
consistency-derived bound for this identity-bearing observation, not a fitted
order, generic fallback, or new tolerance.  All scalar, velocity, and
inventory channels are re-evaluated uniformly.  No readmission result is
inspected before this rule is sealed.

The exact rerun takes the second branch only for oracle sensible energy.  Its
bound is `nextUp(0.001273209006325096) = 0.0012732090063250962`; all other
production and oracle scalar, velocity, and inventory channels retain their
Richardson terms.  The complete matrix has zero refusals, 37 Richardson terms,
and one measured-floor upper bound across 19 paired production/oracle rows.
Thus the temporal part of the additive
contract is complete without fitting a slope through branch-flip noise.  Exact
exit `192`, all 19 paired rows, the no-Metal target schedules, and the raw transcript
are retained under `rendered/fire_production_calibration/r173_filtered_temporal`.
The next rung is the eight-slice readmission verdict; no readmission result is
claimed in this entry.

### 7.56h Equal-time eight-slice readmission (r174)

r174 applies the now-complete additive contract to the eight sealed r171
beginnings.  Every observable uses the same SHA-pinned r112 tensor cubic
cardinal B-spline and physical width before differencing.  Production advances
one represented step.  The Binary64 reference advances to that exact endpoint
with eight converged substeps: seven equal nominal substeps followed by the
positive representable remainder that makes the accumulated endpoint exact.
The terminal target digest and endpoint are checked before production consumes
them.  The duplicate serial oracle replay is intentionally absent here because
r112/r139 already own worker-count identity; this rung measures the admitted
contract, not that retired duplication.

All 152 inequalities pass: nine filtered scalar rows, one filtered velocity
row, and nine inventory rows on each of eight slices.  The maximum scalar,
velocity, and inventory contract ratios are 0.0018329622432418256,
0.0081254983789433733, and 0.0032325000146012773.  Thus the original
83x/28x/8.7x excess classes pass under the completed filtered equal-time
contract rather than being hidden or tolerance-widened.  The pre-registered
manifold-floor fidelity flag is evaluated and is not implicated.  Exact exits
`189/188`, all 152 identity rows, trace
`75817882f342187f794a263f6adfd58ad4d165b6b91e433529afac8c1fdcfc19`,
and the raw transcript are retained under
`rendered/fire_production_calibration/r174_equal_time_readmission`.

This completes readmission.  Thermochemistry/source maps, their Binary32
source-producer subdominance term and burning-tail measurement, and first
light remain the next rung; none is claimed by r174.

### 7.56i Binary32 thermochemistry/source maps and first-light release (r175)

r175 removes the production `+0` source restriction without weakening any
conservation-class gate.  A Binary32 frozen source packet is accepted only
when every resident source byte is finite and exactly representable, positive
zero is canonical, the mass and certified affine rows close, the packet ledger
closes, and the post-source conservative state remains inside r60.  The source
producer contributes `128 eps32`; remap (`256`), force (`64`), two possible
projections (`2*256`), and source therefore total `960 eps32`, below the
existing `1024 eps32` accepted-state envelope.  No tolerance is widened.

The sealed golden burning checkpoint uses the canonical ignition-eligibility
map, no pilot source, soot oxidation, the case's `33 kW` nominal release, and
the audited CFL step `0.0016462659696117043 s`.  Independent Binary32 and
Binary64 packet construction gives 219 active cells and `15289.762218506474 W`
realized heat release.  The source-packet digest is
`acf87f65...cf0e`; the source-induced tangent target is
`7334d417...d0eb`.  Every component satisfies the pre-registered
`|S32-S64| <= 2^-3 dist` rule; the minimum margin is
`112.55273459563601x`.

Production accepts the first step.  The monitored manifold distribution is
max/p95/p50 `0.085888981819152832 / 0.00071436166763305664 /
1.1920928955078125e-7`, below the `2^-2` dynamics bound.  There are no
beginning-state tail cells, so the tail-restoration projection is correctly
not invoked; the single terminal projection validates and an accepted-state
token is minted.

The fixture then applies that accepted payload and repeats the real source
step from the produced beginning.  The source-created tail now engages exactly
one cell: excess `0.023389111965054887` is drained by
`3.4288999032069217e-7 m3`.  Both projections validate, max/p95/p50 are
`0.079523563385009766 / 0.00082623958587646484 /
8.3446502685546875e-7`, the hard bound remains clear, and the second step also
accepts.  This closes the pre-registered burning-tail continuation with the
correct one-step causal ordering.

The paired run measures device/wall projections of
`0.40872554073287515/0.63840048687616735 h` for tier-10 x 25 s.  Source maps
therefore add device work honestly while retaining the sub-hour device class
and the requested wall class.  Exact exit `184` and both source, target,
schedule, tail, projection, timing, and source-owner records are retained under
`rendered/fire_production_calibration/r175_thermo_source_maps`.  This accepted
checkpoint releases the explicitly preview-labelled tier-6 first-light
artifact.  The clean capstone reaches `2.3170101413580206 s` in 523 accepted
steps, peaks at `2284.8533 K` with pilot input off, and terminates at
`1784.60397 K` with `1.27718934e7 W/m3` maximum heat release.  The terminal
scene-linear primary is
`rendered/fire_production_first_light/r175_preview_tier6/methane_preview.exr`
(SHA-256 `bf02ffa4...82c9`); its canonical provenance sidecar is
`5e6187b9...44e`.  The animation retains eight FP32 EXR
`preview_primary` frames and canonical sidecars.  Their ImageIO-authored,
looping 8-frame/8-fps display derivative is
`methane_preview_animation.gif` (`994a5b1a...77f9`); the visible blue-plume
still is `methane_preview_display.png` (`53b0cb29...3b3`).  The PNG and GIF are
explicitly `display_derivative`, never `preview_primary`, and each is linked
to its primary input.  A +6 EV renderer-owned ACES-to-sRGB view transform
exposes the preview.  Its preview-only volume spatializes the sealed reaction
and thermal-excess fields while retaining their exact checkpoint maxima; it
does not alter the simulation, source-map evidence, golden checkpoint, or
primary-radiance contract.  AVFoundation returned `AVErrorCannotEncode` in the headless
authoring process, so the optional MOV derivative was not claimed; ImageIO
GIF/LZW is the truthful portable derivative while the EXRs own the evidence.
The committed sidecars identify the authoring binary honestly as source revision
`f3b56b90e8349b0246b39d1d5f601b5afd416e30`, dirty-state digest
`a82c5699...348c`; the r175 evidence binds both rather than substituting a
symbolic rung label for producer provenance.

The animation is a preview camera move over the terminal monitored state, not
an assertion that eight additional solver timesteps were advanced.  Its sealed
20-degree orbit and dolly produce eight visible structured blue-plume frames;
every adjacent decoded frame differs and the lit-area range changes by at least
five percent.  The lit-mask centroid must move by at least two display pixels.
A synthetic seven-black-frame/one-terminal-plume sequence, a visible static
sequence, and static geometry with digest/area flicker all fail the shared
classifier.

Publication is fail-closed at the file boundary.  The producer intentionally
commits sidecar before artifact; a unique staging directory and per-frame
fresh-provenance wait therefore require the complete new artifact/sidecar pair
and verify artifact digest, provenance identity,
primary linkage, non-black decoded pixels, eight decoded frames, and temporal
change before copying anything.  Sidecar-first/delayed-artifact REDs cover
both the primary and visible derivative interleavings.  No conditional block
can now silently return success without first light.

### 7.56j Tier-6 full spectrum and complete-owner cost correction (r177)

The tier-6 temporal trajectory is measured over its complete statistics window,
not reduced to the largest Fourier bin.  The centerline signal is the existing
central two-by-two-column, full-height, cell-volume-integrated heat-release
probe.  The visible-area signal is the accepted preview definition: the number
of display pixels whose maximum sRGB channel is greater than 127.  Each signal
is linearly resampled to 512 uniform points over its own exact observation
span, least-squares affine detrended, multiplied by a symmetric Hann window,
and evaluated at every nonnegative DFT bin through Nyquist.  The complete
signals, display-input hashes, and spectrum table are retained under
`rendered/fire_production_calibration/r177_tier6_puffing_spectrum`.

Both spectra are dominated by `0.13648504273426187 Hz`.  The dimensional
puffing expectation is `1.5/sqrt(0.30) = 2.7386127875258306 Hz`; its nearest
bin is `2.7297008546852375 Hz`.  Centerline expected-bin power is
`0.00035234515564980826` of total positive-frequency power and
`0.0013755911450037712` of the dominant-bin power.  Lit-area values are
`0.00022353198046603235` and `0.001295326657846992`.  The wider
`2.5--3.0 Hz` bands contain `0.0113695656449402` and
`0.002445681094526029` of total power.  Tier 6 therefore shows a weak expected
component beneath a slow domain mode; it does not show that the expected
component is absent.  This interpretation preserves the r57 resolution pin:
four burner cells were the general-case admissibility minimum, tier 6 supplies
`7.3545957039557281`, and puffing was pre-registered for the refined
approximately-ten-cell claim tier.

The same rung replaces the partial resident-step wall projection with a
complete-owner measurement.  The tier-6 run consumed `8388.969329416 s` for
15,662 accepted steps (`535.625694123 ms/step`).  Resident attempts account for
`115.223978036 ms/step`; `420.401716087 ms/step` is owner preparation, target,
state publication, and statistics.  The 235 frame writes are not responsible:
a one-warmup/five-sample durable VDB benchmark measures `34.131667 ms` p95 per
frame.  Physical projection retried 7,183 times; hard-bound retry count is zero.

On tier 10, the first complete-owner profile is `2008.334 ms`: request layout
`628.994`, target generation `306.259`, resident publication/application
`492.809`, with the balance in eligibility, transport, source, control, and
postprocessing.  The r177 scheduling-only repair uses fixed worker partitions
for request construction, reuses the exactly reinverted canonical temperature,
and reuses molecular thermochemistry while recomputing all target velocity
gradients and eddy terms.  Three REDs compare the complete request, every
canonical temperature, and the entire divergence target with their prior
paths.  After warmup, nine tier-10 steps average `1116.682666667 ms`, including
`99.410111111 ms` layout, `199.215777778 ms` target, and
`430.550444444 ms` resident publication/application.  At the audited CFL step,
the 25.156864-s campaign projects to `4.74005935697 h` before any later-state
retry cost.  The earlier `0.638400486876 h` number remains a valid resident
microcampaign observation but is retired as a complete production-run claim.
The tier-10 run proceeds under this explicit cost account; no physics tolerance,
operator ordering, or checkpoint authority is changed.

### 7.56k Tier-10 density/velocity physics stop (r178)

The tier-10 trajectory is launched at the pinned cadence under the r177
complete-owner account.  It cannot reach the `5 t_ft` discard boundary.  The
last recoverable format-13 checkpoint is SHA-256
`acb53171836660252789379ecdd669da47bd36124d6e78e46244c727019455e5`, at
step 1,542 and `2.1426204254821641 s`.  Its minimum accepted timestep is
`3.0300463549792767e-5 s`, already roughly 54 times below the audited CFL
operating step.

The read-only r178 audit localizes the failure.  Maximum physical velocity is
`255.64169311523438 m/s`, with an independent advective candidate of
`4.786874268372288e-5 s`.  Maximum positive reduced gravity is
`62.179819320204423 m/s2`, but its buoyant candidate is
`0.01402869021009805 s`; it is not the selector that collapses the step.  Gas
density ranges from `0.15965474117547274` to `1.2099052290432155 kg/m3`
against `1.1719579191113527 kg/m3` ambient.  The diagnosed chain is therefore
density corruption coupled to momentum growth -> advective CFL collapse.  The
narrower claim that density division alone owns the velocity is superseded by
r179's direct momentum/scalar compatibility and projection-impulse audit.  The
simultaneous manifold max/p95/p50 is
`0.11917137460802585 / 0.009886252187254363 /
1.1382639254042815e-5`.

Heat-release accounting remains tight: the checkpoint source probe gives
`94466.977979105024 W`, while methane consumption times LHV gives
`94467.009612291862 W`, only `3.3485973103748931e-7` relative difference.
Thus the bundle points away from the combustion ledger and away from the
buoyancy timestep selector, and toward density/momentum consistency in the
production trajectory.  Since the failure occurs before the statistics
window, no tier-10 spectrum, empirical rows, or temporal animation are
claimed.  Continuing would produce neither the requested physics row nor a
meaningful cost projection, so the run is deliberately stopped.

The checkpoint format does not persist the in-memory retry counters or
per-step device/wall histories.  r178 records both as unavailable rather than
reconstructing them from transient console output.  The retained small
evidence files are under
`rendered/fire_production_calibration/r178_tier10_density_velocity_stop`; the
121-MB restart checkpoint remains a local recoverable run product identified
by the SHA above.

### 7.56l r178 momentum decomposition and cap decision (r179)

The r178 checkpoint is instrumented before applying the proposed restoration
cap.  Its minimum gas-density cell has `rho=0.15965474117547274 kg/m3`,
`T=1981.9763228118441 K`, and signed deviation
`-0.060475840911561773`; the low density is not manifold-consistent.  At the
maximum-velocity face, the two adjacent `(rho,T,deviation)` tuples are
`(0.23534662136808038,1614.7138732295894,+0.11917137460802585)` and
`(0.21136353025212884,1643.7593009811521,+0.02378781404850927)`.  Face density
is `0.22335506975650787 kg/m3`, momentum is
`57.098869323730469 kg/(m2 s)`, and velocity is
`255.64169311523438 m/s`.  The maximum checkpoint compatibility residual
`|M-rho_f u|` is `9.639436029829085e-6`, so the velocity is the compatible
image of an already-large momentum rather than a stale scalar/momentum pair.

The short decomposition starts from a copy of the recoverable checkpoint and
accepts eight more steps.  Every request is executed normally and again with
the existing restoration-removed diagnostic; transported-dual momentum must
be byte-identical, which makes the physical-only terminal the projection
baseline for the normal restoration result.  Across
`dt=4.7868743422441185e-5--6.0848105931654572e-5 s`, maximum physical velocity
is `204.94149780273438--254.57662963867188 m/s`.  Restoration's maximum face-
velocity change is `6.11376953125--12.380699157714844 m/s`; its largest ratio
to maximum physical velocity is `0.058162335108278389`.  The proposed
momentum-subdominant ceiling is `2^-3=0.125`, so it is slack by more than a
factor of two.  Maximum restoration/physical projection-impulse ratio is
`0.17676903757991816`; restoration does not dominate.  The replay's largest
terminal compatibility residual is `1.8754599295789376e-6`.

The pre-registered conditional therefore refuses the implementation branch:
a cap that never engages cannot make the r178 replay plume-scale, and adding
it would create machinery without a measured customer.  The r159 lineage is
recorded exactly: excluding per-step-finite restoration velocity from CFL was
necessary, but not sufficient to prevent momentum accumulated elsewhere; the
new evidence shows restoration itself is not that accumulation owner.  r178
is reclassified as coupled off-manifold density and physical-momentum runaway.
Tier 10 remains blocked until the physical projection and its provisional
momentum budget are decomposed.  No cap, cap REDs, full-window spectrum, or
animation is claimed by r179.  Raw per-attempt data and the dt plot are under
`rendered/fire_production_calibration/r179_r178_momentum_decomposition`.

### 7.56m Force-inclusive projection budget and preservation stop (r180)

The r179 stop is instrumented at the recoverable production checkpoint before
choosing either proposed projection remedy.  The schedule audit is explicit:
the resident force solve produces force-inclusive staggered momentum; the dual
remap acts on that momentum; the face-source impulse is added; and this complete
provisional momentum is passed to the physical projection in the same step.
Only after that solve does the thresholded restoration projection run.  An
independent Metal-force/CPU-dual reconstruction differs from the resident
provisional field at 10,960--13,521 faces by at most
`1.9073486328125e-6--3.814697265625e-6 kg/(m2 s)`, the one-Binary32-ulp class at
the extreme momentum.  It is retained as an independent schedule witness, not
mislabelled as byte identity.  The pre-registered post-projection force defect
is therefore false.

The eight-step replay writes all 49 vertical faces in column `(38,42)` and
closes
`dM/dt = stress + buoyancy + advection + source + pressure + restoration`
with maximum absolute residual `0.009961346891941503 kg/(m2 s2)`.  Across the
eight steps, column maximum absolute rates are:

| term | minimum of step maxima | maximum of step maxima |
|---|---:|---:|
| stress | 3,135.266 | 5,352.502 |
| buoyancy | 9.482 | 9.587 |
| advection | 79,966.694 | 176,609.678 |
| pressure gradient | 149,525.786 | 266,096.563 |
| restoration | 27,874.478 | 45,603.292 |
| total | 149,653.942 | 266,967.063 |

The units are `kg/(m2 s2)`.  At the original runaway face (`z=10`), pressure
ranges from `-63,579.314` to `+42,226.628` while buoyancy remains
`9.303--9.524`.  Pressure is not weak relative to buoyancy; it is four orders
larger.  Consequently the conditional second-projection/tighter-tolerance
branch is not authorized, and r180 lands no production arithmetic change.

The requested cross-solver table cannot be completed from the preserved oracle
bytes.  The oracle console retains scalar rows at steps 1,100/1,110/1,120 and
times `2.09079544/2.10336069/2.11639945 s`, but not density, species,
staggered momentum, pressure, or per-term fields.  Its atomically replaced
checkpoint now holds step 3,479 at `2.8854439500002069 s`.  That later state is
not substituted as a matched reference.  The decisive cross-solver attribution
therefore stops until a full oracle checkpoint in the step-1,100--1,120
neighborhood is recovered or regenerated.  Tier 10, its spectrum, empirical
rows, and animation remain blocked; neither proposed projection remedy is
claimed on a one-sided budget.

### 7.56n Runaway-onset and retained-state campaign (r181)

r180's later-state table is not used to choose a remedy.  The tier-10
production trajectory is regenerated from zero with one max-velocity record per
accepted step, immutable step-numbered checkpoints in addition to the atomic
`latest.checkpoint`, and one-shot column budgets at the first crossings of 15,
30, and 60 m/s.  The retained-checkpoint writer refuses a duplicate step name
whose bytes differ.  A threshold-audit controller defect discovered by this
campaign is also fixed: the audit had completed and published the 15-m/s row,
then tested the step's not-yet-published acceptance flag and stopped the solver.
The audit now owns its computed-attempt disposition before it can gate the
trajectory.  Budgets are written only for accepted attempts.  When targeted
restoration keeps its physical projection resident, the diagnostic runs one
restoration-disabled attempt through the same Metal force/remap/projection path
and requires the transported provisional momentum to match before using that
physical impulse; it never substitutes a CPU projection.

Tier 6 completes to `2.2000000000116415 s` in 1,339 accepted steps with a
`13.52214527130127 m/s` maximum.  Tier 10 reaches `14.056404113769531 m/s` at
step 1,442 and `2.1312820004532114 s`; the corrected continuation then crosses
`16.140964508056641`, `30.402894973754883`, and
`63.686721801757812 m/s` in only `0.0066079621465178 s`.  Its represented step
falls from `0.00087058159988373518` to `0.00020934514759574085 s` over the same
interval.  The optional tier-8 control remains plume-scale through step 1,123
and `1.8264868019614369 s` (`9.005696296691895 m/s`, historical peak
`10.292028427124023 m/s`).  It is deliberately stopped there rather than called
a completed window: beyond `1.7755 s` it starts the ordinary monitored
dynamics-bound retry, accumulating 110 refused candidates while velocity
remains small.  This separates the scalar-tail event from the fine-grid
momentum runaway.

At the velocity-owning face in each selected column, the signed rates are:

| state | stress | buoyancy | advection | pressure | restoration |
|---|---:|---:|---:|---:|---:|
| tier-6 local peak (`11.05 m/s`) | -1.529 | 7.479 | 467.238 | 487.314 | -322.201 |
| tier-8 pre-bound (`8.86 m/s`) | -0.292 | 8.287 | 397.969 | 400.239 | -273.497 |
| tier-10 15-m/s crossing | -14.924 | 9.710 | 1,081.759 | -752.522 | 160.928 |
| tier-10 30-m/s crossing | -55.717 | 9.547 | 3,616.685 | -2,570.920 | 379.546 |
| tier-10 60-m/s crossing | -255.019 | 9.445 | 14,648.895 | -9,909.792 | 1,221.155 |

The units are `kg/(m2 s2)`.  Pressure responds at the same order as advection
and opposes it at every tier-10 threshold; the pressure-under-response branch
is false.  Restoration is secondary and its magnitude/advection ratio falls
from 0.149 to 0.083 through the three aligned faces.  Vreman is not numerically
zero: the larger adjacent-cell value grows from `0.0041604521684348583` through
`0.0078698946163058281` to `0.016612404957413673 m2/s`.  Nevertheless the
resulting stress remains only 1.38%, 1.54%, and 1.74% of advection.  This is the
pre-registered dissipation-silent outcome in the dynamical, not bitwise, sense:
the smooth accelerating column is not controlled by SGS stress, and production
remap dissipation does not substitute for it at the refined grid.

The resolution controls make the distinction sharper.  The aligned tier-6 and
tier-8 Vreman maxima are only `0.0007109809666872025` and
`0.0005931847845204175 m2/s`; their total stress is respectively 0.327% and
0.073% of advection, yet pressure/restoration keep the velocity plume-scale.
At tier 10, Vreman and stress rise only after the focusing mode is established;
they do not prevent its 6.6-ms acceleration corridor.

The onset verdict is therefore advective focusing with resolution dependence.
It selects the remap/flux-limiting remedy class and rejects both a projection
tolerance change and an arbitrary viscosity constant.  The r63 `tau`
lineage applies here: a dissipation coefficient chosen because it suppresses
this trajectory would be another fitted knob, not a derived production
contract.  A retained binary64 oracle trajectory is regenerated independently
through 2.2 s; its diagnostic replay preserves the oracle's exact R0/R1
buoyancy, stress, source, accepted flux, and final pressure operands.  Immutable
step 1,024 at `2.1079791976176079 s` supplies the matched comparison.  Across
its eight-step replay the column stays at `4.88894--5.06229 m/s`.  At the
largest aligned oracle advection event the signed rates are stress `-1.05823`,
buoyancy `4.04890`, advection `63.33961`, and pressure
`-14.62329 kg/(m2 s2)`; the nonpressure split residual is below `1.78e-15`.
Production's first accepted 16.14-m/s crossing has `1,081.75857` advection,
17.08 times the oracle aligned maximum, while its pressure response grows to
`-752.52229`.  Pressure is not missing; it is reacting to a transport mode
that is already much too large.

The oracle's accepted scalar face alpha is `1` at the velocity-owning face,
but the minimum elsewhere in the same column falls from `0.85065` to
`0.23367--0.38942`.  That measurement motivated a coefficient-free diagnostic
trial coupling the primary nine-component shared alpha into the dual remap.
It delayed the main onset from `2.13128` to `2.14092 s`, but did not remove it:
the run still crossed `15.15995`, `32.16662`, and `61.39589 m/s` by
`2.14853 s`, with aligned advection reaching `1.50922e4 kg/(m2 s2)`.  A second
candidate bounding reconstructed momentum/density ratios changed the terminal
time by only `1.45e-8 s`.  Both trials are rejected and removed from ordinary
production; neither is a claimed remedy.

The stronger result is an architecture finding: a cell-average reconstruction
cap and a local velocity-envelope cap do not control the unconformed dual-grid
flux divergence that focuses at tier 10.  The next remedy class must be a
conservative compatible flux correction or adaptive front/column dissipation
derived from the onset budget and checked against a matched oracle state—not a
viscosity constant fitted to this trajectory.  The independent oracle regeneration
retains step 1,024 for that comparison but stops honestly at `2.15671067 s`
(step 1,070) on its physical-temperature/Picard-Zeno boundary; it did not reach
`2.2 s`, and no later checkpoint is represented as matched.  Tier-10 spectrum,
empirical rows, animation, and the capstone report remain blocked until the new
flux-level remedy is designed and replays r178, r170, and the admitted cold
slices.

### 7.56o Compatible momentum-flux conformance and measured stop (r182)

r182 implements the coefficient-free §3.7 candidate in an identity-bearing
resident diagnostic route, rather than either rejected r181 candidate.  Every
one of the five scalar palindrome passes retains the accepted Binary32 gas-mass
dose obtained from the actual shared-alpha limited primal face flux.  When the
candidate route is selected, the staggered momentum pass maps that accepted
flux with the boundary-aware arithmetic MAC restriction and advances

> `K_i = I_i(Phi_hat_g) (u_i,L + u_i,R) / 2`.

The staggered auxiliary density uses the same restricted flux.  At a pressure-
open or wall boundary the ambient half of the staggered density is fixed, so a
transverse interior dose is restricted by one half; the earlier v1 diagnostic
incorrectly duplicated the full interior dose there.  The corrected periodic
and pressure-open/wall REDs now keep `M_i - U I_rho,i rho_g` at roundoff for a
uniform velocity even with spatially varying scalar alpha.  The candidate Metal
path consumes five Private retained-flux buffers in one 15-submap command and
performs no interstage host transfer.  Those five retained buffers and every
candidate dispatch occur only through the explicit diagnostic API; the shared
Metal context still initializes the retained diagnostic pipeline states once at
process setup.  Ordinary production retains zero candidate fields, performs no
candidate dispatch, and cannot activate the experiment through process
environment.  The diagnostic owner passes its immutable resident request by
reference and selects retention only on the resident cell task; it does not
deep-copy the tier-10 host payload, so the certified peak remains the Metal
resident working set rather than an unreported host duplicate.  A
CPU/Metal commuting RED, malformed retained-flux RED, and old-independent-remap
difference RED bind the implementation.  The strict mirrors remain on the
ordinary admitted operator because the candidate was not adopted.  The
mechanism is the one selected at r181: momentum
inherits the scalar limiter's front-adaptive dissipation through its mass flux;
the removed independent momentum reconstruction had no such local coupling.
The earlier scalar-alpha cap and reconstructed velocity-envelope cap remain the
rejected-candidate trail and are not revived.

The v1 onset comparison is withdrawn: it followed each production step's moving
maximum column rather than the oracle's fixed `(38,42)` column, called one
terminal velocity the trajectory maximum, and contained the pressure-open
restriction error above.  Fresh review also rejected the first attempted v2
trajectory: its bytes were the unchanged r181 ordinary trajectory, and its
step-1,400 continuation switched operators only after resuming.  Those hybrid
CSV files are removed and none of their `14.06/23.40 m/s` claims survives.

The replacement run begins at the analytic tier-10 golden state with the
explicit compatible-momentum API active on every step.  Its summary binds that
mode, build ID `01c760fe...46bf8`, executable SHA, trajectory SHA, and final
checkpoint SHA.  The candidate crosses 15 m/s at accepted step 885 / beginning
time `1.3969225193141028 s` / accepted time `1.3978566413279623 s`; the aligned
advection rate there is `914.30106927339784 kg/(m2 s2)`.  It reaches the 30 and
60 m/s classes together at step 1,222, which is the campaign's pre-registered
first-accepted-`>=60 m/s` stop.  Seven ordinary hard-bound retries precede that
accepted state, whose represented step is `4.2156947377414156e-10 s` and whose
recorded maximum is `4,768,055 m/s`; no subsequent CFL selection was run, so
this is not evidence of a CFL-selector stall.  The late event is also not
advection-dominated: its maximum pressure-gradient, restoration, and advection
rates are respectively `1.0998092339304464e16`, `1.6466909209178628e15`, and
`282.77495636377211 kg/(m2 s2)`.  It is a hard-bound-retry/projection-correction
event and is recorded separately from the valid early advective-onset result.
The run stops at `1.731649393462722 s`, before its sealed `2.2 s` target.

The pre-registered matched-column criterion nevertheless **does not pass**.
A caller-SHA-sealed checkpoint at step 1,400 / `2.0792286795331165 s` is
replayed for 28 accepted steps with the diagnostic column fixed at `(38,42)`.
At production beginning time `2.1077302111661993 s`, the column advection-rate
maximum is `321.34195540099722 kg/(m2 s2)` and the physical velocity maximum is
`11.123290061950684 m/s`.  The nearest retained oracle beginning is
`2.1079791976176079 s`, whose column advection-rate maximum is
`159.00892323854879`, giving a matched ratio of about `2.0209`.  The historical
`63.339610872283018` oracle sample is at `2.1135824235796083 s`; it was not the
matched `2.10798 s` value claimed by v1.  The corrected evidence therefore does
not reuse the moving-column `494.85`, the later unconformed `1081.76`, or the
unmatched `63.34` as if they were one equal-time comparison.

The candidate fails both the oracle-class aligned-budget criterion and the
continuous from-zero onset trajectory.  The tier-6/tier-8 controls and
r170/readmission obligations are consequently not promoted from either invalid
earlier experiment and must be re-derived only if a later ruling revives this
candidate.

Because those pre-registered obligations fail, the compatible candidate is not
adopted by ordinary production; the prior admitted remap remains the default,
while the candidate and its exact diagnostic activation stay retained for
reproduction and future design work.  This is a rollback of an unsuccessful
candidate, not a weakening of its acceptance criteria.

The historical r181 unconformed trajectory remains the immutable behavioral RED:
it contains the exact `16.1409645 -> 30.4028950 -> 63.6867218 m/s` crossings
and its source/evidence SHA.  r182 therefore records a genuine measured stop,
not a successful tier-10 remedy: compatible flux is implemented and retained,
but the matched-column onset criterion is still about 2.02x the equal-time
oracle balance and the from-zero candidate reaches 15 m/s about `0.733 s`
before the unconformed r181 crossing, then collapses its step before `2.2 s`.  The
healthy-resolution/r170/readmission obligations have not been re-derived under
the corrected operator.  Tier-10 full
window, spectrum, empirical rows, animation, and capstone report remain
blocked pending an owner ruling on this residual compatible-flux/tail coupling.

### 7.56p Tier-8 deliverable stop and projected-Heun bootstrap (r183)

Track A began immediately from the analytic tier-8 state with the pinned
`0.0625 s` frame cadence and the full `5 t_ft` discard plus statistics window.
It did not reach the first statistics sample and therefore produces neither a
spectrum nor an animation.  The run reached accepted step 1,980 at
`2.70156268 s`; its smallest logged step was `5.57543046e-7 s`, its peak heat-
release probe was `1.09938804e10 W/m3`, and 819 hard-bound candidates were
rejected.  A separately retained step-2,149 checkpoint at
`2.703921805823768 s` localizes the collapse: maximum physical velocity is
`429.0181884765625 m/s` on the vertical face `(34,34,45)`, the advective CFL
candidate is `3.56548008e-5 s`, and realized heat release is already zero.
The tier-8 preview remains honestly labelled and rows remain unclaimed; a
full-window spectrum or movie cannot be formed from zero frames.

Track B closes the form-level-graft route.  The tokenless single-stage FCT
diagnostic does execute the compatible flux, but profiling shows about
`2.50--2.53 s` resident device time and `3.09--3.17 s` completed-call wall time
for each of its first two tier-10 steps.  More importantly, its force/FCT/
projection ordering is not the §3.7 projected-Heun tableau.  It is retained as
the old-operator RED only and is not eligible for production acceptance.
During that diagnostic, source staging exposed an r60 ownership bug: stored
species may contain affine-envelope negative roundoff even though the physical
mixture is admissible.  Sensible energy is now evaluated with the signed
mixture routine at that seam; the stored constituents are not clipped or
rewritten.

r183 establishes the independently testable prerequisites for the faithful
owner:

- scalar FCT is split into source-free flux-pair construction, pair averaging,
  and an acceptance solve that applies the source exactly once; the averaged
  pair obtains a fresh shared `alpha_H` rather than averaging stage alphas;
- a resident Metal stage-RHS evaluator publishes instantaneous buoyancy,
  Vreman-stress, phase-source, and combined momentum rates without multiplying
  by `Delta t`, committing its caller-owned command, or staging a full grid;
- the projection primitive can consume a sealed R0/R1 pressure-open class and
  a separately sealed integrated Bernoulli head, then derive the R2 endpoint
  class from corrected velocity while retaining the seed only inside the
  explicit deadband; and
- every added caller-owned or Private buffer is included in the re-derived
  two-GiB certificates.  The focused projection and solver suites, the staged
  Metal FCT selector, strict mirror generation check, and exact resource/source
  guards pass.

The first conservation prerequisite beyond those seams is also present, but
only at the authority it has earned.  A standalone CPU producer builds the
separately retained physical nonadvective mass/energy tuple `f_N` and gas
diffusion subflux `J_g` from fixed binary32 `Q,T,D,k,u`.  It evaluates the
certified `N_C N_C^T` mass projection in binary64 against the record's
independent forward-error envelope, then records a separate binary32
publication bound and recomputes `J_g` from the published component bytes.
Periodic seams are byte-canonical; walls are exact zero; pressure-open inflow
uses the sealed class and half-cell ambient gradient while sealed outflow is
zero-normal-gradient.  The exact standalone peak `4(12C+11F)+B` is denied
before payload access above two GiB.  Temperatures are restricted to the
PhysicalV1 record's inclusive `[300,5000] K` domain.  Metal deliberately fails
closed for this producer: its fp64 identity and publication contract has not
yet been ported.

These are bootstrap seams, not the completed operator.  There is no ordinary
production projected-Heun activation in r183, no coupled R0/R1/R2 Picard/cycle
owner, no `Phi_g` retention/FCT composition, no packet-derived `S_div` or
accepted-state EOS inversion seam, no caller-command projection/FCT encoder,
and no stage-averaged momentum commit yet.  Consequently the r182 onset
criterion is not re-run, r170 and readmission are not re-derived, and tier-10
full-window, spectrum, empirical rows, animation, and report remain blocked.
The next admissible implementation steps are `Phi_g` retention and composition,
then packet/`S_div`, EOS, and caller-command seams; only after those are green
can the complete stage schedule be wired and compared kernel-by-kernel with the
oracle.  Promoting the single-stage diagnostic would misrepresent a time
integrator as §3.7 conformance.

### 7.56q Authenticated accepted-state EOS prerequisite (r184)

r184 closes the CPU acceptance seam for the two conservative states that the
projected-Heun tableau must invert separately: `Q*` and `Q^(n+1)`.  The request
cannot author a temperature ceiling.  It carries the canonical fire-case-v1
envelope, which is validated against PhysicalV1 before any conservative-grid
payload is inspected; the case record supplies the exact `300 K` lower bound,
strict `2300 K` maximum-accepted-temperature ceiling, and case SHA.  Stage role,
represented timestep, attempt identity, shape bytes, fuel/case identities,
derived bounds, exact conservative bytes, published binary32 temperatures, and
the monitored pressure-deviation maximum all enter the result identity.  A
Q-star seal therefore cannot be transplanted to the final Heun state or to a
stale attempt with the same topology.

The record now exposes one allocation-free signed-mixture inversion over
explicit authenticated bounds and one canonical pressure-ratio evaluation at
the published temperature.  The latter deliberately preserves the oracle's
gas-density, molar-density, mean-molecular-weight, represented-pressure
operation order rather than using an algebraically equivalent rescaling.  The
r60 affine envelope remains the admissibility predicate: envelope-negative
stored species participate in sensible energy unchanged, while positive-part
gas densities participate in the ideal-gas diagnostic.

Fresh review caught and rejected an initial draft that treated `1e-3` as a
production pressure gate.  That number is the binary64 oracle's validity
detector, as corrected in r159; it is not a production thermochemistry-domain
property.  Production now records the P0 deviation without stalling on it.  A
uniform `1.01` density/energy scaling is consequently accepted by this
inversion prerequisite and reports its approximately one-percent deviation;
the independently owned low-Mach/manifold policy remains the authority for
fidelity classification.  The strict case temperature ceiling and r60/EOS
physical evaluability still fail atomically.

The exact CPU logical payload is `40C` bytes (nine binary32 conservative
components plus one published temperature), with the adjacent two-GiB shapes
gated before conservative payload access.  The fp64 and roundoff manifests now
also bind the live FireSimulationRecords and FireCase dependencies used by
their generated Transport mirrors.  This remains a CPU-only prerequisite: no
Metal resident inversion, caller-command owner, `Phi_g/J_g` Heun composition,
packet-derived `S_div`, R0/R1/R2 schedule, onset claim, or tier-10 deliverable
is enabled by r184.

### 7.56r Exact 30F scalar-flux composition prerequisite (r185)

r185 closes the CPU composition seam between the source-free donor/MC pair and
the independently certified physical flux producer. A stage is built only
from the raw same-stage operands: `Q`, frozen velocity, boundary/open class,
ambient tuple, and topology must be byte-identical between the advective and
physical requests. The composer invokes both producers itself; it never
accepts a mutable prerequisite result or trusts a caller-set qualification
boolean. Attempt identity and the distinct R0/R1 role bind the raw input
digest, retained content digest, record identity, frozen-velocity digest, and
a shared FCT-contract digest covering the source dose, nullspace/projector,
enthalpy bounds, feasibility/reserve factors, ambient tuple, and open classes.
R0 and R1 cannot be averaged unless that shared contract is identical.

The retained binary32 payload is exactly `30F` for `F` packed primal faces:
`9F` composite low flux, `9F` advective antidiffusive delta, `8F` physical
mass flux, `F` physical energy flux, `F` `J_g`, `F` advective `Phi_g^L`, and
`F` advective `Delta Phi_g`. Composition publishes `f_N` first and adds it
once to the eight mass low-flux components; physical energy is added once to
the energy low flux. The antidiffusive tuple remains advective-only. Gas low
and delta are summed independently over components 1--6 in ascending order,
and `J_g` is recomputed from the retained physical component bytes in the same
order. Every consume rechecks the physical affine residual against the
producer's independently derived publication bound. The averaged stage derives
a new bound that adds each input bound, the measured binary32 publication
rounding, and the binary64 row-reduction error; its averaged `J_g` has a
separate operation-count forward bound. Both are re-evaluated before consume.
Periodic seams are bit-canonical across all 30 fields.

R0 and R1 fields are averaged independently as `0.5*(R0+R1)`, producing a
Heun-average role with no limiter state and explicit parent composition IDs.
A solve-from-average-stage API validates the R0 request identity, shared
contract, attempt, parents, averaged content, and physical certificates before
performing one fresh shared-alpha solve. The resulting alpha token binds its
bytes to the attempt, averaged stage, and both parents; the legacy pair-only
diagnostic cannot mint that token. Three live
stages use exactly `360F` bytes; the `58x80x424` / `25x140x559` neighbors bind
the average's two-GiB admission boundary before payload access. Composition's
live face set is `58F`; its adjacent over-cap shape likewise refuses before
raw payload inspection.

The compatible momentum seam gained a CPU-only direct-delta entry point. It
evaluates `Phi_g^L + alpha*Delta Phi_g + J_g` from the retained fields and
then applies the existing boundary-aware `I_i` operator. The legacy diagnostic
continues to accept low/high inputs, but r185 never creates `high=low+delta`
and subtracts it again. A cancellation RED records a one-bit difference
between the retained delta and that reconstruction. The consumer accepts only
the fresh-alpha token whose parent identifies the selected R0/R1 stage; a
caller-provided or mutated alpha is refused. The final shared alpha is
applied separately to each original R0/R1 stage with that stage's own velocity
and `J_g`; the later owner must average their momentum rates rather than use an
averaged flux with an averaged velocity. The current compatible kernel has an
oracle only for all-periodic or all-nonperiodic topology and fails closed on a
hybrid periodic/nonperiodic domain.

This is still a granular CPU prerequisite, not the projected-Heun owner. No
Metal composed-stage encoder, packet-derived `S_div`, coupled R0/R1/R2 cycle,
state commit, onset verdict, or tier-10 deliverable is enabled. Those remain
blocked behind packet/source composition and caller-command resident encoders.

### 7.56s Canonical frozen-source authority prerequisite (r186)

r186 closes the CPU source-map ownership seam needed before packet-derived
`S_div` can be composed.  The public request contains only authenticated raw
inputs: shape, `Delta t`, beginning time and attempt, the canonical fire-case
envelope, beginning component-major binary32 `Q`, the controller's pilot mask,
per-cell mixing time, predictive-radiation policy, and a topology-bounded
worker count.  It cannot supply a conservative source dose, pilot energy or
expansion pair, reaction diagnostics, radiation factor, or a qualification
bit.

One implementation now owns the source arithmetic.  The previously
tool-local cell state, ignition/reaction, exact pilot projection, global
radiation, backward-Euler cooling, ledger, and finite-expansion kernels moved
unchanged to `FireProductionSourceKernel.h` under `src/Library`; the offline
tool includes that shared implementation and no longer defines a second grid
producer.  The only seal-minting `Build` member is non-inline in the compiled
`FireProductionSource.cpp`.  This corrects a rejected draft whose friend-capable
definition lived only in the tool header and could therefore be replaced by a
client definition.

The authority validates PhysicalV1, OpenV1, HITEMPPlanckMeanV1, and the case
record references before producing anything.  Temperature comes from the
same r184 record inversion over the authenticated `[300,2300] K` interval:
the lower endpoint is inclusive and may publish a certified below-endpoint
roundoff state as `300 K`, while publication at the `2300 K` ceiling refuses.
It derives ignition connectivity and the continuous pilot command, including
the exact `(E(T')-E(T0))/V'` energy and `1-1/V'` expansion pair; applies
reaction headroom, the single grid radiation escape factor, cooling, source
ledgers, and expansion admission; then publishes an opaque `9C` resident dose
plus eight diagnostic fields.

Beginning-state, reaction-control, raw-source-input, global-radiation, packet-
content, and final packet identities bind all retained bytes and record
parents.  Scheduling is deliberately not physical identity: one- and
multi-worker builds are byte-identical.  Worker creation is topology-bounded,
partial growth rolls back atomically, task exceptions cross the completed
barrier, and the pool remains reusable.  Because the process-global pool keeps
its high-water worker set, every admission charges a conservative 8 MiB stack
reservation for the complete hardware-concurrency capacity, not merely the
current request.  It also charges a one-MiB bounded case envelope/record
scratch and every simultaneously live per-cell field.  Exact adjacent-shape
and high-worker-then-low-worker tests bind that formula; oversized shape,
envelope, and worker requests refuse before payload parsing or allocation.

This is a compiled CPU prerequisite only.  r186 does not yet compose
packet-derived `S_div`, encode Metal source commands, wire the R0/R1/R2 owner,
rerun onset, or authorize any tier-10 spectrum, row, animation, or report.

### 7.56t Packet-derived divergence-target publication (r187)

r187 removes one more caller-authored seam from the future projected-Heun
owner.  The canonical source authority already evaluated each packet's finite
volume change while enforcing its expansion bound.  It now publishes that
same value as one component-major binary32 cell field,
`S_div=(Delta t*S_thermo+I_pilot)/Delta t`, alongside the `9C` conservative
dose.  `S_thermo` is evaluated from the represented packet with pilot energy
removed exactly once; `I_pilot=1-1/V'` remains the separately derived exact
pilot expansion integral.  No second thermochemistry or EOS path is introduced.

The target is part of the opaque packet-content and final-packet identities.
Seal validation requires exactly `C` finite canonical values, including
positive zero.  The RED reconstructs each retained packet from the sealed
binary32 dose plus its pilot pair, reruns the finite-increment expansion
identity, and requires bit equality with the published target.  Working-set
admission correspondingly changes from a sealed `9F+8D` result to `10F+8D`,
and the exact adjacent/high-to-low worker certificate is re-derived.

This remains a CPU publication prerequisite.  It does not yet encode a Metal
source command, bind the target to R0/R1 projection commands, commit a
projected-Heun step, rerun onset, or authorize tier-10 deliverables.

### 7.56u Authenticated base physical divergence target (r188)

r188 composes the first non-projectable R0/R1 target from the two authorities
that own its physics: the r186 frozen source packet and the r185 same-stage
retained nonadvective flux. The implementation rebuilds the 30-face-field
stage from its raw authenticated operands rather than accepting a mutable
physical-flux result. It then implements the pinned source/physics split
cell-by-cell and in component order:

`S_div^stage = dV(Q_stage)[D(f_N^stage)] + S_div^source(Q_n, Delta Q_source)`.

The first term is the exact PhysicalV1 tangent evaluated from the stage's
physical nonadvective rate. The second is the beginning-referenced absolute
finite-source target published by r187 and is byte-identical for R0 and R1.
In particular, a zero source dose still yields `(V(Q_n)-1)/Delta t`; it does
not erase absolute restoration. A reviewed draft that inserted `D(f_N)` into
the nonlinear finite-source map was rejected because it changed the pinned
tableau and made the source contribution stage-dependent.

R0 accepts only the exact beginning-temperature bytes retained by the
canonical source authority and recomputes that authority's beginning-state
identity from the stage Q/T bytes. The retained temperature vector is moved
from the source producer's already-live inversion buffer. The retained result
becomes `11F+8D`; the exact source-build peak therefore advances from r187's
`19F` certificate to `20F`, without a second temperature allocation.
R1 has a distinct role identity and
independently inverts its current stage through the shared accepted-state
record operation; the later coupled owner must still supply Q-star accepted-
candidate lineage before an R1 target can become terminal.

The opaque target binds exact shape and `Delta t`, attempt and role, methane
record, source-packet identity, rebuilt flux-composition identity, every
binary32 target byte, and the maximum absolute scaled expansion. It has no
projection-consumer overload. Its live CPU peak is the retained `30F` stage
plus one `C` canonical-temperature buffer and one `C` target buffer; the
adjacent over-cap RED refuses before source or stage payload access.

The independent fixture reconstructs `D(f_N)`, evaluates the record-owned EOS
tangent, adds the frozen r187 target, and requires target bit identity. A
distinct accepted R1 stage proves only the physical tangent changes while the
source packet identity remains fixed. A zero-dose nonmanifold fixture proves
the absolute-source branch, while a nonuniform thermodynamic field proves at
least one ordinary output differs from the source-only field. One-ULP stale
source and temperature parents refuse in production and the traced scalar
comparison distinguishes the stale temperature.
The r70 accepted-candidate correction, terminal verification, projection
command, Metal implementation, complete R0/R1/R2 owner, onset rerun, and all
tier-10 deliverables remain blocked.

### 7.56v Authenticated projection-target chain and Metal-host manifest (r189)

r189 closes the initial CPU projection-consumer prerequisite without promoting
a single projection to accepted-step authority.  The r188 base seal now retains
the exact boundary topology of the stage whose physical flux produced it and
includes all six boundary classes in its identity.  The base publication may
enter projection only after it is wrapped as iteration zero of an opaque target
chain; the wrapper inherits topology and accepts no caller boundary argument.
This closes the valid-but-different topology hole found in review.

The new CPU projection consumer takes its request by value, requires the raw
`divergenceTargetPerS` slot to be empty, verifies the complete seal and exact
shape/step/boundary match, and only then fills the slot and invokes the existing
projection oracle.  A matched-input comparison is bit-identical in pressure,
velocity, momentum, and residual diagnostics.  A preauthored target on this
capability, a valid but different topology, and an unsealed base parent all
refuse with an empty result. The older raw projection function remains public
as a calibration oracle; it is not an accepted-step capability, so r189 makes
no repository-wide claim that arbitrary callers cannot invoke it.

Review rejected an attempted standalone r70 child-target producer. An r184 EOS
result proves that a state is admissible, but does not prove that it was created
by the immediately preceding projection/flux iteration. Permitting it here
would admit a stale, internally valid Picard candidate. Therefore r70
correction, closed-nullspace compatibility (no pressure-open face, not merely
all-periodic), terminal verification, active-set acceptance, R2 endpoint
authority, and accepted-step publication all remain owned by the complete
R0/R1/R2 owner, which is the next host-independent rung. The Metal projection
consumer also cannot be live-validated on this host because no Metal device is
available.

The separately SHA-bound r189 Metal-host manifest records that no device run
occurred here and makes Track A bindingly first when the M4 worker returns:
tier 8 from zero through the full window, full spectrum, preview-primary
animation and immediate delivery.  Only then do the device kernel sweep and
tier-10 onset run, followed by regression re-derivation and the tier-10 queue.
All simulations and renders are sequential.  The animation stage also carries
the HDR container check (`nclc` primaries 9, transfer 16, matrix 9) rather than
trusting codec tags alone.  The manifest SHA is
`c9c48a6eb64d195235f59bb73783296f54f61507f2fb6271280da07d07433f5c`.

### 7.56w Complete CPU projected-Heun owner (r190)

r190 closes the host-independent R0/R1/R2 ownership rung after a fresh review
rejected its first draft. The central r70 correction remains private to
`FireProductionProjectedHeunCPUOwner`. Its candidate identity is recomputed
from the exact attempt, stage, parent, flux, alpha, and candidate bytes at the
correction boundary. Closed domains remove the constant mode from the complete
target, including iteration zero, whenever no pressure-open face exists.

The owner is a strict protocol state machine. Each stage enters an explicit
in-progress state before any transport callback; a scoped rollback restores the
last stable stage on refusal, so a provider cannot reenter R0/R1/R2 and mint a
nested publication. `Begin` binds the canonical case
envelope to the frozen source case, preflights the simultaneous owner live set,
and accepts only empty dynamic slots in the invariant contracts. `SolveR0` and
`SolveR1` perform projection, coefficient publication, flux composition,
scalar acceptance, r70 correction, and coupled convergence in order. Every
projection must pass validation. Coefficient identity binds the exact projected
state, temperature, velocity, and all payload arrays, so stale bytes with
current metadata refuse. Both EOS publications must retain the source case.

Terminal R0/R1 acceptance reprojects the corrected target, verifies target,
momentum, and coefficient convergence, and applies r59's pointwise
`min(alpha_next,alpha_ver)` for a discontinuous limiter class. The selected
alpha is run back through the complete r60 accepted-state validation and the
compatible `D_i I_i = I_rho,i D` witness before the identity-bearing Heun
solve may consume it. That selected-alpha validation is owner-local. r190 does
extend the r189 transport target payload: it adds the authenticated
parent/candidate/correction chain and the compatible-momentum delta consumed by
the complete owner. This is recorded as a source change, not disguised as
byte identity; none of those additions can publish standalone accepted-step
authority. Open-face class history stores the class actually used by each
projection. On a cycle, every member is reprojected against the same current
target with its class frozen, and every subsequently projected or derived class
is added to the proved set before terminal selection. Only then are branches compared with the configured
velocity deadband and a lexicographic tie break. A fresh numerical review found
that this canonical choice had originally been made against a transient target.
The corrected owner treats a first terminal class change as another Picard
iteration; if the terminal classes cycle, it reprojects every proved member
against the corrected accepted target and chooses only from those results. R2
does the same and performs its terminal projection before rebuilding endpoint
transport, physical flux, and target. Each accepted stage publishes two named,
identity-bearing seals: `projectionTarget` is the exact input consumed by the
published projection, while `target` is the Picard-map output minted from that
projection's authenticated flux/candidate and is the only seal that binds
`acceptedCandidateIdentity`. This makes the tolerance-level residual explicit
without mislabelling it as bitwise fixed-point equality. Thus the published
class is selected against the exact projection target, not a winner frozen from
an earlier target. The
owner records cycle length,
differing-face count, canonical reprojection count, and the maximum discrepancy
over the full trajectory. The shared Heun
contract excludes stage-local pressure-open classes, while each stage request
identity binds its own class.

`SolveR2` fixes `Q^{n+1}` and `M^{n+1,dagger}` and iterates
`u2 -> f_N,2 -> S_div,2 -> projection` to convergence. The Heun-averaged
physical flux is not an endpoint target. The integrated head is exactly
`-rho_ambient (I0 |u0|^2 + I1 |u1|^2)/4`, including tangential velocity. R2
performs no scalar commit and publishes endpoint momentum, velocity, and
step-average pressure atomically.

Transport callbacks receive private deep snapshots of state, temperature, and
velocity rather than aliases into owner storage. Exact-bit postconditions run
after both success and refusal over every context scalar and pointer as well as
all pointed-to bytes. Publication identity is recomputed from a fresh owner-local
context, so a provider that casts away const, repoints a context member, or
mutates payload cannot affect retry state or mint an identity over an
unprojected velocity. Those `10C+F` snapshot words and the two-seal publication
copies are included in the exact working-set certificate, now
`(274C+97F) sizeof(float)`.

REDs cover out-of-order stages, stale r70 candidate identity, stale coefficient
payload with current echoes, forged parent, closed constant-mode compatibility,
stale whole-result replay against a different sealed source attempt, result
mutation, the actual `Begin` combined-live-set refusal, every initial/iterative/
terminal/R2 bootstrap/endpoint projection-validation branch, validation of
every selected-cycle reprojection in both the coupled R0/R1 path and R2, an
exercised two-class active cycle with
distinguishable discrepancies, an exercised nonuniform r59 selected-alpha path,
terminal first-class transitions and terminal canonical-winner changes in R0,
R1, and R2, an R2 terminal-validation refusal, all 60 combinations of callback
stage, scalar/pointer/payload operand, and success/refusal mutation, and retry after an
injected mid-publication allocation failure. The caller's
result remains the complete default publication until a separate deep copy has
finished; committing the internal and external publications then uses
compile-time-proven non-throwing moves. A callback that retains the caller's
result reference, overwrites it with a stale valid publication, and refuses is
also rolled back recursively to the complete default before a clean retry. The final verifier
binds every public owner payload: conservative state, final and stage momentum,
velocity and pressure, both EOS publications, fluxes, limiter state,
nonpressure rates, targets, diagnostics, all nested shapes/timesteps/boundaries,
and the exact frozen-source packet identity. Pressure, stage-velocity, alpha,
EOS-temperature, flux-metadata, physical-flux-shape, and EOS-shape mutation
REDs prove that the mutable diagnostic struct has no acceptance power without
the verifier. The acceptance seal itself is private and owner-minted with a
read-only getter, so source-visible hashing cannot be used to re-sign mutated
public bytes. A re-signing RED copies a valid result, changes its state, and
proves both that the seal is not publicly writable and that verification
refuses. A consumer must supply the current sealed source packet; an
internally intact publication from another attempt refuses. The working-set
RED both measures a valid 1024-cubed shape above two GiB and injects that live
count through `Begin`, proving the admission branch rather than only its query.

A source-active pressure-open `4^3` matched-input differential reconstructs the
exact canonical one-ULP pilot packet at the oracle-convergent `1e-4 s` schedule
and uses oracle transport evaluations, then compares every projected stage to
the independently implemented fp64 `AdvanceConservative3D` owner. State,
velocity, and momentum use the measured 64-epsilon binary32 forward envelope.
Pressure is compared to separately converged oracle schedules at tolerance
ratios `1`, `1/16`, and `1/256`; the coarse-to-fine difference is
`2.988941126e-4 Pa`, the fine-to-finer tail is `4.302731804e-5 Pa` (ratio
`0.144`), and production differs from the finer reference by
`3.396462939e-4 Pa`. The comparison gate is the one-bit-headroom dyadic
envelope `2^-10 Pa = 9.765625e-4 Pa`, derived by rounding that measured
converged differential upward and then adding one binary headroom bit. It is a
fixture-specific measured upper bound, not the rejected local
`2 dx^2 epsilon/dt` pressure formula. A second, nonuniform periodic fixture
compares complete generated fp32/fp64 owner publications: stage targets,
projections, flux pairs, nonpressure rates, and shared alpha. The signed r60
gas-density differential also proves that a rounding-scale negative constituent
is included rather than clamped. The generated binary64 mirror separately
bounds roundoff over state, targets, momentum, velocity, pressure, and alpha.
The tier publisher is also tested through its serialized evidence path for
tiers 8 and 10, and the Metal manifest parser validates the complete HDR encode,
container probe, and SHA sidecar independently inside each movie stage rather
than accepting global substrings from another stage. No Metal run is
claimed. The r136 diagnostic's source-bound trace digest is re-derived from
historical `19371e6ef60fb1c78e6feeb0616b5952993ee375a7b9f7d97afd16b544182326`
to `7736ec4adb7fd3b0cb3c1bf7bddd5b1d2a050e7e0fbd56bc31e928b7f9faa22d`.
The digest encoding prefixes all 16 generated manifest fields: every primary
operator, `FireSimulationRecords` and `FireCase` header/source dependency, and
the three trace-support files plus the generator. A shared enumerator is used
by the diagnostic and a per-field mutation RED proves that every field changes
the identity. A detached build at `0e70f17f1` and the current
build both return the exact diagnostic success code 237; all 3,972,326 branch
obligations, frozen metrics, and the `0xff` a-priori refusal remain unchanged.
The SHA-bound `r136_trace_repin_evidence.v1` records both runs instead of
rewriting the historical record. R2 complementarity is now scored against the
classification actually supplied to the bootstrap and iterative projections
rather than either newly derived endpoint class; separate forced class-flip
REDs distinguish both choices.
The caller-reset RED compares every public field recursively with a fresh
default result, including endpoint physical flux, projection diagnostics,
nonpressure rates, all FCT payloads, EOS metadata, active-set counters, target
lineage, and the private acceptance seal. The deferred Metal manifest also
passes tier 8 or 10 explicitly to the spectrum publisher and checks it against
the checkpoint's canonical grid metadata. Its tier-8-first and tier-10 movie
stages now name the complete headless HDR pipeline: primary linear EXRs enter
`encode_pq_prores.py`, FFmpeg writes ProRes 4444 with `setparams` plus
`+write_colr`, `ffprobe` must report `nclc: pri 9 trc 16 matrix 9`, and a SHA
sidecar binds the movie to all primary-frame provenance sidecars. These remain
deferred commands; no device result is inferred from their host-independent
validation.
The fp32/fp64 owner comparison also includes every R2 endpoint physical mass,
energy, and gas flux plus all certificate fields and metadata. When the worker returns,
r189 still runs tier 8 first and sends its movie before the device sweep and
tier-10 onset verdict.

### 7.56x Metal execution-context reclassification (r191)

r191 corrects the deferred-host premise without changing the sealed r190
manifest. `system_profiler` identifies the current machine as an integrated
40-core Apple M4 Max with Metal support. More decisively, one probe calling
both `MTLCreateSystemDefaultDevice()` and `MTLCopyAllDevices()` was run in two
contexts. The workspace sandbox returned a nil default device and an empty
enumeration; the identical binary outside that sandbox returned one Apple M4
Max, registry ID `4294969469`, through both APIs. The earlier result therefore
measured a blocked execution context, not absent hardware.

The production capability seam now records device discovery independently
from kernel validation. Its explicit states are `available`,
`blocked-by-execution-context`, `no-device`, `backend-not-built`, and
`unqueried`. It consults both discovery APIs, but authorizes execution only
when the same default-device API used by every production consumer succeeds.
A nil default with a nonempty enumeration is therefore context-blocked, not an
advertised fallback. On Apple silicon, an empty result from both APIs is also
classified as execution-context denial because the integrated GPU cannot be
absent; no-device remains available for hosts on which hardware absence is
meaningful. `available` still requires the safe-math identity
kernel to compile, dispatch, complete, and return exact bytes, so enumeration
alone grants no production authority.

A capability-only test mode makes this distinction the manifest preflight. In
the workspace sandbox it returns exit 86 with
`discovery=blocked-by-execution-context`; unrestricted it returns exit 0 with
`discovery=available`, `default_present=1`, `enumerated_count=1`, and an exact
identity-kernel pass on Apple M4 Max. The corrected manifest v3 preserves the
r190 stage definitions and binding order—tier 8 remains first—while requiring
an unrestricted execution context. Manifest SHA:
`109a18a5e7803679bf35acf5adb3b34d89fb3548a549889f043414937d1e2763`.

Fresh evidence review rejected the original stage-2 wording because it compared
device fields only with fp32 publications. The corrected sweep now stages the
private compatible-momentum resident kernel through a test-only terminal
comparator and runs two matched-input fixtures: fully periodic and mixed
wall/pressure-open. Device output must be bit-identical to the fp32 CPU
authority, while the same fields are compared directly with the mechanically
generated fp64 mirror. The initial sweep exposed contraction-induced one-bit
device/CPU differences; the compatible kernel now disables floating-point
contraction only inside that kernel body, matching the pinned CPU expression
topology without changing any other Metal kernel. The bootstrap compares every
donor, MC-MUSCL, averaged-pair, and fresh-shared-alpha device publication
directly with the generated fp64 stage. The dyadic periodic fixture obtains
zero mismatches after exact binary32 rounding. A second non-dyadic mixed
wall/pressure-open fixture is byte-exact against fp32 and differs from fp64 by
at most `2.1297667185393721e-7`, inside its input-conditioned flux/state bound
`4.1114563049705465e-5`. Ratios and shared α are not admitted under that
arithmetic bound: their fp32/fp64 unit-interval classes are checked directly.
The fixture exercises 18 interior ratios, 21 interior α values, and four
boundary-adjacent limited faces; the minimum class margin is
`0.10898987999238696` against maximum observed difference
`0.00039498558320727462`. Compatible output is byte-compared rather than
value-compared, with a signed-zero RED. Its fp64 difference is evaluated in
double and checked both against the original relative `gamma128` gate and an
absolute forward bound derived from `rho_min-10D/h` and the five-pass
momentum-magnitude recurrence. The development-qualification sweep reports
fp64 maximum absolute difference `1.1175870895385742e-7`, maximum normalized
difference `2.2941710525697736e-7`, and derived absolute bound
`3.1200230559651367e-5`. This run is explicitly pre-manifest qualification,
not Stage 2: the same sealed command reruns formally only after tier 8.

### 7.56y Tier-8 live-trajectory rejection (r192)

The corrected Metal-host manifest was started unrestricted on the verified
Apple M4 Max, with tier 8 first and no concurrent render. Its retained
trajectory is healthy through the early pilot, then develops an intermittent
vertical-momentum runaway after pilot shutoff. Retained samples report
`10.166 m/s` at `t=2.15824 s`, `19.835 m/s` at `2.38744 s`, and `68.786 m/s`
at `2.69935 s`, with respective CFL steps `1.508e-3 s`, `7.480e-4 s`, and
`2.392e-4 s`. Unsealed intermediate summaries include both a `22.880 m/s`
excursion and a rebound to `12.459 m/s`; monotone growth is not claimed. The
retained step-1873 checkpoint is the durable rejection authority. A manually
curated, unsealed operator observation records a later step-2048 value of
`1659.15625 m/s` and `9.615e-6 s`; it is context, not a gate claim.

At the last retained checkpoint, step 1873, the monitored manifold maximum is
`0.15253`, below the unchanged `2^-2` physicality bound; this proves the
accepted state did not violate the hard bound, not that tail restoration was
irrelevant. The momentum/velocity compatibility residual is `1.8463e-6`. A
newly reconstructed source probe has HRR/consumption-times-LHV internal
consistency of `1.3215e-7` relative; it is not the persisted accepted-step
source ledger. Thus the evidence excludes only a gross compatibility-identity
break at that checkpoint and an inconsistency inside that reconstructed packet.
Accepted-step scalar application, tail-restoration coupling, advection,
force/projection ordering, and other momentum consumers remain in scope.

The run was interrupted after the physical failure. It did not reach the full
statistics window, so there is no spectrum, animation, or tier-8 empirical
claim. Because Stage 1 is a binding prerequisite in the v3 manifest, the
formal device sweep and tier-10 stages remain unexecuted. This distinguishes
the earlier matched-input development qualification from live-trajectory
acceptance: component byte agreement cannot overrule a failed integrated
trajectory. The next authorized instrument is an onset budget from the last
healthy retained state through the first 15/30/60 m/s crossings, with the
ported and prior momentum paths compared at the same inputs. It must capture
per-term momentum rates, restoration, Vreman value, the exact velocity-owning
face state, momentum/scalar compatibility, the persisted accepted-step source
ledger, and pre/post `(rho,T,species,deviation)` for both adjacent cells. No
bound, tolerance, or timestep rule changes on this evidence alone.

Evidence is sealed under
`rendered/fire_production_calibration/r192_tier8_runaway/`. The golden
checkpoint remains untouched.

### 7.56z Matched-replay operator-identity stop (r193)

r193 first corrects the tier-8 stability premise: r181 observed tier 8 only to
`1.8264868 s`, while r192's runaway begins later.  Calling that prefix stable
beyond its measured horizon was an extrapolation error.

The requested matched replay is refused before measurement because its second
leg does not exist as a live production entry point.  The completed
authenticated R0/R1/R2 projected-Heun owner is a CPU authority.  Source
enumeration finds its declarations and implementation, but no production or
`FireSequenceTest` caller.  The available Metal alternatives are not aliases
for it: `AttemptFireProductionCompatibleMomentumDiagnosticMetal` only scopes
the r182 shared-alpha flag around the ordinary resident owner, and r183's
tokenless single-stage FCT route has the wrong tableau.  Both were already
rejected as production acceptance paths.  Running either and labelling it the
ported leg would fabricate the three-way verdict and could improperly
authorize integration.

Fresh independent operator, numerical, and provenance reviews all classified
that substitution as P1.  They also caught evidence-protocol defects in the
discarded draft: resume could be labelled from-zero, nonempty directories
could mix appended thresholds, a crossing-local moving-column rate was
misnamed as the fixed matched-state observable, skipped thresholds were cloned
without a shared-event identity, vertical-only cell indexing was not guarded,
and global packet totals did not preserve the source field's spatial identity.
No such mode or measurement is landed.

The replay unblocks only after an identity-bearing live Metal entry point owns
the complete R0/R1/R2 schedule and binds its accepted result to checkpoint
publication.  The pair protocol must then seal the exact tier-8, `3.0 s`, seed,
case, initial-state, executable, and preregistration identities before either
run; refuse resume and nonempty output roots; publish axis-general 15/30/60
crossing budgets plus a separately named exact-equal-time oracle-composed
fixed-face or fixed-column observable whose spatial selection and match rule
are sealed before execution; and bind the complete binary32 packet field with
the adjacent-cell packet values.  The historical
`63.339610872283018 kg/(m2 s2)` value is a velocity-owning-face sample at
oracle time `2.1135824235796083 s`, not an equal-time criterion.  The nearest
retained oracle fixed-column maximum is
`159.00892323854879 kg/(m2 s2)` at `2.1079791976176079 s`, compared with the
production column at `2.1077302111661993 s` (separation
`2.489864514086e-4 s` from the authored decimal timestamps); it too is not
exact equal-time composition.
Evidence is sealed in
`rendered/fire_production_calibration/r193_matched_tier8_replay/`; the stop
artifact SHA-256 is
`e7347d76d964b80d0cb2ebd7867ddea9afcf21afcda8a60f04a4c1cfc487912a`.

### 7.56aa Sealed tier-8 legacy replay (r194)

r194 executes the baseline half of the corrected split replay before the live
projected-Heun Metal owner exists. The run is from zero at tier 8 with the
ordinary independent dual-momentum resident operator, seed 1234, the full
binary32 source field, independent CFL selection, and a 3.0 s target. It stops
on the pre-registered 60 m/s onset gate at `2.6991133776609786 s` after 1872
accepted steps. The first 15, 30, and 60 m/s observations end at
`2.3182910486939363`, `2.6965437539038248`, and `2.6991133776609786 s`, with
realized maxima `15.0224609375`, `32.694915771484375`, and
`63.960872650146484 m/s`.

At the final crossing the aligned column advection rate is
`11691.86634461989 kg/(m2 s2)`, while buoyancy is
`9.538979544294932 kg/(m2 s2)`. The physical impulse maximum is
`5.3986992835998535 kg/(m2 s)` versus restoration's
`0.36649751663208008 kg/(m2 s)`, and Vreman spans
`3.1672589830128888e-4` to `1.935882493853569e-2 m2/s`. The old operator thus
reproduces the late advective-focusing runaway without the r183 integration
state. The three-way verdict is tier-spanning: the completed Section 3.7
operator must pass at tiers 8 and 10 before either statistics window resumes.
Tier 6 remains the interim movie authority.

This also records the r181 tier-8 health premise as an extrapolation error: its
sample ended at `1.8264868 s`, before this late onset. The baseline's separately
captured fixed-column candidate begins at `2.1081581366597675 s` and has
maximum aligned advection magnitude `302.19727231644055 kg/(m2 s2)`. It is not
an equal-time oracle composition and carries no 159-class verdict. Each
from-zero leg retains its own CFL schedule; the 159-class decision belongs only
to the separately sealed frozen-state equal-time probe.

Tracked evidence is
`rendered/fire_production_calibration/r194_matched_tier8_replay/`
`baseline_verdict_evidence.v1`, SHA-256
`d37e1aa85fb1e96b6ee69950c0df71ab92572a5c7427990c3c439c12efea8844`.
The authoritative raw summary SHA-256 is
`57f40959d60ea78c516fcd048b4f6a1dbcbd33ea132643bed25a675012ea749c`,
and the full velocity trajectory SHA-256 is
`49d8802f72735925ee406596628f613b56c3412e70e7a91659c79be40e811e80`.
Fresh numerical and sealing re-reviews report zero P1/P2. The golden checkpoint
is unchanged.

### 7.56ab Live Metal owner prerequisite audit (r195)

r195 starts the requested live-owner rung only after the r194 baseline proves
that the late focusing defect spans tiers. Source enumeration then rejects the
premise that only an entry-point wrapper is missing. The physical scalar-flux
Metal function is a literal fail-closed stub; EOS acceptance can be minted only
from host vectors by the CPU authority; the projected-Heun transport-provider
contract accepts host vector pointers; and the r70 target authority friends
only the CPU owner. The resident FCT functions are explicitly diagnostic and
there is no complete live owner caller in `FireSequenceTest`.

A provisional physical-flux/EOS device experiment was run only as development
work and rejected by fresh review before commit. Its physical comparison bound
scaled by a possibly cancelled output and did not cover coefficient
quantization or the transcendental term. Its EOS path compared a device result
and then returned the CPU-minted acceptance identity, so CPU substitution could
pass. Neither may be promoted as production. The required next contract is a
resident candidate lineage spanning transport coefficients, physical flux,
EOS, and the r70 target chain, with exact binary32 operator mirrors, a separate
fp64 physics certificate, private interstage fields, and a complete owner peak
working-set certificate.

The ported replay is therefore not executed or claimed. This is an
owner-authority stop rather than a numerical failure of the port, and no
diagnostic wrapper, CPU fallback, tolerance change, or shortened run is used.
The audit is sealed in
`rendered/fire_production_calibration/r195_live_metal_owner_prerequisite_audit/`
`live_owner_prerequisite_audit.v1`, SHA-256
`88f79c08beed9b70e43b2655d5f8b7f9ea05154abe5e31cd2f5941d8f9eb9f0d`.
The golden checkpoint and dirty main checkout remain untouched.

### 7.56ac Resident candidate-lineage contract (r196)

r196 accepts the r195 owner stop and authorizes a provenance extension, never
a relaxation. The live Metal path may consume only device-produced resident
authority surfaces. A CPU-computed value cannot mint, copy, or reconstruct the
private lineage carried by a resident candidate; the checkpoint-authority
pattern established by r148/r164 is binding, and CPU substitution is a standing
RED at every new surface.

Every surface has two independent numerical obligations. Streaming arithmetic
is compared bit-for-bit against its strict binary32 mirror. The physical
certificate is evaluated by the fp64 mirror at matched inputs, using bounds
derived from uncancelled terms. A quantity made small by subtractive
cancellation cannot scale its own error bound. Kernels with arithmetic branches
must enumerate their walker paths and discharge an obligation for each path.

The resident interval permits zero full-grid interstage transfers. Each rung
must add a transfer RED and extend the simultaneous working-set certificate.
Changes to case-authored semantics or inputs regenerate `case_record_id`;
solver-implementation changes instead regenerate producer/run identity, per
the canonical Section 6 distinction. This contract-only commit changes
neither. The ordered, separately committed rungs are: device-resident
transport coefficients, physical-flux authority, EOS candidate identity,
authenticated target lineage through r70, the complete R0/R1/R2 Metal owner,
the device/fp64 kernel sweep, and the sealed tier-8 ported replay. The replay
inherits r194's seed and exact-equal-time observables and uses the corrected
`159.01` aligned-advection class before the three-way verdict.

The binding contract is sealed in
`rendered/fire_production_calibration/r196_resident_candidate_lineage_contract/`
`resident_candidate_lineage_contract.v1`, SHA-256
`594cd3978d22bec423c7e8132b7d85bbe99cb97e67bbb0add51620560fb40253`.
The tier-6 movie remains the owner's interim deliverable; the golden checkpoint
is untouched.

### 7.56ad Resident transport authority (r197)

r197 completes only the first r196 resident-lineage rung. The live authority is
created from device-private conservative state, temperature, projected face
velocities, and a fixed per-face physical fuel-inlet classification. Its type
and constructor are private to the Metal implementation. The public comparator
is a qualification-only terminal tap: it uploads a matched fixture, invokes
the same private issuer, and can return staged coefficient fields and the
device publication identity, but its result is deliberately not convertible
to the accepted owner transport-coefficient surface. There is no CPU issuer or
fallback. The fixed fuel-inlet mask is distinct from the pressure-open
boundary's dynamic inflow/outflow active class and is identity-bearing.

The authority evaluates density, projected-velocity gradients, Vreman SGS,
NASA9 heat capacity, PCHIP viscosity/conductivity, Wilke/WMS mixture
properties, total diffusivity, effective conductivity, and molecular
kinematic viscosity on Metal. The immutable table packs 109 transport knots
inside a forced maximum of 128 and six gas species; solid carbon is not a gas
transport species. Device identity covers all state and output fields, the
immutable NASA9/transport records, shape and spacing, boundary/fuel-inlet
classes, constants, and mutable stage, attempt, parent-candidate, and
projection lineage. A failed validation publishes neither coefficients nor an
identity.

Transport evaluation is classified as a non-streaming physical operator, so
the binding numerical certificate is the canonical fp64 mirror at matched
inputs plus a termwise binary32 forward-error enclosure. The host binary32 DAG
comparison is retained as a diagnostic and is not bit-identical; no streaming
bit-comparison obligation is waived. The enclosure propagates record rounding
and every add, subtract, multiply, divide, square root, absolute value, and
maximum. Vreman cancellation is bounded by the uncancelled sum of absolute
terms, never by the cancelled result, and any nonfinite bound fails closed.
An independent branch walker matches the device bitmap `0x0007fffb`, including
PCHIP endpoints/interior/search directions, all boundary classes, and the
Vreman cancellation signs. NASA9 segment 2 is proved unreachable for the
canonical record over the admissible temperature domain and is therefore not
silently counted as covered.

The r197a reseal makes the acceptance comparison explicit. For every cell and
each coefficient field across all sealed boundary and lineage fixtures, the
gate is `|device - canonical fp64| <= 2e`, where `e` is that sample's termwise
binary32 enclosure error; residual and bound therefore have identical units
and scope. For diffusivity the maximum absolute residual is
`5.6840302565076757e-08 m2/s`, with `5.9999482434865543e-07 m2/s` local bound
at that residual and maximum residual/local-bound fraction
`0.094734654797700393`. For effective conductivity the corresponding values
are `6.0894076837687827e-05 W/(m K)`, `0.00064270352746039427 W/(m K)`, and
`0.09474675995370252`. For molecular kinematic viscosity they are
`1.0286298429782598e-10 m2/s`, `5.9100077840629978e-09 m2/s`, and
`0.017404881356536891`. All local fractions are below one without widening.
The former `0.0020747848823717565` number is retained only as a dimensionless
positive-scale diagnostic and is not an acceptance comparison. The maximum
uncancelled Vreman scale is `0.049928860855711937`.

On the sealed 4x5x4 mixed-boundary fixture, residency counters measure one command, one terminal
staging read, and zero interstage full-grid transfers. The comparator's
certified and actual allocation are both `44296` bytes. The incremental live
authority certificate is `131072` bytes, and adding it to the already certified
force/projection/cell/dual-remap components yields a `5941576`-byte complete
owner peak. A missing lineage tuple, broken periodic seams, invalid device state, and a
short velocity surface each refuse atomically. Changing attempt, parent,
projection, stage, immutable records, or physical boundary classes changes the
device identity. Authentication against a nonzero expected parent belongs to
the separately scoped r196 target-lineage rung and is not claimed here.

No case-authored semantics or inputs change, so `case_record_id` is not
regenerated. The r197a live-binding revision regenerates solver producer
identity, and every later run must regenerate run identity from these solver
semantics. The golden checkpoint remains untouched. Evidence is
sealed in `rendered/fire_production_calibration/r197_resident_transport_authority/`
`resident_transport_authority_evidence.v1`, SHA-256
`7c00ca11c48b6dccddec1c08ec604c36a9d8aac7ac79d67f4c80a62b90bf7059`.
The next separately committed and reviewed rung is resident physical-flux
authority; r197 does not claim a live R0/R1/R2 owner or execute the ported
replay.

### 7.56ae Resident physical-flux authority (r198)

r198 completes the second r196 resident-lineage rung. A private Metal issuer
consumes the private r197 transport authority and device-private state,
temperature, projected velocity, boundary classes, pressure-open active set,
and reconstruction/projector records. It produces two separately named
advective candidates and one nonadvective candidate: `f^L =
f_adv^donor + f_N` and `f^H = f_adv^MC-MUSCL + f_N`. The same centered `f_N`
buffer is added to both candidates. A device-side identity check recomposes
both sums and refuses publication if either candidate does not contain that
bit-identical nonadvective value. The qualification-only public comparator
cannot be converted to the private live authority; there is no CPU issuer or
fallback. The child issuer additionally requires exact identity of the parent
command plus the state, temperature, velocity, thermochemistry, parameter,
and failure handles and their extents. A transport authority minted from
candidate A therefore cannot be paired with candidate-B state; that replay is
a standing atomic-refusal RED. Short inflow and oversized basis surfaces also
refuse before encoding.

The donor and MC-MUSCL advective candidates are each bit-identical to the
strict binary32 CPU operator across wall, periodic-seam, and pressure-open
inflow/outflow fixtures. The MC-MUSCL high candidate preserves the production
operator's two-stage rounding: donor and delta are first formed by the
streaming kernel, then high is materialized as `donor + delta` in a separate
kernel. This prevents a blended composite from concealing a candidate-level
mismatch. The physical mass, energy, and gas candidates are compared with the
canonical fp64 mirror at matched device-transport inputs. A high-temperature
fixture reaches the second NASA9 segment that the original 300--525 K fixture
did not exercise.

Every acceptance check is face- and field-local. A face residual is compared
in identical units with an enclosure built from its adjacent-cell or boundary
ghost stencil, local transport coefficients, uncancelled reconstruction
terms, and the operation-count gamma for that field. Pressure-open ghost
classification uses the exact boundary-face index. Sensible enthalpy is now a
staged device candidate rather than a hidden cancelling primitive. Its log is
computed by a CPU/Metal bit-identical exponent reduction and atanh series
through odd degree 17. The local proof combines a gamma-64 uncancelled series
scale, binary32 ln(2) packing, and the analytic odd-19 remainder; its worst
residual/bound fraction is `0.0051601568397958561`. Per-species NASA9 bounds
then add coefficient/offset packing and uncancelled term scales, and the energy
bound propagates those enthalpy bounds with the already local mass-flux bounds.
No across-field maximum is an acceptance claim, and no bound is scaled by a
cancelled result. The sealed artifact reports all 55 family/field checks
individually. The largest local
fractions are `0.050697956482785987` for donor sensible-energy advection,
`0.00022132475937333758` for MC-MUSCL CO2 advection,
`0.00020309178466380829` for physical CO2 mass flux,
`0.0082494670907624044` for physical O2 sensible enthalpy,
`0.0017207804234051897` for physical sensible-energy flux,
`3.5605115203941027e-6` for the physical z gas flux,
`0.0099341976536549831` for the low CO2 composite, and
`0.00020307817027690736` for the high CO2 composite. These are descriptions
of separate per-field gates, not a pooled acceptance statistic; every local
fraction is below one without widening.

The physical branch word is isolated from its parent transport word. Its
bitmap `0x00001dbf` exactly matches an independent host walker and covers
interior, periodic seam, wall, pressure-open inflow/outflow,
harmonic-positive, NASA9 segments zero and one, shared-nonadvective,
boundary-advective, and interior-advective obligations. Harmonic zero is
unreachable after the positive transport authority, and NASA9 segment two is
outside the transport-admissible temperature domain. The shared-`f_N` mutant,
missing parent lineage, parent-A/child-B candidate, CPU-substitution, extent,
and working-set-understatement surfaces all refuse atomically. Changing
attempt identity changes the device
publication identity. The resident interval remains one command with one
terminal staging read and zero interstage full-grid transfers. The fixture
certificate is `720896` bytes against `159832` bytes actually allocated; the
live incremental certificate is `278528` bytes and the complete 8-cubed owner
peak becomes `6498632` bytes.

No case-authored input changes, so `case_record_id` remains stable. Solver
producer identity changes for r198, later run identities must inherit it, and
the golden checkpoint is untouched. Evidence is sealed in
`rendered/fire_production_calibration/r198_resident_physical_flux_authority/`
`resident_physical_flux_authority_evidence.v1`, SHA-256
`ac13e3f986261337c9cb5a64f27ccb801cdea91f30a0ff077caa4864725386a6`. The next
separately committed and reviewed rung is resident EOS candidate identity;
r198 does not claim the target-lineage rung, live R0/R1/R2 owner, kernel
sweep, or ported replay.

### 7.56af Resident EOS candidate identity (r199)

r199 completes the third r195 resident-lineage rung. The first implementation
was rejected on review because it advanced only the donor/physical low-flux
state and allowed the caller to relabel the same bits as either accepted stage.
The corrected private Metal issuer now performs the complete source-inclusive
FCT `Q*` construction: it consumes r198's low composite and the canonical
advective increment retained before the shared nonadvective flux is composed,
builds the r60 ratios, constructs one shared face alpha, and commits the
accepted state. Reconstructing the increment as `highComposite-lowComposite`
was rejected because it subtracts independently rounded sums and is not the
§3.7 FCT operator. A deliberately limiting fixture obtains alpha
`0.832918644`; its device candidate is byte-identical to the complete binary32
CPU FCT solve. The ordinary fixture also carries nonzero source and flux-delta
terms. This rung accepts only the sealed R0-to-`Q*` role. `Q^{n+1}` refuses
until the live owner can supply its authentic averaged R0/R1 flux parent.

There is no public candidate input. The comparator's terminal candidate copy
is qualification output only and cannot mint authority. Candidate identity
covers the resulting bits, source dose, shared alpha, binary32 precision, step,
attempt/case identity, and the exact r198 and r197a publication handles. The
device also proves the duplicated parent tuple before publication: EOS, FCT,
and transport cell counts agree, EOS and transport attempts agree, and EOS and
FCT timesteps have identical binary32 bits. Stage, precision, attempt, cell
count, and timestep mutations on the same resident handle all refuse with
bitmap `0x50`; a host label therefore cannot repair stale device metadata. The
accepted publication is withheld until conservation admissibility, EOS
inversion, and the r170 physicality bound complete. The raw terminal identity
words are decoded even on device rejection, so those REDs observe the
device-written zero rather than a default host result. Host-preflight lineage
refusals are recorded separately as “no publication issued”; they do not claim
a device terminal read. The short-surface RED presents a genuinely short
private allocation to the issuer. All resident authority members are private
to their issuer functions, preventing same-translation-unit aggregate forgery.

The device evaluates the accepted-state temperature inversion and represented
pressure functional per cell. Apple Metal has no binary64 arithmetic, so the
sealed binary64 thermochemistry and affine coefficients are represented as
three-term binary32 expansions and evaluated with two-sum/FMA compensated
operations. The exact private EOS parameter and thermochemistry handles are
retained as the candidate's single metadata lineage; a different same-sized table refuses, and
all 870 table words are incorporated into the EOS publication identity. The
device searches the positive binary32 temperature lattice and emits the same
single binary32 projection as the CPU/fp64 mirror. Candidate, temperature, and
represented-pressure fields are bit-identical per cell. The per-face shared
alpha is terminal-copied and bit-identical to the CPU FCT mirror, so a
non-injective final-state comparison cannot hide a limiter mismatch. Device
stage, precision, attempt, cell-count, and timestep mutations on the same
handle refuse with exact bitmap `0x50`; only the distinct case handle refuses
at host preflight. The fixture spans the 1000 K NASA9 segment boundary as well
as 300, 425, 950, 1500, 1900, and 2200 K.

The earlier `2^-41` arithmetic claim is withdrawn: its hard-coded obligation
count was not connected to the Metal DAG and therefore was not a proof. Each
three-term value now carries a fourth, outward error bound. Packed constants
begin with the local spacing of the discarded tail. Addition accumulates with
general `TwoSum`; multiplication accumulates every 3-by-3 product and its FMA
residual; division certifies the quotient from its computed residual divided by
a proved denominator lower bound. The logarithm uses an atanh series through
odd order 49 and includes its analytic remainder; its denominator uses
directed-down subtraction/multiplication, the `ln(2)` triple carries its
`2^-54` semantic remainder, and a predeclared `2^-52 max(1,|log|)` absolute
allowance covers the exact host `std::log` projection only after full-domain
qualification. This allowance was not fitted to a sampled maximum. The Metal
sweep exhausts all `24346625` binary32 temperatures from 300 through 2200 K
and all `24346624` adjacent midpoints: `48693249` arguments total. On the
identity-bound `/usr/lib/system/libsystem_m.dylib`, macOS build `25F84`, kernel
`25.5.0`, arm64 host, its worst residual/bound is `0.5192248117002557` at
index 1. The Metal half is independently bound to registry
`0x000000010000087d` (`Apple M4 Max`, family `apple9`), Metal runtime
`com.apple.Metal` version `373.2`, explicit language `3.2` and safe math, the
exact resident-library source SHA-256
`293dd5e520f9f7c9d330da9e128177127a2b8b90f9d4b31d9c0a3d9b44b990d4`,
compiled function-set SHA-256
`6e409886fb98102be190cc90c4c460a1c1c5e15184a05d21f50442f3575e87b7`,
kernel name, and pipeline traits (`32`-wide execution, `1024` maximum threads,
zero static threadgroup bytes). The source digest is computed from the exact
string passed to `newLibraryWithSource`; the function-set digest comes back
from the compiled library. Metal exposes no portable binary export, so these
two digests plus the compiler/runtime/device/options tuple are the compiled
library identity. All batches returned the identical tuple. Any CPU libm, OS,
Metal device, runtime, language, math mode, source, function set, or pipeline-
trait change requires requalification. An independent
Q100 fixed-point atanh-series interval also proves mathematical `ln(2)` lies
inside the packed triple's `2^-54` bound; the separately named binary64
projection residual is zero on this provider. Every gate compares the
resulting intervals and refuses when their order overlaps. Thus r60 and r170
are protected by a connected error enclosure, not output spacing or
subtractive cancellation.

Every cell's temperature, represented-pressure, and absolute-deviation output
bit-matches the fp64 mirror's single binary32 projection. The local projection
enclosures remain diagnostic, not an acceptance substitute. All 192 cell/field
rows are retained in the SHA-bound raw transcript; their residuals and worst
residual/enclosure ratios are zero. Four additional arithmetic-boundary cases
span 300, 1000, 1000.001, and 2200 K and state scales from `0.750500023` to
`1.24950004`. No pooled across-field statistic participates in acceptance.
Pressure and deviation publish only when their complete propagated intervals
fit strictly inside one binary32 rounding bin. Qualification values placed
exactly on each midpoint refuse with `0x200` and zero candidate/EOS identities;
an ambiguous interval can never acquire publication authority. Zero and
minimum-subnormal bin centers publish with exact bits. Because Apple GPU
arithmetic flushes subnormals, the proof scales tiny expansion components by
`2^126` through their integer significands before constructing exact
midpoints; intervals covering the zero/min-subnormal neighboring boundaries
both refuse with `0x200`.

The isolated r60 RED calls the production `fct_commit_scalar` kernel directly:
energy `2283578` is admitted and its adjacent binary32 value `2283578.25`
refuses with exact bitmap `0x10`. The separate `0xD0` and `0x80` observations
are EOS lower/upper inversion refusals, not r60 endpoint evidence. The r170
kernel still refuses a constructed exact ratio above `1.25` even though its
binary32 publication rounds to `1.25`; its bitmap is `0x400`. No dynamics
threshold was widened.

The monitored-manifold boundary remains explicit. EOS deviation is recorded
for every candidate, but no inherited pressure-deviation or low-Mach ceiling
is introduced. A 20% represented-pressure deviation publishes successfully.
Only conservation-class accepted-state admissibility and the r170 dynamics
physicality bound `|P/P0 - 1| <= 2^-2` refuse; the 30% mutant is rejected.
An fp64 label on the binary32 resident surface also refuses per the r148
precision-boundary rule. The isolated EOS branch bitmap `0x000003ff` exactly
matches its independent host walker and covers materialization, identity,
admissibility, lower clamp, interior inversion, publication, and all three
NASA9 segment classes reachable over the sealed case interval.

The qualification interval is one Metal command with one terminal staging
read and zero interstage full-grid transfers. Its fixture certificate is
`1130496` bytes against `128384` bytes allocated. The r198 live certificate is
`294912` bytes after counting its retained advective delta explicitly; the EOS
live incremental certificate is `262144` bytes because it consumes rather than
duplicates that parent. The complete 8-cubed owner peak is
`6875464` bytes. No case-authored input changed, so `case_record_id` remains
stable; producer and future run identities inherit r199 semantics, and the
golden checkpoint remains untouched. Evidence is staged for fresh review in
`rendered/fire_production_calibration/r199_resident_eos_candidate_identity/`
`resident_eos_candidate_identity_evidence.v1`; its final SHA is recorded after
the fresh boundary review closes.
The next separately committed and reviewed rung is authenticated target
lineage; r199 does not claim the live R0/R1/R2 owner, final kernel sweep, or
ported replay.

### 7.56ag Authenticated resident target lineage (r200)

r200 completes the fourth r195 resident-lineage rung. A private Metal target
issuer consumes the exact command-local publications made by r197a transport,
r198 physical flux, r199's source-inclusive accepted `Q*`, r199 EOS, and an
opaque device-produced frozen-source authority. Its issuer accepts only the
opaque `FireProductionFrozenSourcePacketSeal` minted by the canonical FireSim
source authority; no raw source-target field is accepted by the qualification
surface. Metal publishes the seal's source field into private storage and binds
its on-device identity to the exact candidate publication. Host preflight also
recomputes the seal's beginning-state identity from the exact resident
conservative and canonical-temperature fields and requires the exact case-record
identity. Genuine canonical seals minted from a different beginning state or
different case therefore refuse even when their source dose is byte-identical.
Neither shared CPU
bytes nor a byte-identical private CPU blit can substitute for that issued
authority. It accepts no public candidate or target object. The child retains the five exact
publication handles and the parent command; every parent must be nonzero on
device, while the issuer separately requires pointer identity through the
immediate chain `transport -> physical flux -> Q* -> EOS`. A stale but valid
candidate and an EOS publication relabelled onto a different candidate both
refuse at host preflight, before a command is committed. Thus the r70 child can
only attach to the projection/flux-produced candidate whose EOS was evaluated,
not merely to an independently EOS-admissible state.
The target also requires the exact EOS thermochemistry handle retained by the
candidate. On device it compares shape, face offsets, cell width, timestep,
attempt, and boundaries against the resident transport, FCT, and EOS parameter
surfaces, and compares the source-packet identity against the sealed frozen-
source metadata. Agreement between target and consumer metadata alone is not
authority.

The target is evaluated as independently published terms before composition:

`S_div = dV(Q_stage)[D(f_N)] + S_div^source + S_div^tail`.

The first term consumes r198's separately retained centered nonadvective mass
and energy flux, with the exact r197a stage state and temperature. The second
is the frozen absolute source-packet target. The EOS absolute-reference signal
is published separately as the correctly rounded binary32 value of
`(P_rep/P0 - 1)/Delta t` for monitored diagnostics; it is not summed in the
bulk. Only r170's signed tail excess beyond `2^-4` is drained toward that
threshold and enters `S_div^tail`. Tail magnitude consumes r199's authoritative
binary32 absolute-deviation publication; the represented-pressure publication
supplies only its sign. This closes the near-threshold alias where rounding the
pressure ratio first can hide an above-threshold exact deviation. Tangent and
tail remain expansion-valued through composition and the assembled term is
rounded once; their separately published diagnostic fields retain their own
binary32 projections. Closed topology then removes the constant mode;
pressure-open topology preserves it.

Closed and pressure-open 64-cell Metal fixtures separately compare tangent,
frozen source, raw absolute-reference diagnostic, monitored tail, and final
compatible target. Every one of the 640 cell/field values bit-matches the fp64
mirror's binary32 projection. Each row also reports the error to its binary64
reference against a same-cell, same-field enclosure in `s^-1`, derived solely
from half the larger adjacent binary32 spacing of the published value; it does
not consume the measured residual. Only the exact-copy source field has a zero
bound. The ten acceptance triples `(max residual, max local bound, worst
residual/bound ratio)`, all in `s^-1` except the dimensionless ratio, are:
closed tangent `(2.5014402105227873e-08, 2.9802322387695312e-08,
0.97716035693883896)`; closed frozen source `(0, 0, 0)`; closed absolute
diagnostic `(0, 0.000244140625, 0)`; closed monitored tail
`(0, 6.103515625e-05, 0)`; closed assembled `(0.0001656421027291799,
0.000244140625, 0.986663818359375)`; pressure-open tangent
`(2.3924225600602256e-08, 2.9802322387695312e-08, 0.99220840632915497)`;
pressure-open frozen source `(0, 0, 0)`; pressure-open absolute diagnostic
`(0.0001367544009553967, 0.000244140625, 0.99999964237213135)`;
pressure-open monitored tail `(1.4684030247735791e-05, 6.103515625e-05,
0.90429652854800224)`; and pressure-open assembled
`(0.00016326467721228255, 0.000244140625, 0.97005952894687653)`. A
displaced-exact RED exceeds the unchanged
local enclosure by 4x, proving that the bound can fail. There is no across-field
acceptance statistic and no cancellation-sensitive bound. Integrated states
exercise positive and negative deviations just below and above `2^-4`.
Qualification-only device EOS injections publish positive and negative exact-
threshold EOS surfaces under sealed lineage; both produce positive zero drain.
This proves the device predicate without claiming a naturally evaluated exact-
threshold candidate. During implementation, an
initially float-evaluated face
divergence missed one tangent result by one ULP. The remedy was to carry the
already certified expansion arithmetic through the flux divergence, not to
widen a tolerance. The final five fields are byte-identical.

Because adding these kernels changes the exact library compiled by Metal,
r200 does not inherit r199's execution identity silently. The complete r199
EOS sweep is re-run through the same resident context. All 48,693,249 log
qualification arguments, 192 per-cell EOS comparisons, rounding-edge cases,
and 13 device REDs pass unchanged. The current library source and compiled
function-set digests are recorded from that control in the SHA-bound r200
evidence artifact, rather than inherited from r199.
The device/runtime/language/safe-math tuple remains the r199-qualified Apple
M4 Max tuple. This control binds the parent EOS proof and the newly compiled
target functions to one actual library identity.

The device refusal surface is explicit. Zeroing any one of transport,
physical-flux, candidate, EOS, or frozen-source publication produces failure bitmap `0x2800`
(`0x0800` target-parent failure plus the downstream `0x2000` consumer refusal),
with zero target and consumer identities. A preauthored target and a mismatched
projection topology each reach the resident consumer and refuse with `0x2000`;
their otherwise valid target publication remains diagnostic, but consumer
identity is zero. The topology RED changes the projection metadata request while
leaving the target authority intact. The projection-consumer admission stage
issues an opaque private metadata surface on device and binds its identity to
the actual target publication; the consumer accepts that type, not uploaded bytes, and compares
exact shape, cell width, timestep, attempt, and all boundary classes.
Byte-identical CPU metadata and CPU target substitutions both refuse at host
preflight. The source issuer has no raw-field input surface; its two substitution
REDs replace the exact issued handle with either the canonical seal's shared upload
or a byte-identical private CPU blit, and both refuse before target issuance. A mismatched source packet,
genuine wrong-beginning and wrong-case canonical source seals,
and each stale target shape/face-offset/cell-width/timestep/attempt/boundary
mutation, refuse on device with `0x2800`; an alternate same-size EOS table is
refused by exact parent-handle identity before submission. The target identity now includes
the exact `2^-4` bits: a one-ULP dormant threshold mutation leaves all five
fields bit-identical but changes the target identity. The stale-candidate,
EOS-accepted-but-unlinked, CPU-produced-
target, and understated-working-set cases refuse before device execution and
issue no publication. These distinctions re-derive r189's refusal surface on
device without claiming that a host-preflight refusal was a GPU observation.

The qualification interval remains one command, one terminal staging read,
and zero interstage full-grid transfers per valid interval. A scoped ledger at
every copy seam records these independently of host reads. Its RED performs a
real full-grid private-to-shared interstage blit without reading that buffer;
the ledger observes one transfer and the publication certificate refuses. The
fixture certificate is `1507328` bytes against `132000` bytes allocated. The target's
live incremental certificate is `196608` bytes; it accounts for the private
source field, its sealed metadata and candidate-bound identity, five per-cell
target fields, target identity, the device-issued projection metadata and its
identity, and consumer identity while consuming rather than duplicating the
other resident parents.
Closed and pressure-open observed/required arithmetic-obligation bitmaps are
exactly `0x03700180` and `0x02f00180`, respectively; they bind below/above tail,
both tail signs, term assembly, and both compatibility branches. The
qualification result is a
terminal tap only and is statically not
convertible to projection authority. No case-authored semantic or input changed,
so `case_record_id` remains stable; producer and later run identities inherit
r200 semantics, and golden checkpoint
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947` is
untouched. Evidence is staged
for fresh review in
`rendered/fire_production_calibration/r200_authenticated_device_target_lineage/`
`resident_target_lineage_evidence.v1`; its final SHA is recorded only after the
boundary review closes. The next separately reviewed rung is the complete
R0/R1/R2 owner live on Metal. r200 does not claim that owner, the final kernel
sweep, or the ported replay.

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
