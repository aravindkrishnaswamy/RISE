# r251 — Picard stop at an output-event sliver

The owner accepted r250's partial-window combustion result. This task addresses
its numerical stop, within an approximately one-day diagnosis/fix time box,
then repeats tier 8 from zero at unchanged 33 kW. No tier 10 before review.
Diagnosis began 2026-10-18 at 13:36 UTC (06:36 PDT).

## Reproduction and classification

The retained input is r250 step 13,539 at **21.441342722 s**. A bounded native
probe resumes its physical state using the existing isolated loader and writes
only to `r251_picard_diagnostic`, leaving r250's checkpoint and sidecar intact.
The production operator, boundary conditions, fuel source, 16 Hz event cadence,
maximum source step and eight workers match r250. Observers print residual
components without changing solver arithmetic. The probe reproduces the stop
at step **13,555, 21.466040127 s**, including the original residual history.

**Classification (b): residual stagnation near tolerance at a microscopic
output-event timestep.** The failed R2 stage has stable open-boundary classes
throughout; no class cycle is frozen. The existing R0/R1 two-class treatment
remains in use. This is not evidence of a new flame-edge expansion instability:
the iteration contracts rapidly before reaching a small, nearly fixed floor.
The observed face momentum differences are around 1.55×10⁻⁵ kg/(m² s), with
ordinary O(1) face momenta, not growing excursions.

The prior report mistook the error's `target=0.000056` for a convergence
threshold. That field was the **change in the assembled divergence target**.
The actual unchanged Picard tolerance is **0.000477735593**. Failure text now
names component changes explicitly and includes the tolerance.

| Same failing state, diagnostic Δt | R2 iterations 0 / 1 / 2 | Final residual, iteration 63 |
|---|---|---:|
| 3.757172635 µs | 2.89896393 / 0.630496979 / 0.00589458924 | 0.000506558630 |
| 1.878583703 µs (approximately half) | 3.24022675 / 0.694971204 / 0.00603291858 | 0.000502662035 |
| 0.939291851 µs (approximately quarter) | 3.94151306 / 0.854669631 / 0.0130468076 | 0.000502662035 |

The original plateau is predominantly 0.000506559, with excursions across
0.000502662–0.000753019 after iteration 2. Halving/quartering Δt does not
remove it. The small target and transport-coefficient changes are recorded
separately. All iterations, not only this summary, are in the
[diagnostic](data/fire_r251/diagnostic_iterations.csv),
[half-step](data/fire_r251/half_iterations.csv) and
[quarter-step](data/fire_r251/quarter_iterations.csv) CSVs.

**Original Δt retries attempted: zero.** Candidate 0 failed; the resident
attempt returned no accepted token or suggested next timestep, so the existing
launcher stopped. The two smaller-Δt cases above are separate diagnostics from
the reproduced same failing checkpoint, not retries that occurred in r250.

## Smallest correction and bounded check

The previous ordinary step was **1.646265970 ms**. It landed at
21.466036712 s, leaving only **3.415611445 µs** to the next output event.
The existing 1.1 growth cap then selected the failed 3.757172635 µs successor.
A requested output time should not force this unnecessary small-step solve.

`LimitBinary32ProductionStepToEvent` now divides the final remaining interval
between two balanced steps whenever it lies between one and two admissible
steps. The first is rounded downward to Binary32; the existing outward rounding
lands the final step at the event. The first remains within the admissible step; the second retains the
existing outward event rounding and satisfies the growth bound in the regression. This uses the interval itself, not an epsilon,
new tolerance or minimum physical timestep. The Picard and projection algorithms,
64-iteration cap, tolerances, source/energy arithmetic and flame-edge gate are
unchanged. The numerical-policy identity is v5 to record the timestep change.

The corrected retained-state probe accepts **64 steps**, reaching
**21.541635784 s** and crossing both the original stop and another output event.
After the original boundary, R2 converges in at most four main iterations;
its largest last main-iteration residual is **2.07007142×10⁻⁵**, below the
unchanged tolerance. [Complete balanced history](data/fire_r251/balanced_iterations.csv).
No rejected candidate is published. This bounded probe is diagnostic evidence,
not a replacement for a from-zero run.

