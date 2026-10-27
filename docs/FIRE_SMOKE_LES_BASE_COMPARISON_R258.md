# r258 — explicit LES FDS and near-base comparison

Launched 2026-10-25; results finalized 2026-10-27. **Both simulations complete;
CHARM does not close the gap, while matched LES FDS retains good entrainment.**
No production RISE solver changes. The owner requests explicit
LES/Vreman/CHARM FDS, then near-base measurements if FDS entrains correctly
and valid CHARM B does not close RISE's gap.

## First B: measured numbers and invalidity

Common window **15.01872063824–19.01872063824 s**, net mass R<0.8 m:

| Height (m) | A MC (kg/s) | Nominal first B (kg/s) | Earlier FDS VLES/Superbee, same window (kg/s) |
|---|---:|---:|---:|
| 0.994 | 0.243158 | 0.243158 | 0.292287 |
| 2.004 | 0.478832 | 0.478832 | 0.667779 |
| 3.013 | 0.718458 | 0.718458 | 1.208187 |

**First B is invalid as a CHARM experiment.** Its external patch changed
`fct_mc`, while the active resident run uses `evaluate_resident_advective_pair`
→ `pf_mass_slope` → `pf_mc`. The resident reconstruction remained MC. The exact
match cannot exclude reconstruction as a cause. This implementation/review miss
was discovered when checking the suspiciously exact result. Original raw data,
patch and hashes are retained and explicitly labelled invalid. Failed runtime
output is preserved as `invalid_inactive_path_run` within the same directory.

The corrected external patch changes only **active resident pf** reconstruction
to direction-aware CHARM. Affine projection, FCT bounds, compatible momentum,
source and all other solver terms remain unchanged. Independent review traced
the actual resident dispatch to the changed routine. The build is warning-free;
production source/objects/binary are untouched. Corrected B uses the same
checkpoint, observer and nominal four seconds; A/B uses the common physical
interval if timesteps change. It runs serially after FDS. **Completed results are below.**

## Explicit LES FDS repeat

Started **2026-10-25 07:20 PDT**, unmodified FDS 6.11.1, one MPI rank and eight
OpenMP threads. Input explicitly selects `SIMULATION_MODE='LES'`,
`TURBULENCE_MODEL='VREMAN'`, `C_VREMAN=0.07`, `FLUX_LIMITER='CHARM'`, Pr/Sc=0.7.
Runtime output confirms LES and Vreman. Grid/domain, patterned 69-face burner,
33 kW methane source, 300 K ambient and radiative fraction 0.20 match r255.
End time 25.0910401272 s; statistics 10.4660401272–25.0910401272 s. LES also
changes FDS mode-specific numerics; this is not a limiter-only FDS experiment.

New output retains native horizontal u/v/w, temperature, density and
thermodynamic D near the base and near 1/2/3 m, plus burner-face w devices.
Existing mass-flux slices and instantaneous centerline devices remain. This
closes the earlier off-axis output gap for this new run.
Input/manifest: `docs/data/fire_r258/`. **Completed FDS results are below.** The prior full-window 1≤f<3 Hz centerline powers
at 0.994 m are 996.925 K² / 0.147518 m²/s² for r251 and
3845.338 K² / 0.494977 m²/s² for earlier VLES FDS (temperature / velocity).
The new run uses the identical Welch reduction for comparison.

## Near-base definitions

- Actual divergence uses native MAC differences in both codes. FDS output
  `DIVERGENCE` is thermodynamic target D, recorded separately. U/V index-zero
  boundary faces are retained until differencing.
- Compare signed mean, mean absolute and RMS divergence over r<0.30 m, at
  native heights **0.107–0.505 m**, over the common statistics window.
- Compare actual w at each authored burner face, rather than infer inlet
  velocity from nominal heat flux.
- Rim shear: first outward 90%-to-10% crossings of the peak radial mean w,
  with zero reference. Report peak location and thickness. At dx=0.030593 m
  this is a resolution-limited mean-profile measure, not an instantaneous
  material-layer or subcell thickness.
- RISE's offline probe reads retained r251 checkpoints with r254's existing
  temporal weights; it advances no solver stage.

No generation-versus-dissipation conclusion follows from invalid B or from
normalized spectra alone. First compare explicit-LES FDS and valid B, then
report where near-base quantities differ. **Stop for review before production
solver changes. No tier 6 or tier 10.**

