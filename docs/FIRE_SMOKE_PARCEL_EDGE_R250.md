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

## Launch

Started 2026-10-17 at approximately 12:50 UTC from committed `ed6757e23`
(after rebuilding the executable to refresh its build identity). Both independent
reviewers found no P1/P2 issues. Command:

```sh
./bin/tests/FireSequenceTest --fire-production-temporal-capstone 8 0.0625 /Volumes/SSD/RISE/fire_production_calibration/r250_tier8_parcel_edge/
```

The sibling `r250_tier8_parcel_edge.log` contains progress and the existing
32 GiB memory guard. First accepted step: time 0.001646266 s, RSS 1.87 GB,
no memory stop. The prior tier-8 window took 21.8 hours; this run's elapsed
cost remains to be measured.

Status: **run stopped incomplete at 21.466 s**, exit 94; see the measured
accepted-prefix results below. No tier-10 run has been started.


## Early retained-checkpoint trend (not final acceptance)

At the owner's request, a read-only reduction uses the retained checkpoint
endpoints nearest 6 and 10 s: steps 3730 and 6148, at 5.993228656–9.973831141 s.
The matching immutable accepted-stage ledgers supply both inventory and exact
accepted R1 boundary/source totals. No solver replay, build, restart or run
mutation was performed. This precedes the 10.466040127 s statistics boundary.

| Methane quantity | Grams |
|---|---:|
| Supplied (all inward boundaries) | 2.625777421 |
| Prescribed burner supply | 2.625760531 |
| Burned | 2.618533724 |
| Initial inventory | 0.073505712 |
| Final inventory | 0.067300743 |
| Inventory change | −0.006204968 |
| Top outward export | 0.013431603 |
| All outward export | 0.013448615 |
| Balance residual, Δinventory + burned − supplied + exported | −0.000000051 |

**Burn fraction: 99.724131%.** Top outward export is **0.511529%** of supply.
The residual is −1.94×10⁻⁸ of supply. Top inward methane is 0.000002257 g;
all inlet directions are retained in the balance rather than silently discarded.
Interstep inventory change is zero.

| Retained interval, s | Burned / supplied |
|---|---:|
| 5.993–6.981 | 100.561% |
| 6.981–8.005 | 98.544% |
| 8.005–9.019 | 100.019% |
| 9.019–9.974 | 99.811% |

Values above 100% draw from stored inventory. These early results support the
correction but do not replace any of the three full statistics-window criteria.
[Small data and endpoint provenance](data/fire_r250/early_fuel_balance.json)
and the five endpoint ledgers are retained in git. At that point the simulation
was left running; its subsequent stop is recorded below.


## Run outcome — incomplete, stop for review

The from-zero run accepted **13,555 steps to 21.466040127 s**, then exited 94.
The target was 25.091040127 s. Only **11.000 of 14.625 statistics seconds
(75.214%)** were completed. The stop receipt records a projected-Heun resident
Picard **stage-2 nonconvergence**, momentum residual 0.000507 against target
0.000056 (transport residual 0.000023). This was a solver stop, not the RSS
ceiling. No restart, timestep adjustment or solver change was made afterward.

The failure followed a 3.4156 µs accepted step landing on the 21.466040127 s
frame boundary. That is a diagnostic observation, not a demonstrated cause.
The complete log, checkpoints and original stopped output remain in the same
run directory. There is no successful capstone summary or final checkpoint.

### All three requested measurements

These are **accepted-prefix observations, not full-window acceptance**.
Fuel/export use every accepted statistics increment through 21.466040127 s.
Heat uses the last retained checkpoint, step 13,539 at 21.441342722 s, covering
10.975302595 statistics seconds. Its matching ledger is also retained so the
slightly different endpoints are explicit.

| Requirement | Measured accepted prefix | Full-window verdict |
|---|---|---|
| ≥95% consumed and balance closes | **99.500462%** burned; residual −6.31×10⁻⁸ g (−8.70×10⁻⁹ of supply) | Incomplete; prefix meets the fuel threshold |
| Near-zero top methane export | **0.035365277 g**, **0.487387%** of supply | Incomplete; approximately half a percent in the prefix |
| Most heat below ~1.5 m, height near Heskestad | **99.9999916%** below 1.5 m; nominal-input H99 **0.782710 m** vs **0.645642 m**, **21.23% high** | Incomplete; vertical localization is good, height remains above prediction |

The fuel balance in grams is:

`7.256096185 supplied = 7.219849259 burned + 0.000859449 inventory gain + 0.035387414 outward export + 0.000000063 residual correction`.

Inventory starts at 0.071400516 g and ends at 0.072259964 g. All boundary inflows
are included; prescribed burner supply alone is 7.256028692 g. Top inward
methane is 0.000026862 g and interstep inventory change is zero. The signed
residual definition remains `Δinventory + burned − supplied + outward`.

The matched retained-checkpoint prefix burns 99.512739% of supply. Its mean
HRR is **32.839510 kW**; **99.918120%** of heat lies below 1 m. The separate
99%-of-realized-heat quantile is **0.694606 m**, but the preregistered
99%-of-nominal-input proxy, 0.782710 m, is the acceptance comparison. No height
tolerance is invented after seeing the result. Both quantities are heat-profile
proxies, distinct from the experimental visual intermittency definition.


| Height band | Mean heat, kW | Fraction of realized heat |
|---|---:|---:|
| 0–0.25 m | 25.902242179 | 78.87524043% |
| 0.25–0.5 m | 5.603997704 | 17.06480324% |
| 0.5–1 m | 1.306380660 | 3.97807603% |
| 1–1.5 m | 0.026886333 | 0.08187191% |
| 1.5–3.24287 m | 0.000002757 | 0.00000840% |

The existing native probe's `--retained-profile` mode reads the checkpoint's
stored plane integral and duration; it does not recompute chemistry, step the
solver, or modify the checkpoint or accepted ledgers. The read-only extraction was compiled only after
the simulation had exited, with no compiler warnings. Its output goes to a
separate CSV. Source, endpoint ledgers, profile, hashes and failure excerpt are
in [the small evidence set](data/fire_r250/incomplete_acceptance.json).
Independent review recomputed both balances, height quantiles, profile totals
and provenance hashes with no P1/P2 findings. The complete-window reducer deliberately remains strict and cannot report this
failed run as a completed acceptance window.

**Overall: not passed because the run is incomplete. Stop for owner review
before any restart or tier 10.** The next issue to investigate is this solver
nonconvergence, including the frame-boundary step history; the fuel correction
has encouraging prefix measurements but no completed dynamic acceptance.
