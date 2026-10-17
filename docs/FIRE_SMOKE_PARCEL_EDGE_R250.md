# r250 — tier-8 one-cell parcel-edge test

Selected gate: **(e)**. The [offline comparison](FIRE_SMOKE_TIER8_EDGE_R249.md)
measured 0.136754 g eligible methane, almost entirely below 1 m. The shared
source predicate, graph and both floor-holder callers implement that gate;
actual species consumption, mixing time, energy, expansion and radiation
updates are unchanged. The production numerical-policy identity is v4.

Run: tier 8, from zero, 0.30 m methane burner, unchanged 33 kW input, original
25.091 s duration and 14.625 s statistics window. Output directory:
`/Volumes/SSD/RISE/fire_production_calibration/r250_tier8_parcel_edge/`.
No tier-10 run is authorized before the resulting owner review.

## Acceptance measurements

All three must hold; none is established by offline eligibility:

1. Statistics-window fuel consumption ≥95% of supplied fuel, with the measured
   inlet/consumption/inventory/export balance closing.
2. Near-zero top methane export, measured from the actual accepted R1 Heun
   boundary flux, not retained endpoint donor estimates.
3. Most time-averaged heat below about 1.5 m and mean flame height near the
   Heskestad value, `0.235*33^(2/5)-1.02*0.30 = 0.646 m`.

The ordinary tier-8 capstone now enables the existing accepted-stage ledger.
It exports only the small step ledger, without the old lateral-study spatial
capsules/columns (which require even horizontal grid extents). The statistics
inventory at a clipped first step retains the existing explicit linear-
increment definition. No new memory or admission system is introduced.

The existing per-plane accepted source integral is exported as
`budgets/heat_release_height.csv`, covering every statistics step. Use the
[FDS flame-height proxy, p. 7](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=907227):
integrate the mean profile until 99% of the **nominal input** heat is reached.
If unreached, report the height unavailable. Also report the 99% realized-heat
quantile as a separate descriptive result. It must not hide incomplete burning.
The HRR proxy differs from the visual definition underlying Heskestad.

Postprocess with `tools/fire/r250_acceptance.py <output-dir> --output <json>`.
The measured residual and top-export fraction are reported directly; no new
conservation tolerance or near-zero cutoff is selected to obtain a pass.

## Verification before the run

The graph regression fails on the former gate and passes with the parcel
edge. Coverage includes a cold chain that cannot extend itself, all six face
neighbors, exclusion of a diagonal and a disconnected seedless pocket, and
an independent unnormalized FDS enthalpy oracle over lean/rich mixtures.
Accepted-budget reduction and the native burner launcher checks pass. The
solver suite retains its single documented V6 owner-history convergence
failure; it is not turned into a prerequisite for this physics experiment.

Clean make rebuild has no warnings. Xcode Deployment and Opto clean builds
succeed; local Homebrew libraries built for macOS 27 produce deployment-target
link warnings against the project's macOS 26.2 target. The unrelated AppIntents
metadata warning is also present. No changed-source compiler warnings remain.
Two small clean-build repairs migrate deprecated Metal family queries and
exclude make's generated dependency file from Xcode's synchronized source set.
They do not change simulation arithmetic or Apple-device family selection.

Status: verification/review preceding launch; acceptance measurements pending.
