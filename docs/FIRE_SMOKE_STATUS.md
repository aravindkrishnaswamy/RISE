# Fire & smoke — current status (start here)

Last updated 2026-10-19, during the authorized r255 sequential comparison runs. This page
is the entry point for any new fire/smoke session. The `FIRE_SMOKE_*_R2xx.md`
pages are a historical log, not instructions.

## Where things are

| What | Location |
|---|---|
| Code | branch `fire-smoke-design` in `/Volumes/SSD/Working/RISE` |
| Design | [FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md), [FIRE_SMOKE_PRODUCTION_SOLVER.md](FIRE_SMOKE_PRODUCTION_SOLVER.md), [FIRE_SMOKE_DESIGN_HISTORY.md](FIRE_SMOKE_DESIGN_HISTORY.md) |
| Current run results | `/Volumes/SSD/RISE/fire_production_calibration/` (campaign outputs, listed below) |
| Golden oracle checkpoint | `rendered/fire_methane_capstone/tier10.run.checkpoint` in the main checkout, SHA-256 `1b944176…cb4947` |
| Oracle reference runs (tier-8 matched reference, tier-10, equal-time compositions, r108 spatial) | `/Volumes/Big Store/Archive/RISE/oracle_references/` |
| Restart snapshots of r237 (tier 10) and r241 (tier 8) | `/Volumes/Big Store/Archive/RISE/fire_production_calibration/`; delete once r250 and a new tier-10 run supersede them |
| Removed branches and uncommitted work | local tags `archive/2026-10-16/*` (never pushed) |

## What works

- The Metal production solver runs a full 25 s McCaffrey methane window
  (0.30 m burner, 33 kW) from zero with no retries, using the ported §3.7
  compatible-momentum operator.
- Launch a window with
  `bin/tests/FireSequenceTest --fire-production-temporal-capstone <tier> 0.0625 <output-dir>`.
  It writes checkpoints, 235 frames over the statistics window, a sequence
  manifest and `production_capstone_summary.txt`.
- Measured cost on this Mac Studio:

| Tier | Steps | Wall time | Per step |
|---|---:|---:|---:|
| 8 (r241 baseline) | 15,309 | 21.8 h | 5.1 s |
| 8 (r251 parcel edge + balanced events) | 15,348 | 30.14 h | 7.07 s |
| 10 | 16,298 | 38.8 h | 8.6 s |

- Render a sequence through the spectral path with
  `tools/fire/r242_render_movie.py` and the scene template
  `tools/fire/r242_high_quality.RISEscene`. The tier-6 puffing spectra were
  produced with `tools/fire/r244_spectrum_report.py`.

## Measured results

The statistics window is 14.625 s. Supplied heat is 33 kW.

| Quantity | Tier 10 (r237) | Tier 8 (r241) | Target |
|---|---:|---:|---:|
| Centerline puffing frequency | 3.07 Hz | 3.41 Hz | 2.74 Hz, within 20% |
| Mean realized heat release | 19.0 kW (58%) | 21.3 kW (64%) | 33 kW |
| Integrated radiative fraction | 0.199 | 0.199 | 0.20 |
| Heat release vs fuel consumed × LHV | exact | exact | exact |
| McCaffrey max temperature rise error | −45% | not extracted | within 10% |
| McCaffrey max velocity error | −28% | not extracted | within 10% |

Tier 10 ran before the r239 burner-boundary fix; r241 tier 8 ran after it.
These are historical baselines; the corrected r251 result is below.

## r249–r251: combustion and Picard stop resolved

The r237/r241 baselines below burned only 58–64%. r249 localized the lost
fuel to cold, dilute top methane export and selected the stoichiometric-parcel
CFT plus one-cell warm-edge waiver. The owner accepted r250's partial-window
combustion result. r251 reproduced its stage-2 Picard stop: a microscopic output
alignment step and its growth-limited successor produced residual stagnation
near unchanged tolerance. Two balanced event steps remove that trigger.

