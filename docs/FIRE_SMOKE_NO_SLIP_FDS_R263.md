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

## Completed result

FDS completed25.091s, exit0, in5384.6wall seconds (1h29m45s).
The serial queue finished at17:57 PDT on2026-10-28 and stopped for review.
Early physical fields differ: at0.081375s, bottom temperature has67 differing
values (maximum6.48e-5K) and W has4899 (maximum8.80e-6m/s). At0.135625s
these are130/4899, with maxima0.002581K/2.86e-5m/s. Those timestamps
happened to match the retained baseline endpoints, so interpolation fractions
are1.0. These remain retained-state checks, not internal-two-step checks.

**Upward mass flux, radius0.8m, kg/s:**

|Height|RISE r251|FDS default wall|FDS no-slip|Change from default|Default-to-RISE gap closed|
|---:|---:|---:|---:|---:|---:|
|0.291m|0.09301|0.13997|0.14186|+1.35%|−4.0%|
|0.505m|0.14370|0.18472|0.18186|−1.55%|7.0%|
|0.994m|0.26331|0.31063|0.28769|−7.38%|48.5%|
|2.004m|0.51287|0.74222|0.67690|−8.80%|28.5%|
|3.013m|0.77801|1.30388|1.18500|−9.12%|22.6%|

At0.29–0.5m, matching the tangential wall condition does **not** substantially
move FDS toward RISE: the upward-flow gap grows slightly at0.29m and closes
only7% at0.5m. By1m, however, it removes nearly half the upward-flow gap.
The boundary treatment is a material downstream contributor in this measured
window, but does not explain the majority of the in-flame base deficit.
No-slip FDS still entrains substantially more than RISE at every height.
This is one finite, nonstationary realization, not an ensemble causal fraction.

### Wall-function proposal for review only

Because the1m gap movement is substantial, a matching wall function is a
reasonable secondary modeling proposal, **not a demonstrated fix for the
0.29–0.5m deficit**. For a future RISE experiment, use the FDS smooth-wall
LES law to compute friction velocity and tangential wall shear from wall-normal
distance, molecular viscosity, density and tangential gas speed. Apply it on
both the surrounding floor and burner, with zero wall tangential target.
Preserve burner normal mass flux, elevation, thermal/species conditions and
pressure projection. Use the resulting wall shear/ghost treatment consistently
in momentum stresses and transport/SGS velocity gradients; do not merely add
an unrelated drag force or change only one gradient consumer. Retain Vreman
and scalar transport initially to isolate the wall treatment. Matching FDS's
separate near-wall eddy-viscosity choice would be a distinct experiment.

Before adoption, verify the external experiment changes accepted fields,
repeat onset safety at tiers8/10, and run the tier8 case from zero for the
full combustion/heat/centerline/entrainment window. These are proposed future
checks, not launched work. **No wall-function implementation or production
change. Stopped for review.**

Measured CSV and JSON: `docs/data/fire_r263/upward_mass_comparison.csv` and
`results.json`; full-plane upward and net radius0.8m results are included
separately. Do not mix these full-statistics-window upward values with the
short-continuation net fluxes used to select centered momentum.

## Preparation checks

Input/measurement review found no P1. Queue review found one P1: requiring
identical early slice timestamps could reject a valid FDS variant, because
outputs occur on accepted steps. Fixed by interpolating baseline fields to
B's actual retained times, with explicit bracketing. The native-record reader
and interpolation were checked against r261 retained outputs, including exact
recovery at baseline timestamps. Fresh final queue review found no P1.
Python syntax checks pass. No production code or binary was rebuilt.
