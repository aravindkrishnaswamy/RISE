# r263 — FDS no-slip floor and burner repeat

Owner instruction: after the current r262 queue, repeat the full-output LES
FDS case once with `NO_SLIP=T` on the floor and every burner surface, keeping
everything else unchanged. Compare upward mass flux at0.29/0.5/1/2/3m
with default-wall FDS r261 and RISE r251. If the base flux moves substantially
toward RISE, propose a floor/burner wall function for RISE; stop before implementation.

## Definition and execution

[Input](data/fire_r263/r263_fds.fds) and [exact diff](data/fire_r263/input.patch).
The only input changes from r261 are the output CHID and `NO_SLIP=T` on
`FLOOR` and `B0` through `B68` (70 surfaces). A round-trip assertion verifies
that removing these additions restores the original namelists exactly,
ignoring whitespace. LES/Vreman/CHARM, grid/domain, burner area/elevation,
fuel flux/temperature, gravity, chemistry/radiation, end time25.091s,
all devices, slices and output cadences remain unchanged. Same unmodified
FDS6.11.1 executable and8 OpenMP threads.

`NO_SLIP=T` selects the reflected-ghost tangential velocity condition and
bypasses FDS's wall shear function, as verified in the preceding boundary
audit. Existing FDS wall-adjacent eddy-viscosity modeling remains unchanged;
this is a controlled wall velocity/shear choice, not an assertion that all
RISE/FDS near-wall discretization and SGS details become identical.

External directory:
`/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/`.
The bounded, non-recurring launcher waits for the existing r262 queue to
finish and for all RISE/FDS solvers to exit before starting. It retains all
physical fields and stops after reduction for review. No production change.

A short field sanity comparison uses bottom-layer temperature and vertical
velocity at the first two noninitial retained B output times against r261,
with baseline fields linearly interpolated to B timestamps. FDS writes at
accepted-step times, so matching output cadence does not require equal times.
It stops the repeat if both states remain numerically identical. These are
**retained output states**, not the first two internal FDS timesteps; those
reference fields were not retained. Exact times/counts/magnitudes will be
saved in `early_fields.json`. RISE's two-internal-step A/B checks are unchanged.

## Requested measurement

Statistics window10.4660401272–25.0910401272s, matching the accepted r251/r261
comparison. Native cell heights0.290635/0.504786/0.994276/2.003849/3.013422m.
The primary upward flux is the positive part of the centered vertical mass
flux, integrated inside radius0.8m; full-plane upward flux and net radius0.8m
flux are reported separately. FDS's adjacent native mass-flux faces are
averaged before positive clipping. RISE uses the previously measured
retained-checkpoint quadrature. The r262 cell-density/centered-velocity check
found less than0.01% convention sensitivity at the three near-flame heights.
Actual timestamps and the known binary32 endpoint are used in quadrature.

Pending: `upward_mass_comparison.csv` and `results.json` will contain the
five rows, relative change from default FDS, and fraction of the default-FDS
to-RISE gap closed. The sign and size of that movement will determine the
wall-function proposal; no causal verdict or production implementation is
made before the measurement.

## Preparation checks

Input/measurement review found no P1. Queue review found one P1: requiring
identical early slice timestamps could reject a valid FDS variant, because
outputs occur on accepted steps. Fixed by interpolating baseline fields to
B's actual retained times, with explicit bracketing. The native-record reader
and interpolation were checked against r261 retained outputs, including exact
recovery at baseline timestamps. Fresh final queue review found no P1.
Python syntax checks pass. No production code or binary was rebuilt.
