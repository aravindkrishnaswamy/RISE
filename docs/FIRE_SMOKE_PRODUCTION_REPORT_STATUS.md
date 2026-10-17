# Production fire — report status, not capstone sign-off

As of r237, a complete tier-10 window and temporal preview exist. The
under-hour performance goal and the complete physical validation goal are
not met. This report separates delivered output, measured failures, and
missing qualification; engineering survival is not an empirical pass.

## Delivered window and preview

The from-zero tier-10 run completed 16,298 steps to 25.051545636115861 s,
including the full 14.625-s statistics window, with 235 retained frames.
Elapsed wall time was 38.77 hours. Peak/final velocity was 20.03506/6.75872
m/s; final sampled RSS was 1.995 GB with zero compression. No memory-flat
claim is inferred from this endpoint alone. Full trajectories are retained.

Summary SHA256: `9702162e20b8cf74b4aaa4e3524df3c7d2b4f4b55ca5bd7facbc4b36dc7b7e42`.
See [r237](FIRE_SMOKE_TIER10_WINDOW_R237.md) for the case, binary, launch
receipt, checkpoint and payload locations.

Delivered movie: `/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_preview.v2/temporal_fire_preview_16fps.v3.mov`.
235 decoded frames, exact 16-fps timestamps, 14.6875 s including the final
frame hold. Label: **64x64 SDR preview_primary**, not final-qualified media.
SHA256: `b6ad27ec7f2fc73bbdee1f6e5afeec01987b173d2123dddb549425c1af5cce18`.
The sequence-backed spectral rasterizer executes, but the actual records
remain preview-only/synthetic-chemistry; more samples cannot remove that
qualification. Higher presentation quality and predictive qualification
are different outstanding tasks.

## Empirical results

| Observable | Measured | Original comparison | Status |
|---|---:|---:|---|
| Centerline HRR peak | 3.07091346 Hz | 2.73861279 Hz, 20% frequency gate | Within frequency gate |
| Corrected display lit-area peak | 1.43309295 Hz | Expected-bin/peak power 0.662832 | Diagnostic; not a joint puffing pass |
| McCaffrey temperature-rise maximum relative error | 45.0474814% | <=10% | FAIL |
| McCaffrey velocity maximum relative error | 28.1368465% | <=10% | FAIL |
| Integrated HRR / consumption-LHV relative discrepancy | 7.82031e-11 | <=2e-10 | PASS |
| Integrated radiative fraction | 0.1986850834 | 0.18–0.22; context 0.07–0.28 | PASS |

Spectrum SHA256: `03381c849cf962ef239b051d1f30fb2aed156d7aaf1fcbee9da2cfbf1f2b7fce`.
Retained-integral extraction SHA256:
`dd96796e7f7bf6e776b234432e70d1dc40878929f60ad04fef06ff4adea0b1b3`.
The earlier 0.1365-Hz dominant lit-area result was contaminated by PNG
transfer metadata and is superseded, not evidence of domain recirculation.
The exact event-split assertion fails separately: the first contributing
step begins 233.053 microseconds early, although integration clips its
contribution to the prescribed statistics boundary.

Mean HRR is 19.0323 kW against the unchanged 33-kW reference. Passing the
consumption ledger does not establish complete fuel utilization. Matched
retained inventories leave a 4.106-g unaccounted residual; neither that
residual nor an instantaneous donor outflow estimate is measured integrated
boundary export. The causal audit remains open; do not renormalize the
McCaffrey reference or assign the deficit to entrainment without evidence.
Inventory extraction SHAs and the reviewed retained-event staging probe
are recorded in r237.

## Formal filtered-field contract: NOT QUALIFIED

The [r214 prerequisites](FIRE_SMOKE_FILTERED_ONSET_R214.md) still apply:
filter before differencing with the pinned cubic B-spline at physical width
0.04894898570785762 m. The required allowance is
`Eh,P + EΔt,P + Eh,O + EΔt,O + B32`. The ported owner's shared-input
calibration and independent additive terms are not measured. Historical
remap bounds and separate-trajectory distances cannot populate this table.

| Conservative channel | Filtered distance / allowance | Inventory-density distance / allowance | Verdict |
|---|---|---|---|
| 0: rho*Z | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 1: CH4 | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 2: O2 | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 3: N2 | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 4: CO2 | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 5: H2O | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 6: CO | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 7: carbon | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| 8: sensible energy | unavailable / unavailable | unavailable / unavailable | NOT QUALIFIED |
| Velocity | unavailable / unavailable | not an inventory channel | NOT QUALIFIED |

Unavailable is not zero and NOT QUALIFIED is neither pass nor numerical
failure. The -184.0827 versus -23.3962 kg/(m² s²) face-rate comparison
remains a residual-concentration diagnostic outside the filtered contract.
Prerequisite document SHA256:
`26075d6cc4bc5303ebddd25398d9e45ce8b81aac2ab359923ea8c5195cf3e98e`.

## Remaining closure

1. Diagnose the physical-row deficit using actual accepted transport and
   applied-injection accounting; retain the original physical gates.
2. Measure the missing owner-specific formal terms; do not manufacture a
   completed contract from the survival result.
3. Resolve final-media record qualification and presentation quality.
4. Complete the historical-arc evidence digest, final independent reviewer
   round, and backed-up branch hygiene. This status document does not claim
   those tasks are finished or replace the requested final report.
