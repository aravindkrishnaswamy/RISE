# r249 — where r241's unburned methane goes

Measured 2026-10-16. **The missing fuel leaves predominantly through the top,
already cold and dilute.** It does not accumulate in the domain. The r239
prescribed burner floor is working. The next correction should address
combustion before dilution, rather than retain exhausted plume gas at the top.
No solver arithmetic was changed and no new simulation was launched.

## Measured fuel balance

Read all 257 archived r241 checkpoints plus the retained final checkpoint.
Their SHA-256 hashes match their existing publication sidecars. The archived
states are at
`/Volumes/Big Store/Archive/RISE/fire_production_calibration/r241_tier8_full_window.v1/checkpoint_history/`;
the final state and original summary are in
`/Volumes/SSD/RISE/fire_production_calibration/r241_tier8_full_window.v1/`.

The original 14.625 s statistics window received **9.647220 g**, not the
16.551028 g supplied over the entire 25.091 s run. It burned **6.214101 g**
(64.41%). Those two differently scoped summary fields must not be subtracted.
There is no retained conservative state exactly at the statistics boundary,
10.466040127157 s. Use the first checkpoint after it, step 6464, and the final
step 15309 for an endpoint balance covering **99.4717%** of statistics time:

| Quantity, 10.543308698034–25.091040127256 s | CH4 mass (g) |
|---|---:|
| Prescribed inlet, integrated over 14.547731429222 s | 9.596250605 |
| Chemical consumption, difference of retained cumulative statistics | 6.175158519 |
| Beginning inventory | 0.627147419 |
| Ending inventory | 0.596279195 |
| Inventory change | −0.030868224 |
| Inlet − consumption − inventory change | **3.451960310** |

The final row is **net export plus any unmeasured accumulated numerical
residual**, not a directly recorded historical boundary integral. Inventory
fell while fuel was supplied: storage cannot explain the combustion deficit.

All rows above use binary32 solver geometry. The recorded consumption uses
original-double staging volume, so it is multiplied by
`(dx_solver / dx_authored)^3 = 0.9999999773449917` before comparison with
inventory and represented inlet mass. This changes consumption by only
0.000000140 g, but is explicitly accounted for. Solver dx is
0.030593115836381912 m; authored dx is 0.030593116067411014 m.
The existing burner formula, summing represented per-face prescribed flux
with represented face area, gives **0.659638971976 g/s**. It independently
matches full-run external mass divided by duration to about 3e-13 relative.

## Boundary localization

At each of the 156 retained states in that interval, evaluate outward
advective donor flux from CH4 density and retained face-normal MAC velocity,
then integrate with trapezoids at the actual, irregular checkpoint times:

| Boundary | Sampled outward advective CH4 integral (g) |
|---|---:|
| Top | **3.451410140** |
| xmin | 0.000007140 |
| xmax | 0.000001468 |
| ymin | 0.000001494 |
| ymax | 0.000056370 |
| Bottom outward | 0 |

Top alone agrees with the balance remainder to **0.01594%**. All sampled
outward advective boundaries together leave a gap of **0.000483698 g**.
The floor has prescribed inward flux on the burner and wall flux elsewhere;
its zero outward value also agrees with the actual boundary construction.

**These are sampled endpoint donor estimates, not accepted-R1 observations.**
The owner uses a Heun pair with R1 scalars and R2 final momentum; retained
endpoint velocity cannot reconstruct that historical pair. Nonadvective
transport on pressure-open inflow faces is also omitted. Boundary
antidiffusion is zero in the current operator. Keeping both endpoints and
using every other checkpoint changes the top integral by **−0.5652%**;
therefore the close balance agreement is a useful localization cross-check,
not a 0.016% accuracy certificate or a new conservation tolerance. The
measurements support top escape as the dominant destination, while leaving
its exact historical accepted-flux integral unavailable.

## State of the escaping fuel

A separate read-only native probe used `LoadMethaneRunCheckpoint`,
`BuildIgnitionEligibility`, `ProductionEstablishedFlameHolderEligible`, and
`TrialAdiabaticTemperatureK` directly, including canonical pilot/source masks.
It independently reproduced the Python inventory and cumulative burn at
three retained states (inventory differences below 1e-15 kg).

