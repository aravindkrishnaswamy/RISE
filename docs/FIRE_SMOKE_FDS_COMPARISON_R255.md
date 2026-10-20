# r255 — FDS matched-grid comparison completed

2026-10-19. The owner accepts the FDS comparison as evidence that the entrainment
deficit is specific to RISE. **Tier 6 is cancelled.** No production solver
source or kernel was changed. FDS used the approved unmodified 6.11.1 native
build and the matched physical input documented in
[FIRE_SMOKE_DISCRIMINATING_RUNS_R255.md](FIRE_SMOKE_DISCRIMINATING_RUNS_R255.md).

Statistics: 10.466040127212–25.091040127212 s. Source input is 33 kW; realized FDS
HRR is 32.9934 kW and radiative loss 6.7162 kW. r251 realized 32.837 kW.
McCaffrey uses exactly r252's selected native stations and nominal 33 kW.

| Quantity | r251 | FDS 6.11.1 | Reference |
|---|---:|---:|---:|
| Maximum temperature-rise error | +87.29% | −14.10% | McCaffrey |
| Maximum vertical-velocity error | +25.01% | −15.08% | McCaffrey |
| Plume mass at 0.994 m (kg/s) | 0.24148 | 0.30056 | 0.24833 |
| Plume mass at 1.514 m (kg/s) | 0.36982 | 0.49042 | 0.45769 |
| Plume mass at 2.004 m (kg/s) | 0.48618 | 0.72153 | 0.70596 |
| Plume mass at 2.493 m (kg/s) | 0.60248 | 0.99957 | 0.99920 |
| Plume mass at 3.013 m (kg/s) | 0.74236 | 1.31127 | 1.35644 |

FDS reaches 96.7% of the correlation at 3.013 m; r251 reaches 54.7%.
The FDS face-flux versus r251 cell-product quadrature distinction remains
explicit in the launch report and JSON; it is not an exact discrete-operator
match. The whole-plane upward bounds are 1.36198 / 0.79936 kg/s respectively,
so the large difference is not created by the R<0.8 m integration mask.

Profiles, source hashes and reduction results are in `docs/data/fire_r255/`.
Large FDS slices and logs remain in the external `r255_comparison/` directory.

## Separate low-priority issue: restart non-identity

The first observer continuation stopped at accepted step 9,276,
t=15.104210254969075 s: **15,043 differing binary32 Q/T/M/U values after 52
steps** from the step-9,224 checkpoint. Timesteps matched. The remaining
four-second interval and tier 6 were not run. Evidence:
`r255_comparison/limiter_run/equivalence.csv` and `limiter_run.log` externally.

The owner explicitly defers this issue and waives the bit-identity requirement
for the next statistical diagnostic. **Do not investigate it now**, alter
solver tolerances or reopen accepted r251 combustion because of this issue.