Runtime: `/Volumes/SSD/RISE/fire_production_calibration/r258_fds_les/`.
Corrected B reuses `/Volumes/SSD/RISE/fire_production_calibration/r257_charm_ab/`.

## Launch and retained-data checks

The bounded completion controller waits for this single FDS run, reduces its
mass/spectral/near-base data, then launches only the corrected resident CHARM
continuation and reduces A/B over their common interval. It stops after this
comparison; no repeated job, production adoption or other simulation is queued.
Runtime status is `completion_status.json` beside the FDS log.

Independent review found the corrected active dispatch and formulas consistent.
One reducer endpoint issue was fixed: binary32 slice endpoint rounding is
aligned to the known final event within half an ULP before temporal quadrature.
Constant/linear quadrature and analytic rim crossings pass independent checks;
all near-base native slice headers have the expected quantity and z face.

RISE-only retained summary (FDS comparison pending): over r<0.30 m, mean absolute
MAC divergence is 0.52492 s⁻¹ at 0.107 m and 0.23895 s⁻¹ at 0.505 m; RMS is
1.55721 and 0.57717 s⁻¹. Its 69 positive burner faces have mean w 0.0201311 m/s,
range 0.0153802–0.0237715 m/s. These are measurements, not a comparison verdict.

## Standing A/B rule: prove a change within two steps

Before the corrected four-second B, replay exactly two MC accepted steps from
this checkpoint with a paired host observer and save all Q/T/M/U reference
fields. The CHARM process starts from the same checkpoint, compares its first
two accepted states, and writes per-field numeric differing-value counts and
maximum absolute differences to `early_fields.csv`. If all fields are unchanged,
it exits before taking a third step. Signed-zero bit changes do not qualify.
A changed field is proof against a no-op experiment, not proof of its physical
cause. Historical r251 restart identity remains waived and deferred.

The same observer object is linked to MC and CHARM. An external launcher-only
copy permits a two-step witness; no production test/source is edited. Build is
warning-free and both observer/admission and paired-link reviews are clean.
The early check runs after the higher-priority FDS measurements. No full B
window can proceed without its first-two-step difference check passing.

## Interruption and recovery — 2026-10-25

The first FDS process and both wrappers disappeared after the last logged
step 4,400 at 12.58312 s (07:55:18 PDT), without a logged FDS error or normal
completion. Cause is unknown; stale status JSON is not evidence of a live run.
Interrupted diagnostics are preserved. The same unchanged input was relaunched
from zero at **07:59:37 PDT**, with a detached one-shot wrapper ignoring SIGHUP.
The FDS process and advancing simulation time were verified. No recurring job
or launchd service remains registered. The wrapper still reduces FDS first,
then performs the two-step MC/CHARM check before the long B window.

## Completed results — reported 2026-10-27

Both simulations completed successfully on **2026-10-25**: FDS at 09:07 PDT
(25.09104 s; 8,926 steps), corrected CHARM B at 12:51 PDT (19.000612 s;
2,432 additional steps). Nothing remains running. A final bookkeeping import
used system Python without NumPy and failed **after** both simulations and all
physical reductions completed. The status was repaired read-only; no rerun.

### Active-path B and its early sanity check

The first CHARM step differs from the fresh two-step MC witness in **3,561,631
conservative values** and **59,598 temperatures**, with maximum temperature
difference **22.657 K**. Numeric Q/T/M/U differences were checked at both steps;
the pass was logged before advancing step three. The early field CSV is saved
in `data/fire_r258/early_B_fields.csv`. This detects a no-op experiment, not its
physical mechanism. No historical restart bit-identity requirement was added.

B covered 3.981891 s, slightly shorter than four seconds because its accepted
timesteps changed. Compare every code over **15.018721–19.000612 s**:

| Native height (m) | A MC (kg/s) | B CHARM (kg/s) | B vs A | Earlier FDS VLES (kg/s) | New FDS LES (kg/s) |
|---|---:|---:|---:|---:|---:|
| 0.994 | 0.243094 | 0.247097 | +1.65% | 0.292089 | 0.302533 |
| 2.004 | 0.478899 | 0.501193 | +4.66% | 0.667400 | 0.701387 |
| 3.013 | 0.718660 | 0.758019 | +5.48% | 1.207904 | 1.238150 |

