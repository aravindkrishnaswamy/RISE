# r258 — explicit LES FDS and near-base comparison (in progress)

2026-10-25. No production RISE solver changes. The owner requests explicit
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
interval if timesteps change. It runs serially after FDS. **Results pending.**

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
Input/manifest: `docs/data/fire_r258/`. **FDS results pending.** The prior full-window 1≤f<3 Hz centerline powers
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