[r251 full from-zero tier-8 run](FIRE_SMOKE_PICARD_EVENT_R251.md) completed
**25.091 s, 15,348 accepted steps**, all **14.625 statistics seconds**, exit 0.
All three combustion checks are met:

- Fuel consumed **99.5063%**, 9.599681 g / 9.647308 g supplied; balance residual
  **−1.48×10⁻⁷ g**. Inventory gain 0.000997 g; all export 0.046630 g.
- Top methane export **0.046600 g (0.4830%)**, at the owner-accepted near-zero scale.
- Essentially all heat below 1.5 m. FDS Validation Guide mean flame height
  **0.683 m**, vs Heskestad **0.646 m** (+5.84%). Nominal-input profile H99 is
  **0.779 m** and is reported separately.

Realized HRR **32.837 kW (99.51% of 33 kW)**, radiative fraction **0.19889**.
The averaged subwindow puffing peak is **3.003 Hz** (+9.64% vs 2.739 Hz), but
the full-window peak is **4.504 Hz** and individual windows differ. Do not
claim a single stationary mode. Diagnosis/fix/review took about half an hour;
the full simulation took 30.14 h. The owner accepted r251 combustion, realized
HRR, flame height and averaged puffing.

[r252 centerline extraction](FIRE_SMOKE_CENTERLINE_R252.md), using exactly the
r237 station-selection rule and 33 kW reference, gives maximum temperature-rise
error **+87.29%** and vertical-velocity error **+25.01%**, against r237’s
−45.05% / −28.14%. Both exceed the owner’s 15% condition. **Do not launch
tier 10.** Complete station profiles and extraction hashes are committed with
the report; no new simulation was run for this comparison.

[r253 retained-state diagnosis](FIRE_SMOKE_PLUME_DIAGNOSIS_R253.md) finds
**insufficient entrainment** with essentially the correct convective heat:
at 3.013 m, 0.7424 kg/s net plume mass against 1.3564 kg/s from Heskestad at
26.4 kW convective power; even all upward flow in the entire plane is only
0.7994 kg/s. The requested convective cp flux is 26.347 kW. Side bands remain
exactly 300 K through 3.044 m; tiny positive outlet-zone temperatures cool
through the window rather than build up. Width and reconstructed Vreman
viscosity profiles are in the report. **No wider-domain test or other new run;
no solver change. Stopped for review.** Seven r251 scratch folders (including
Xcode and Picard probes) and 32 scratch files were deleted; the accepted run,
retained states, production log, committed evidence and movie are preserved.


[r254 turbulence audit](FIRE_SMOKE_TURBULENCE_DIAGNOSIS_R254.md) measures upper
centerline RMS/mean ratios **0.200 for vertical velocity and 0.266 for temperature
rise** at 3.013 m, against Shabbir & George's 0.33/0.40. Half-width ratios are
0.424/0.491. Vreman Cv=0.07, filter width and formula match FDS; independent
recomputation agrees with the production evaluator to about 6.6×10⁻⁸ relative
L1 difference. Historical face limiter alphas/stage flux pairs were not retained,
so their activity and the complete momentum update's damping cannot be measured
from these states. The active compatible flux has centered velocity and no
independent upwind-velocity blend. **No limiter-damping cause or solver fix is
established; solver unchanged, no new run. Stopped for review.**

[r255 discriminating runs](FIRE_SMOKE_DISCRIMINATING_RUNS_R255.md) are in progress.
The owner approved installing FDS 6.11.1; its unmodified source was built natively
and the matched physical case is running. A bounded serial batch will then run
an observer-only ~4 s continuation from r251 step 9,224 (first checking original
retained Q/T/M/U states bit-for-bit), followed by canonical tier 6 from zero
with the existing binary. Results and comparison table are pending. Production
solver sources and kernels are unchanged; tier 10 remains unlaunched. Live
stage and outputs are in `r255_comparison/campaign_status.json` under the external
campaign directory. Stop for owner review after the comparison.