The real reconstruction change has a measurable but small effect. B remains
**38.8% below new FDS at 3 m**, recovering about 7.6% of A's gap there.
CHARM does not close the deficit. This checkpoint diagnostic is not a proposed
production adoption or a from-zero acceptance result.

### Like-for-like FDS full-window comparison

Statistics **10.466040–25.091040 s**, net mass inside R<0.8 m. The FDS native
face-flux / RISE cell-density-times-centered-w convention difference documented
in r255 remains explicit; the same convention is used for both FDS cases.

| Height (m) | r251 (kg/s) | Earlier FDS VLES/Superbee | New FDS LES/CHARM | Heskestad 26.4 kW |
|---|---:|---:|---:|---:|
| 0.994 | 0.241484 | 0.300563 | 0.303500 | 0.248329 |
| 2.004 | 0.486183 | 0.721525 | 0.718567 | 0.705955 |
| 3.013 | 0.742357 | 1.311274 | 1.267897 | 1.356438 |

At 3 m, LES FDS retains **93.47% of the correlation**, versus 96.67% for
VLES and 54.73% for r251. Switching FDS to explicit LES/CHARM does not reproduce
RISE's deficit. The 1 m correlation is closer to the flame-transition region;
do not treat its excess as a new tuning target.

The same full-window centerline **1≤f<3 Hz** powers at 0.994 m, using the
unchanged r257 common-grid Welch method:

| Quantity | r251 | Earlier FDS VLES | New FDS LES | r251 / new FDS |
|---|---:|---:|---:|---:|
| Temperature rise (K²) | 996.925 | 3845.338 | 2022.895 | 49.28% |
| Vertical velocity (m²/s²) | 0.147518 | 0.494977 | 0.467195 | 31.58% |

Thus the earlier 26% thermal ratio does not survive the mode comparison;
the velocity deficit remains almost unchanged. A short nonstationary spectral
record and a mode change do not by themselves uniquely distinguish generation
from dissipation, although the weak large-scale velocity signal is already
present just above the flame.

### Where the near-base measurements differ

All measurements below use retained **r251**, not B, and the new LES FDS over
the full statistics window. Actual MAC divergence is compared, not an FDS
thermodynamic target substituted for it.

| Native height (m) | Mean abs divergence RISE / FDS (s⁻¹) | RMS divergence RISE / FDS (s⁻¹) | Outer 90–10% radial-w width RISE / FDS (m) |
|---|---:|---:|---:|
| 0.107 | 0.5249 / 0.5130 | 1.5572 / 1.3970 | 0.08953 / 0.09657 |
| 0.199 | 0.3945 / 0.3524 | 1.2100 / 1.1455 | 0.09914 / 0.10643 |
| 0.505 | 0.2389 / 0.2262 | 0.5772 / 0.6192 | 0.14203 / 0.14902 |

The full 14-height divergence and radial profiles are in the accompanying CSVs.
RISE mean absolute divergence is **2.3–12.4% higher** across these planes; there
is no broad deficit in expansion magnitude. RMS is higher low in the flame and
slightly lower near 0.5 m. This says nothing yet about the spatial/temporal
phasing of expansion relative to the shear layer.

The **burner-face inflow velocity is the largest measured difference**:

| 69 authored burner faces | RISE | FDS |
|---|---:|---:|
| Mean w (m/s) | 0.0201311 | 0.0103222 |
| Minimum w (m/s) | 0.0153802 | 0.0100464 |
| Maximum w (m/s) | 0.0237715 | 0.0107584 |

RISE's measured inlet mean is **1.95× FDS** and its spatial range is much wider,
despite the matched nominal fuel power and burner mask. Individual face rows
are retained. This identifies an inlet-velocity/source-coupling difference;
it does not yet identify the responsible density or boundary formula.

RISE's mean radial-w outer transition is narrower, about **4.2–8.5%** over the
sampled heights. The absolute width differences are below one **0.03059 m**
cell, so this does not resolve a distinct physical rim layer or prove a
smearing mechanism. The radial peak stays on axis for both codes.

**Conclusion:** the scalar reconstruction swap is not sufficient, and matched
FDS still entrains well. The measured near-base distinction is chiefly the
inflow velocity profile, rather than missing divergence magnitude or a resolved
broader RISE rim. The next read-only question is how prescribed fuel mass flux,
face density and source/divergence/momentum coupling set that inlet velocity
and its vorticity generation. No solver change or further run is made.
**Stopped for owner review. Tier 6 cancelled; tier 10 unlaunched.**