The regression uses the real interval and a range of interval/step ratios to
check two-step landing, growth/step bounds and unchanged distant events. It
fails against the former helper and passes with the change. Sibling audit:
both production call sites (pilot ramp and the shared event lambda for
pilot-end, statistics, frame and target events) use the helper. The Binary64 oracle's independent
selector is unchanged; it does not have this Binary32 output-rounding path.

## FDS Validation Guide flame-height definition

The tagged [FDS Validation Guide, Flame Height section](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Validation_Guide/Plume_Chapter.tex#L910-L930)
uses instantaneous vertically integrated HRR to find a height containing a
specified fraction of the instantaneous total. Its Heskestad comparison uses
**99%**, then averages height in time. Experimental flame height instead uses
50% luminous intermittency. The [validation input](https://github.com/firemodels/fds/blob/FDS-6.11.1/Validation/Heskestad_Flame_Height/FDS_Input_Files/Qs%3Dp1_RI%3D10.fds)
confirms a 0.99 percentile control and running temporal average; the
[control implementation](https://github.com/firemodels/fds/blob/FDS-6.11.1/Source/ctrl.f90#L407-L429)
normalizes to the instantaneous realized integral and interpolates the crossing.
This is different from the earlier nominal-input H99 of an averaged profile.

A read-only native reader integrates every retained VDB `reaction` plane and
interpolates percentile heights within the cell layer, then trapezoidally
averages those heights. It consumes all **177 frames at 16 Hz**,
10.466040127–21.466040127 s. It does not step or recompute chemistry.

| r250 partial-window definition | Height |
|---|---:|
| **FDS: mean instantaneous 99%-of-realized-HRR height** | **0.690166 m** |
| Heskestad at 33 kW, D=0.30 m | 0.645642 m |
| FDS value relative to Heskestad | +6.90% |
| Previous nominal-input H99 of accepted mean profile, through 21.441343 s | 0.782710 m |
| Nominal-input H99 of matched 177-frame mean profile | 0.719907 m |
| Realized-HRR H99 of that matched mean profile | 0.694934 m |

The matched rows separate normalization and averaging order from sampling.
The 16 Hz frame mean HRR is 32.9441 kW, versus 32.8395 kW in the every-step
retained integral: the 0.32% difference and slightly different endpoint make
the nominal-input 99% tail height especially sensitive. The FDS mean height
is a sampled estimate, not an every-step temporal integral or a luminous
intermittency measurement. [Frame data and hashes](data/fire_r251/r250_fds_flame_height.json).

## Full tier-8 rerun

From zero, unchanged 33 kW and 0.30 m burner, target 25.091040127 s,
statistics start 10.466040127 s and duration 14.625 s. Output:
`/Volumes/SSD/RISE/fire_production_calibration/r251_tier8_full_window/`.
Because the timestep trajectory changes, the empirical run starts from zero.
No checkpoint migration or resumed acceptance run is used.

Report: consumed/supplied and full balance; actual accepted top methane flux;
heat fraction below 1.5 m and FDS mean flame height alongside nominal-input H99;
accepted realized HRR versus 33 kW; and native centerline puffing spectra with
three overlapping subwindow spectra averaged, alongside the full spectrum.
The existing accepted ledger, stored plane integral and native source probes
supply those observables.

## Prelaunch verification

Clean make, Xcode Deployment and shipping Opto builds succeed. Make and changed
source compilation have no warnings; Xcode reports only the previously
recorded external macOS 27 Homebrew-library link warnings and AppIntents metadata.
Event, parcel ignition, fuel budget, case, projection, accepted-ledger reducer and
burner-launcher tests pass. The projection test's first timing assertion failed
while Xcode was building; its one uncontended repeat passes (46.0 ms device,
62.5 ms wall p95 against the existing 120 ms budget). No timing limit was changed.
The pure event regression was also compiled against the old helper (exit 1)
and corrected helper (exit 0).

Status: bounded diagnosis and prelaunch checks passed; independent review next.
