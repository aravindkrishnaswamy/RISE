# r255 — sequential entrainment discriminators (in progress)

2026-10-19. Owner approved installing FDS 6.11 and its matched comparison.
**Results are pending.** This page records the bounded campaign; it does not
claim completed comparisons. No production solver source or kernel changes.

1. FDS 6.11.1, unmodified tag `ff928db`, built natively on ARM64 using Homebrew
   GNU Fortran 16.2/Open MPI 5.0.11. The published Intel binary cannot run on
   this host without Rosetta, which is absent. Optional HYPRE/SUNDIALS/MKL are
   not used by this build's standard single-mesh pressure solver. Upstream
   OpenMP-deprecation warnings are retained in its external build log.
2. A four-second RISE continuation from retained step 9,224, t=15.01872063824 s.
   The observer checks every timestep against r251's history and compares all
   Q/T/M/U binary32 values against retained steps 9,276 and 9,325 before passing
   that prefix. A mismatch terminates this diagnostic, before its remaining
   interval. These are actual earlier trajectory endpoints, not self-comparison
   between two new continuations. No equivalence certificate is generated.
3. Canonical tier 6 from zero, full window, with the existing
   `bin/tests/FireSequenceTest` (SHA256
   `d1cebdd779a5d18a9e5b92ec162ddfcfc51aaae1a8963104bcdc633fb7bab033`).

Simulations execute one at a time. No tier 10, parameter sweep, recurring
monitor or new certification machinery is authorized. The accepted r251 data
remain read-only. Runtime inputs, binaries, slices, frames, large CSVs and logs
are in `/Volumes/SSD/RISE/fire_production_calibration/r255_comparison/`.

## Matching and measurement conventions

FDS uses the exact r251 extents 2.11092500865×2.11092500865×3.24287030315 m,
69×69×106 cells, dx=0.0305931160674 m, ambient 300 K, dry oxygen mass fraction
0.232917511458, gravity 9.80665 m/s², 0.30 m methane burner, 33 kW input,
LHV=50.0273648804 MJ/kg, zero soot/CO yield and prescribed χr=0.20.
Vreman c=0.07 and turbulent Pr/Sc=0.7 match RISE's selected model. The 69
burner cells and normalized 1% SplitMix64 inlet pattern are taken from the
canonical RISE case, rather than substituting a square burner. Five outer
boundaries are open; the floor is adiabatic. FDS supplies its own ignition,
combustion, wall and radiation algorithms, including its default startup fuel
ramp; it does not emulate RISE's pilot or source-packet chemistry. Realized HRR
and radiation are measured for interpretation, not assumed equal to input.

FDS centerline comparisons use the identical native heights and selected-station
mask as r252's r251 row. Temperatures are converted from °C to K, then subtract
300 K. FDS mass uses native conservative face flux ρ_face w_face, integrated in
the same R<0.8 m cylinder, and adjacent vertical-face averages at r251 cell
heights. This is explicitly distinct from r253's ρ_cell mean(w_faces) quadrature.
The entire-plane upward bound is retained as well. Heskestad's same 26.4 kW
correlation and virtual origin are used. Device/slice products are averaged
over the same 10.46604–25.09104 s statistics interval with interpolated endpoints.

The RISE observer records **accepted R1 scalar-carrier alpha on native MAC
faces**, inherited by the compatible momentum flux. There is no independent
momentum-velocity blend. It records x/y/z faces separately and together, with
R<0.8 m and the adjacent accepted temperatures averaging above 301 K. A face
is limited if alpha<1 exactly. Mean low-order blend is 1−alpha; both all-face
and limited-face-only means are reported. Count and blend sums are weighted
by the represented dt. This measures inherited coefficient activity, not
kinetic-energy loss or the magnitude of a gas-flux correction when its delta
is zero. It does not average alpha to fabricate a dual-face limiter coefficient.

`r255_limiter_driver.cpp` wraps the existing production API only after it has
returned. Logging cannot modify inputs or outputs. Existing library objects and
kernels are reused. Its build script copies the test launcher externally and
changes exactly one diagnostic-duration admission from 64 to 4096 steps; no
arithmetic or production source changes. The actual interval is bounded by
r251's accepted timestep history to approximately four seconds. Original case
duration, 16 Hz output events, source-step ceiling and eight CPU source workers
are preserved. The first-state equivalence check is required because unchanged
source alone does not establish identical continuation arithmetic.

Tier 6 uses the canonical current-binary case, whose rounded domain/grid and
statistics start may differ slightly from tier 8. They will be reported. Its
McCaffrey rows retain r237's flame-tip/height selection rule; entrainment uses
r253's mask/products/correlation on retained full-state checkpoints.

A setup attempt was stopped at 0.814 s after FDS reported overlapping full-floor
and burner vents (burner vents rejected). That input was corrected to use the
adiabatic floor as the default surface, then the comparison restarted from zero
in the same directory. The failed setup log is retained; it is not a comparison
result. Earlier input-parser refusals did not advance a simulation.

## Comparison

| Quantity | r251 tier 8 | FDS 6.11.1 | Tier 6 | Limiter continuation |
|---|---:|---:|---:|---:|
| Maximum McCaffrey ΔT error | +87.29% | Pending | Pending | — |
| Maximum McCaffrey w error | +25.01% | Pending | Pending | — |
| Mass flux near 3 m (kg/s) | 0.7424 | Pending | Pending | — |
| Heskestad mass near 3 m (kg/s) | 1.3564 | Pending | Pending | — |
| Limited-face fraction / mean 1−alpha vs height | Not retained | — | — | Pending |

Stop for owner review after these results. No solver remedy is selected by this
launch record.
