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

Each metric m has an oracle calibration package containing repeated oracle
runs, adjacent dt/grid refinements, the V-tier expected value, and the preserved
tier-10 prefix where applicable. Define

`B_m = max(3 sigma_block,m, 1.25 |m_oracle,fine - m_oracle,coarse|,
           B_fp32,m)`.

`sigma_block,m` is the fixed-block bootstrap spread of the oracle time series;
for bit-identical repeats it is zero. `B_fp32,m` is the independently measured
table/reduction quantization floor. A metric passes only when the production
difference is at most `min(B_m, C_m)`, where C_m is the scientific ceiling
below. Calibration bytes and the resulting tolerance are versioned evidence;
the production run may not estimate its own tolerance.

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
  and Metal;
- one V-cycle has three weighted-Jacobi pre-sweeps and three post-sweeps with
  the stored binary32 result of `2.0f/3.0f`; the coarsest level performs 32
  Jacobi sweeps;
- exactly 12 V-cycles execute from zero pressure.  There is no residual-based
  early exit, retry projection, warm-start history, atomics, subgroup scan, or
  CPU solve.  Residual reductions occur only before cycle one and after cycle
  twelve through fixed padded max/sum trees.

Twelve cycles are the pre-release fixed schedule selected to fit the r82
120 ms projection allocation while leaving a factor-of-two smoother-count
comparison in the P2 validation report.  If the independent manufactured
fields miss the scientific ceiling, the schedule/model is revised in a new
numbered design revision; a runtime knob or per-case cycle adaptation is not
allowed.

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
