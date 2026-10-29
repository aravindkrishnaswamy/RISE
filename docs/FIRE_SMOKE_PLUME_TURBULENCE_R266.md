# r266 — where the entrainment deficit actually is

2026-10-29. Read-only analysis by Claude, which took the campaign over directly from Codex. No solver change and no new run. The r264 full tier-8 run (centered momentum, from zero) was still running at the time of writing.

Scripts are in [`tools/fire/r266/`](../tools/fire/r266/) and outputs in [`data/fire_r266/`](data/fire_r266/). All comparisons use the r251 statistics window (10.466–25.091 s) on the shared 69×69×106 grid, dx = 0.030593 m.

Sources:
- **RISE time means:** r254 `cell_moments.csv`.
- **RISE instantaneous planes:** read from the retained frame VDBs (`dump.cpp`).
- **FDS:** physical slices from the no-slip r263 run, the like-for-like reference.

## 1. There is no in-flame deficit

The 34% "inner-flame" shortfall at 0.29 m came from integrating over the whole r < 0.8 m disc. Split at twice FDS's thermal half-width, r < 2b:

| z (m) | Disc r<0.8, RISE/FDS | Plume core r<2b, RISE/FDS | Outside the core, RISE/FDS (kg/s) |
|---:|---:|---:|---:|
| 0.29 | −45% | −11% | 0.042 / 0.098 |
| 0.50 | −31% | −10% | 0.079 / 0.129 |
| 0.99 | −15% | −5% | 0.068 / 0.102 |
| 1.51 | −18% | −10% | 0.047 / 0.094 |
| 2.00 | −26% | −19% | 0.026 / 0.085 |
| 3.01 | −36% | −31% | 0.010 / 0.091 |

- **Below 1 m, the gap sits outside the plume.** FDS has a broad 5–15 cm/s updraft of ambient-temperature air at r = 0.2–0.6 m, and RISE has almost none. This is the potential flow induced by the stronger entrainment higher up, not flame physics.
- **The core matches below 1 m.** Mean w(r) agrees within about 1% for r < 0.16 m at 0.29 m.
- **Below 1 m, flame physics is not the problem:**
  - plane-integrated expansion per joule is similar in both codes, and slightly higher in RISE;
  - the cumulative heat-release profile Q(z) is the same within 1% (r262 data);
  - axisymmetric puffing at 0.29 m peaks at 2.75 Hz in both codes.
- **R265 centreline divergence is not an expansion deficit.** Its centreline-divergence difference is radial redistribution only; the plane integrals agree.
- **The real deficit grows above 1 m.**

## 2. The deficit is missing large-scale turbulence

Same grid and SGS constants, full window:

| z (m) | Code | w_rms/w_c | θ_rms/θ_c | b_T/z | Meander rms/b |
|---:|---|---:|---:|---:|---:|
| 0.99 | FDS / RISE r251 | 0.348 / 0.254 | 0.461 / 0.250 | 0.109 / 0.102 | 0.383 / 0.254 |
| 2.00 | FDS / RISE r251 | 0.314 / 0.221 | 0.413 / 0.242 | 0.106 / 0.076 | 0.329 / 0.174 |
| 3.01 | FDS / RISE r251 | 0.327 / 0.200 | 0.417 / 0.266 | 0.100 / 0.066 | 0.321 / 0.180 |
| — | Shabbir & George 1994 | 0.33 | 0.40 | 0.10 | — |

- **FDS on this grid matches the published plume data,** so resolution is not the limit.
- **RISE's missing power is at low frequency.** Its spectra are deficient at 0.25–3.5 Hz (1–2 Hz band: 15–27% of FDS power above 0.5 m), but match or exceed FDS above about 4 Hz (r261 spectra and `band_power.txt`). So the cause is damping of the large, slow eddies that drive meandering and spreading, not small-scale numerical dissipation.
- **The open boundaries behave alike.** Top inflow is about 0.09 kg/s (RISE) and 0.12 kg/s (FDS); the top outflow difference just follows the plume.

## 3. Centered momentum restores most of it

The r260/r262 four-second A/B runs start from the r251 state at 15.019 s. Statistics below use the last 3 s, which is 48 frames, so they are noisy:

| z (m) | Run | w_c | w_rms/w_c | θ_c (K) | θ_rms/θ_c | b_T/z | Meander/b |
|---:|---|---:|---:|---:|---:|---:|---:|
| 2.00 | MC baseline (r251) | 3.29 | 0.207 | 128 | 0.225 | 0.074 | 0.26 |
| 2.00 | Half dt | 3.09 | 0.228 | 121 | 0.249 | 0.078 | 0.27 |
| 2.00 | CHARM | 3.29 | 0.237 | 112 | 0.258 | 0.083 | 0.24 |
| 2.00 | **Centered momentum** | 2.54 | 0.349 | 85 | 0.509 | 0.101 | 0.38 |
| 2.00 | FDS (full window) | 2.72 | 0.314 | 73 | 0.413 | 0.106 | 0.33 |
| 3.01 | MC baseline (r251) | 3.26 | 0.177 | 72 | 0.229 | 0.065 | 0.30 |
| 3.01 | **Centered momentum** | 2.22 | 0.284 | 43 | 0.350 | 0.092 | 0.50 |
| 3.01 | FDS (full window) | 2.36 | 0.327 | 40 | 0.417 | 0.100 | 0.32 |

McCaffrey plume-region values for 33 kW:

| z (m) | θ (K) | w (m/s) |
|---:|---:|---:|
| 2 | about 74 | about 2.8 |
| 3 | about 38 | about 2.45 |

**Centered momentum brings the 2 m and 3 m centreline and spreading close to FDS and McCaffrey.** The r262 "37.5% of the 3 m gap" figure understated it, for two reasons:
- It averaged in the first-second transient from the old state.
- Its whole-disc metric includes the slow ambient drift, which takes longer to establish.

**What remains:** at 0.99 m the centreline is still about 310 K, against FDS's 249 and McCaffrey's about 235. The flame-tip and intermittent region did not improve within the A/B window.

## Next

1. **When r264 finishes,** run `tools/fire/r266/` on its full statistics window, using the same plume-core metrics, McCaffrey rows and Heskestad entrainment. This confirms or refutes section 3 with full statistics.
2. **If the far plume is confirmed,** look into the remaining 1 m excess using the same toolkit on r264.
3. **Retire the whole-disc r<0.8 m mass flux as the acceptance metric.** Report plume-core mass flux, centreline McCaffrey rows and spreading rate instead.
