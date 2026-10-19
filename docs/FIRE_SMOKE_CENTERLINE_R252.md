# r252: r251 centerline profiles and tier-10 decision

2026-10-19. **Do not launch tier 10.** Both maximum errors exceed the owner's
15% limit. The accepted r251 combustion result remains the supplied premise;
this measurement concerns the plume above the flame. No solver changes or new
simulation runs were made for this extraction.

| Retained full-window row | r237 tier 10 | r251 tier 8 | Owner limit |
|---|---:|---:|---:|
| Maximum absolute temperature-rise relative error (signed at worst station) | −45.0475% | +87.2908% | ±15% |
| Maximum absolute vertical-velocity relative error (signed at worst station) | −28.1368% | +25.0088% | ±15% |

r251's worst temperature station is z=3.044015 m: ΔT=70.22155 K against
37.49332 K. Its worst velocity station is z=2.921643 m: w=3.169432 m/s
against 2.535366 m/s. r237's two maxima both occur at z=3.096023 m.
These rows are maxima of station errors, not errors of the maximum observed
flame temperature or velocity.

## Reproducing the r237 station rule

Use `FireSequenceTest --fire-production-checkpoint-physics-diagnostic` on the
existing r251 final checkpoint. Its diagnostic writes persisted station
integrals; it does not advance the solver. Divide the temperature and vertical
velocity integrals by the persisted centerline statistics duration. As in
[r237](FIRE_SMOKE_TIER10_WINDOW_R237.md), do not read the unfinalized checkpoint's
zero-valued derived McCaffrey summary fields.

`tools/fire/r252_centerline_report.py` reproduces
`EvaluateMcCaffreyPlumeStations` and its existing finalization rule:

- Q=33 kW (authored input, not rescaled realized HRR), ambient T=300 K.
- ΔT=23.3(z/Q^0.4)^(-5/3) K; w=1.13 Q^0.2(z/Q^0.4)^(-1/3) m/s.
- Retain z > max(flame tip, 0.2 Q^0.4), and z <= domain top − D*/2.
- Flame tip is the highest plane with mean heat release at least 1% of the
  peak mean plane heat release, exactly the existing comparison convention.
  It is not the FDS/Heskestad flame-height statistic.
- D*=0.24474492853928809 m. No arbitrary cutoff at 1.5 m is substituted for
  the original plume-station rule.

r251 duration is 14.625000000055213 s, 106 stations, 76 selected. Its 1%-plane
flame tip is 0.627159 m; selected bounds are (0.809908, 3.120498] m. r237
has duration 14.625000000004771 s, 132 stations, 94 selected, flame tip0.330406 m
and bounds (0.809908, 3.108261] m. Thus the selection rule is the same, with
native cell-center locations at each resolution. The persisted centerline
sampler uses one lateral cell for the odd tier-8 grid and the four central
lateral cells for the even tier-10 grid, as in the existing C++ sampler; no
post hoc regridding is applied. Reconstructed r237 errors
match the published r237 report to its printed precision.

## Profile against McCaffrey

The table samples existing selected stations, without interpolation. Complete
profiles (including the selection flag and every retained height) are in
[data/fire_r252/r251_centerline.csv](data/fire_r252/r251_centerline.csv) and
[data/fire_r252/r237_centerline.csv](data/fire_r252/r237_centerline.csv).

| z (m) | ΔT (K) | McCaffrey ΔT (K) | Error | w (m/s) | McCaffrey w (m/s) | Error |
|---:|---:|---:|---:|---:|---:|---:|
| 0.811 | 426.40 | 340.08 | +25.4% | 3.884 | 3.887 | -0.1% |
| 0.994 | 315.96 | 242.02 | +30.6% | 3.670 | 3.631 | +1.1% |
| 1.239 | 235.53 | 167.71 | +40.4% | 3.519 | 3.375 | +4.3% |
| 1.514 | 177.89 | 120.04 | +48.2% | 3.425 | 3.156 | +8.5% |
| 1.759 | 146.27 | 93.51 | +56.4% | 3.342 | 3.003 | +11.3% |
| 2.004 | 123.91 | 75.26 | +64.6% | 3.250 | 2.875 | +13.1% |
| 2.249 | 106.69 | 62.11 | +71.8% | 3.230 | 2.767 | +16.8% |
| 2.493 | 93.12 | 52.29 | +78.1% | 3.219 | 2.673 | +20.4% |
| 2.738 | 81.70 | 44.73 | +82.6% | 3.199 | 2.591 | +23.5% |
| 2.922 | 74.71 | 40.15 | +86.1% | 3.169 | 2.535 | +25.0% |
| 3.044 | 70.22 | 37.49 | +87.3% | 3.116 | 2.501 | +24.6% |
| 3.105 | 67.87 | 36.27 | +87.1% | 3.079 | 2.484 | +23.9% |

The high plume is too hot and too fast relative to the 33 kW correlation,
whereas the underburning r237 baseline was too cold and too slow. This is an
observed reversal, not evidence identifying its cause. Passing fuel balance,
flame height and puffing does not establish correct centerline entrainment.
No inference about the responsible transport or boundary term is made here.
The owner-requested conditional branch ends with these profiles; tier 10
remains unlaunched.

## Evidence

[data/fire_r252/centerline_results.json](data/fire_r252/centerline_results.json)
records input hashes, checkpoint hashes, duration, bounds and worst stations.
The small native integral extractions for both runs are also committed in
`docs/data/fire_r252/*_retained_statistics.txt`. The original r251 extraction is at
`/Volumes/SSD/RISE/fire_production_calibration/r252_r251_centerline/retained.txt`.
The reader binary was the existing r251 binary, SHA-256
`d1cebdd779a5d18a9e5b92ec162ddfcfc51aaae1a8963104bcdc633fb7bab033`.
The full checkpoint's SHA-256 is
`417472aa4449dcff81de248978780182b16d7d8701eb067b928cd0704c85c5e7`.
The report script depends only on Python's standard library. No build or new
simulation was needed.

An independent read-only reconstruction found no P1/P2 issues: durations,
selected stations, signed maxima, every CSV row and input/checkpoint hashes
agree. The soot evidence/design received a separate source-and-conservation
review; no implementation is included in this deliverable.
