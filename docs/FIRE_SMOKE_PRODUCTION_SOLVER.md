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

Fresh review found that r156 used a valid sufficient-inclusion width in the
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

The exact-manifold branch remains separately fail-closed.  Pass-0 face fluxes
are affine between the already-rounded low and production-high fluxes.  The
diagnostic releases the left and right alpha independently over `[0,1]^2`, a
superset of every shared-alpha/backtracked choice, and subtracts a `gamma_128`
rounding allowance.  At the bound witness the certified lower molar density is
`0.040621989116021835 kmol/m3`, still above
`P/(R*Tmin)=0.040621987915680717 kmol/m3`; the implied
`299.99999426911722 K` enthalpy lookup must return the exact authoritative
out-of-domain error.  This proves only the constitutive-domain boundary; it is
not presented as r60 exclusion.

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
