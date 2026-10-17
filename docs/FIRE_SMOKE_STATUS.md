# Fire & smoke — current status (start here)

Last updated 2026-10-17, during the r249 one-cell parcel-edge correction. This page
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
| 8 | 15,309 | 21.8 h | 5.1 s |
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

Tier 10 ran before the r239 burner-boundary fix; tier 8 ran after it.

## Open problems, in priority order

1. **Incomplete combustion.** Only 58–64% of the supplied fuel burns inside
   the statistics window. This is the leading explanation for the low
   McCaffrey centerline, though r237 did not formally assign the cause.
   r239 found that production injected fuel into bottom cells while treating
   the bottom boundary as open, so fuel could leave the domain; it restored
   the oracle's sealed burner floor with a prescribed inlet. The tier-10 run
   before that fix released 58% and the tier-8 run after it released 64%,
   but they differ in resolution, so the fix's effect is not isolated. The
   remaining unburned fuel is not attributed. It may leave through the top
   or sides, or the mixing-limited reaction closure may under-burn at this
   resolution. Measure where the missing fuel goes, fix the cause, then
   rerun tier 10 to check McCaffrey.
   **r249 measurement:** [tier-8 fuel balance](FIRE_SMOKE_TIER8_FUEL_BALANCE_R249.md)
   attributes the dominant loss to cold, dilute methane escaping the top.
   Over 99.47% of statistics time: inlet 9.596 g, burned 6.175 g, inventory
   change −0.0309 g, balance remainder 3.452 g; sampled top donor export
   3.451 g. This is sampled localization, not an exact historical accepted
   boundary integral. An established-flame mixing/combustion closure correction
   is proposed; its causal effectiveness remains untested.
   **Offline eligibility follow-up:** [four-gate comparison](FIRE_SMOKE_TIER8_ELIGIBILITY_R249.md)
   gives mean eligible CH4 (a/b/c/d) 0.0360/0.0771/0.5774/0.5774 g.
   Parcel CFT alone recovers 7.51% of stranded inventory; allowing cold
   vertices connected to warm seeded components recovers 98.98%.
   The [one-cell follow-up](FIRE_SMOKE_TIER8_EDGE_R249.md) selects (e): mean
   eligible mass 0.136754 g, retaining 57.72% of (c) below 1 m while putting
   99.91% of (e) below 1 m. [Revision (e)](FIRE_SMOKE_EXTINCTION_PARCEL_R249.md)
   is implemented. The tier-8 from-zero acceptance run is active in
   `r250_tier8_parcel_edge` (started 2026-10-17 12:50 UTC): ≥95% fuel
   consumption with closing balance, near-zero top CH4 export, and plausible
   heat-release height against Heskestad. Stop for review before tier 10.
2. **The fire renders blue.** The methane record's gross soot yield is 0,
   and `FireSimulationRecords.cpp` enforces that. With no soot there is no
   incandescence, only blue chemiluminescence. The zero is methane's
   escaping smoke yield; real methane flames at this scale form roughly
   0.1–1 ppm soot in the flame and burn it out. The fix is a soot
   formation–oxidation closure calibrated to published in-flame data.
   Radiation is budget-partitioned at χᵣ = 0.20, so this changes the
   appearance, not the energy balance.
3. **Puffing is close but high.** Centerline heat-release spectra peak at
   3.1–3.4 Hz against the 2.74 Hz law. The "0.137 Hz slow mode" seen in
   earlier image-based measurements is not supported by the centerline
   observable. Do not reopen the tier-6 domain study (r244–r247): tier 6 has
   only about 7 cells across the burner and its spectra are noise-dominated.
   Average several sub-windows of the tier-8 or tier-10 runs instead of
   using one raw periodogram.
4. **Cost.** Tier 10 takes about 39 hours. Faster steps would make the
   iteration loop practical, but cost work comes after items 1 and 2.

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
| `r250_tier8_parcel_edge` | active from-zero tier-8 run of the one-cell parcel-edge gate; fuel/export/heat-height acceptance pending |
| `r249_tier8_fuel_balance` | read-only r241 retained fuel inventory, sampled boundary localization, and native chemistry-state checks |
| `r243_nonphysical_diagnostic.v3` | temperature-mapped structure clips, labelled non-physical |
| `r244_tier6_*`, `r245_*`, `r246_tier6_top_double.v1`, `r246_recovered_top_*`, `r247_transport.v3`, `r247_source_spatial.v2` | tier-6 domain, budget and top-boundary studies: summaries, spectra and previews only; raw frames and snapshots deleted 2026-10-17 |

Each run directory has a matching `.log` where one was written.