## Open problems, in priority order

1. **Upper-plume centerline profiles fail the tier-10 gate.** r251 is too
   hot and too fast above the flame relative to McCaffrey at 33 kW. See
   [the profiles](FIRE_SMOKE_CENTERLINE_R252.md). The
   [r253 measurements](FIRE_SMOKE_PLUME_DIAGNOSIS_R253.md) identify an
   increasing entrainment deficit with height, rather than bulk ambient warming
   or excess transported heat. Which turbulence/transport/domain term causes
   that deficiency is not yet isolated. r254 finds reduced resolved fluctuations
   but no Vreman constant/formula mismatch; unsaved stage alphas prevent a
   historical limiter-activity measurement.
   Tier 10 remains unlaunched under the owner’s conditional instruction.
2. **The fire renders blue.** The methane record's gross soot yield is 0,
   and `FireSimulationRecords.cpp` enforces that. With no soot there is no
   incandescence, only blue chemiluminescence. The zero is methane's
   escaping smoke yield; real methane flames at this scale form roughly
   0.1–1 ppm soot in the flame and burn it out. The fix is a soot
   formation–oxidation closure calibrated to published in-flame data.
   [r252 evidence](FIRE_SMOKE_METHANE_SOOT_DATA_R252.md) records a near-scale
   37 cm/34.5 kW measured axial soot mass-fraction profile (peak 0.0008 kg/kg
   at 0.14 m) and a smaller-flame direct LII volume-fraction profile (local
   ensemble mean peak about 0.5 ppm). Mass and volume fractions are kept
   distinct. The [formation–oxidation draft](FIRE_SMOKE_SOOT_FORMATION_OXIDATION_R252.md)
   retains existing oxidation and proposes fitting gross formation to in-flame
   data; the
   coefficient remains unfitted pending owner review and calibration.
   **No soot implementation or simulation has been made.** The
   [r251 preview](FIRE_SMOKE_PREVIEW_R252.md) uses the r242 spectral scene at
   the owner-selected 128×192, denoising enabled; all 235 frames and the
   16 fps MOV completed and passed full decode verification.
   Radiation is budget-partitioned at χᵣ = 0.20, so this changes the
   spectrum and spatial radiation allocation within that budget. The proposed
   chemistry must reserve carbon chemical energy at formation and release it
   on oxidation; unchanged χᵣ does not imply unchanged solver dynamics.
3. **Puffing has substantial window dependence.** r251's three-window mean
   peaks at 3.003 Hz, within 20% of the 2.739 Hz burner law, while individual
   overlapping windows peak at 2.866/4.504/3.003 Hz and the full periodogram
   peaks at 4.504 Hz. Review the native spectrum before assigning one mode.
   Do not reopen the tier-6 domain study: its burner resolution is inadequate.
4. **Cost.** Tier 10 takes about 39 hours. Faster steps would make the
   iteration loop practical, but cost work comes after the outstanding physics items.

## Known failing tests (pre-existing)

The branch builds warning-free and every test target links. These 27
failures were already present in the committed codex code before the
2026-10-16 cleanup; a baseline build of the pre-cleanup commit produced the
identical failure set. Hygiene, case, records, optics, projection and
fuel-budget tests pass.

| Test | Failures | Nature |
|---|---:|---|
| `FireSimulationSolverTest` | 1 | V6 owner history: R0 augmented Newton system does not converge under timestep refinement. Investigate first; it may be a real numerics regression. |
| `FireProductionSolverTest` | 12 | Earlier-milestone gates the later code outgrew: a 2 GiB resident working-set cap the observer additions now exceed, 200 ms timing gates, and source-topology bindings. |
| `FireProductionCalibrationTest` | 13 | Historical evidence records that pin SHA-256s of source files later commits changed (`UNBOUND_R199_OWNER`, `UNBOUND_R186_OWNER`, and similar). |
| `FireSequenceTest` | 1 | `r136 full-step a-priori refusal remains an exact normal-suite gate`. |

