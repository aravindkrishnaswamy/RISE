# Fire/Smoke Production Solver Contract

Status: **r82 design contract; implementation increments pending**  
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