| State time (s) | Top donor CH4 (g/s) | CH4-flux-weighted T (K) | CH4 mass fraction | O2 mass fraction |
|---|---:|---:|---:|---:|
| 10.543309 | 0.167542 | 321.88 | 0.000317 | 0.230526 |
| 17.869027 | 0.228471 | 331.09 | 0.000430 | 0.229475 |
| 25.091040 | 0.232341 | 329.25 | 0.000406 | 0.229714 |

At all three states, **zero** top methane donor flux comes from cells above
the 600 K pilot threshold, from eligible cells, or from cells whose complete
adiabatic burn could reach the 1700 K critical flame temperature. This is
oxygen-rich, over-diluted methane slip, not fuel waiting for oxygen at the
outlet. Across all 156 statistics snapshots, all outward top donor CH4 is
below 800 K (the additional three native probes establish the stronger
600 K/CFT result only at those three times).

At the final state, 0.596279 g of methane remains in the domain; only
0.036208 g is eligible for reaction, and only 0.063732 g passes the complete
adiabatic flame-temperature trial. These native inventory values use authored
volume. Most retained fuel is ineligible to burn in its measured grid-mean state
under the current local closure.
This does not by itself distinguish the upstream roles of thermal/constituent
transport, the ignition graph, the mixing rate, and grid-mean extinction.

## Proposed fix

**Correct the treatment of mixing and combustion within the established
flame region, before methane becomes a cold, over-diluted grid mean.** The
candidate is a conservative subcell mixing/combustion closure: evaluate the
reactive mixed portion and its flame-temperature trial at flamelet/subcell
conditions, rather than allowing an entire mixed cell to become ineligible
from its averaged temperature/composition before consuming its entrained
fuel. Restrict this treatment to the established flame; preserve the ignition
barrier for remote cold pockets. Consume only available fuel and oxygen and
apply the existing atom-balanced species, enthalpy, expansion and radiation
updates from that one consumption amount.

This is a **fix proposal, not a demonstrated causal correction**. The
measurements identify the destination and terminal extinction state; they do
not establish that changing `ComputeMixingTimeS` alone, changing graph seeds,
or a particular subcell formulation will recover the missing heat. The
current gate in `FireProductionSourceKernel.h:BuildIgnitionEligibility`
requires both T > 600 K and a 1700 K adiabatic trial before the exponential
mixing-limited reaction can act. Simply reducing the mixing time cannot burn
an ineligible cell. A closure change must therefore address established-flame
mixing/extinction together, with physical justification, rather than tune a
rate to match 33 kW.

Do not seal the top, enlarge the domain as a combustion remedy, raise inlet
fuel, or lower the global ignition/CFT thresholds to burn the dilute outlet.
Those changes do not address the measured upstream underburn. A corrected
kernel must run tier 8 from zero with unchanged 33 kW input. Its tangible
acceptance measurements are higher consumed fuel, lower top CH4 export,
closed inlet/consumption/inventory/export balance, and no compensating storage
increase. Only then spend the tier-10 window on the McCaffrey check.

## Evidence and scope

- [Retained samples and checkpoint SHA-256s](data/fire_r249/retained_fuel_samples.csv)
- [Machine-readable balance, native checks, and artifact SHA manifest](data/fire_r249/fuel_balance.json)
- One-off readers, native probe source/binary, empty warning-free build log,
  and native observations remain outside git in
  `/Volumes/SSD/RISE/fire_production_calibration/r249_tier8_fuel_balance/`.
  Reproduce the retained reduction there with the bundled Python plus NumPy:
  `python3 r249_reduce.py`, then `python3 r249_report.py` (the reader module
  `r249_inspect.py` is in the same directory). These are measurement scripts,
  not a new production diagnostic interface. The native probe takes one
  checkpoint path, reads it, and prints scalars; it never steps or writes a
  simulation. Its build recipe is retained beside it.
- No production source, kernel, checkpoint, frame, or original sidecar was
  modified. No full build/test qualification or changed-solver result is
  claimed; the temporary native reader compiled and linked without warnings.

One independent review round reproduced the balance and sampled integrals,
checked geometry and temporal scope, and found no material issues. Its wording
clarification about present-state eligibility is incorporated.