Apart from the V6 item, these are certification bookkeeping, not physics.
Decide per test whether to re-baseline it or retire it; do not build new
machinery to keep historical bindings green.

## Operating rules

These rules come from the r248 stall, where two weeks went into proving
memory bounds by disassembling the compiled binary and no physics ran.

- **Every task names a tangible output**: a run, a spectrum, a movie, a
  measured number. Infrastructure is built only when a run cannot complete
  without it, and then in its smallest form.
- **Memory protection is the existing 32 GiB RSS ceiling with graceful
  checkpoint-and-exit.** Do not add launch-admission gates, a-priori memory
  proofs, allocation accounting or binary disassembly.
- **Do not chain physics milestones behind unrelated verdicts.** Soot work
  was blocked behind an unresolvable puffing verdict for weeks.
- **No unattended recurring automation** for this campaign. Work in bounded
  sessions with an owner review per deliverable.
- **One review round per real deliverable**, not per gate or per prerequisite.
- **Reuse output directories for retries.** Do not create a new `.vN`
  directory for each attempt; the r248 run produced 102 versions of one
  build directory.
- **Evidence policy is unchanged.** Code, docs, small JSON/CSV and SHA
  manifests go in git. Checkpoints, frames, binaries, archives and large logs
  stay outside it. `scripts/git-pre-push-size-guard.sh` enforces this.
- If a kernel's arithmetic changes, rerun from zero; do not build migration
  machinery.

## Results kept in `/Volumes/SSD/RISE/fire_production_calibration/`

| Entries | Content |
|---|---|
| `r237_tier10_full_window.v1`, `r237_tier10_preview.v1`/`v2`, `r237_tier10_spectrum.v1`/`v2`, `r237_inventory_*`, `r237_final_physics.v1`, `r237_retained_statistics.v2` | tier-10 full window and its analysis |
| `r241_tier8_full_window.v1`, `r241_tier8_preview.v1`, `r241_tier8_spectrum.v1` | tier-8 full window after the burner fix |
| `r242_tier8_hq.v1` | spectral HQ render and movie of r241 |
| `r255_comparison` | authorized one-at-a-time FDS, ~4 s limiter continuation and full tier-6 comparison; in progress, no solver arithmetic changes |
| `r254_turbulence_diagnosis` | offline retained-frame/checkpoint moments, independent Vreman comparison and primary source PDFs; no simulation |
| `r253_plume_diagnosis` | read-only r251 thermodynamic/velocity integrals, source/input manifest, cleanup inventory; no simulation |
| `r252_r251_centerline`, `r252_r251_preview`, `r252_soot_sources` | r251 native centerline extraction (tier-10 condition fails), r242-scene preview render, primary soot papers and extraction images |
| `r251_tier8_full_window` | completed from-zero tier-8 run; 99.506% burned, 0.483% top export, 32.837 kW, FDS height0.683m; full spectrum/data retained |
| `r250_tier8_parcel_edge` | incomplete tier-8 parcel-edge run; solver stopped at 21.466 s, accepted-prefix fuel/export/heat-height measurements retained |
| `r249_tier8_fuel_balance` | read-only r241 retained fuel inventory, sampled boundary localization, and native chemistry-state checks |
| `r243_nonphysical_diagnostic.v3` | temperature-mapped structure clips, labelled non-physical |
| `r244_tier6_*`, `r245_*`, `r246_tier6_top_double.v1`, `r246_recovered_top_*`, `r247_transport.v3`, `r247_source_spatial.v2` | tier-6 domain, budget and top-boundary studies: summaries, spectra and previews only; raw frames and snapshots deleted 2026-10-17 |

Each run directory has a matching `.log` where one was written.
