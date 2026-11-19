# r252 — measured in-flame methane soot evidence

2026-10-19. Research only; no simulation, build, or solver change. See the
[design revision](FIRE_SMOKE_SOOT_FORMATION_OXIDATION_R252.md). Source hashes,
measurement types, extraction coordinates and limitations are in
[data/fire_r252/soot_sources.json](data/fire_r252/soot_sources.json).
Large original papers and extracted images stay in
`/Volumes/SSD/RISE/fire_production_calibration/r252_soot_sources/`.

## Closest scale: NIST, 0.37 m and 34.5 kW

Falkenstein-Smith et al., [NIST TN 2082e2, *The Structure of Medium-Scale
Pool Fires*](https://doi.org/10.6028/NIST.TN.2082e2), Table 1, §2.7,
Appendix G and Fig. H.6, reports a quiescent, water-cooled, porous methane
burner, effective diameter **0.37 m**, ideal HRR **34.5 ± 0.5 kW**, and
mean visual flame height **0.640 ± 0.310 m**. This is close in heat release
to RISE's 33 kW case, with a 23% larger burner diameter. The reported HRR
is fuel flow times heat of combustion, not an oxygen-calorimeter measurement.

The primary measured soot quantity is **local gravimetric soot mass fraction**,
not optical soot volume fraction and not exhaust smoke yield. The sampling
line uses a heated PTFE filter; deposits in the quench probe are collected
and weighed too. Sampling lasts 12–25 min. Appendix G propagates filter mass,
flow and detected-gas mass uncertainties. The following values are transcribed
without digitization from the authors' [MaCFP experimental CSV](https://github.com/MaCFP/macfp-db/blob/213206ec45b91b2bd7d5f344ebd519662404892b/Liquid_Pool_Fires/NIST_Pool_Fires/Experimental_Data/Methane_37_cm_species_r%3D0_Falkenstein-Smith_2021a.csv).
The path is **Experimental_Data**; no simulated FDS or other computational
column is being used. The full original CSV is retained alongside a reduced
[axial table](data/fire_r252/soot_nist37_axial.csv).

| Height, m | Measured soot mass fraction, kg/kg | Published uncertainty column, kg/kg |
|---|---:|---:|
| 0.01 | 0.0001 | 0 |
| 0.02 | 0.0002 | 0 |
| 0.04 | 0.0003 | 0 |
| 0.06 | 0.0005 | 0.0001 |
| 0.10 | 0.0006 | 0.0002 |
| **0.14** | **0.0008** | **0** |
| 0.20 | 0.0005 | 0.0001 |
| 0.30 | 0.0002 | 0 |
| 0.45 | 0.0001 | 0 |
| 0.60 | 0 | 0 |
| 0.75 | 0 | 0 |

Zeros in the published uncertainty column are rounded values, **not exact
measurements**. The CSV prints soot fractions at 0.0001 resolution; a rounding
allowance of ±0.00005 kg/kg is recorded separately and is not a replacement
for the experimental uncertainty. The two upper zeros supply neither a
published detection limit nor an exact zero escaping-yield constraint.
The profile peaks at **0.0008 kg/kg at 0.14 m**, then falls by 0.60 m.

For orientation only, the reduced table includes an ideal-gas mean-state
conversion to volume fraction. It combines the measured mean species with a
separate [Sung 2024 experimental temperature release](https://github.com/MaCFP/macfp-db/blob/213206ec45b91b2bd7d5f344ebd519662404892b/Liquid_Pool_Fires/NIST_Pool_Fires/Experimental_Data/Methane_37_cm_TC_r%3D0_cm_Sung_2024.csv),
101325 Pa and soot density 1800 kg/m³. The resulting peak proxy is **0.106 ppm**
at 0.14 m. This is **not a measured mean volume fraction**: in general
`mean(rho * Y_soot) != rho(mean state) * mean(Y_soot)`, and these releases are
not simultaneous samples. Density 1800 is an explicit calculation assumption,
not a new optical preset. Calibrate against the measured mass fractions;
do not use this proxy as an independent second dataset.

## Direct volume-fraction measurements: smaller buoyant flame

Xin and Gore, [*Two-dimensional soot distributions in buoyant turbulent
fires*, Proc. Combust. Inst. 30 (2005), 719–726](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=100919),
DOI 10.1016/j.proci.2004.08.126, measured a **0.071 m** buoyant methane
flame at **84.3 mg/s** (about **4.2 kW**, derived using 50 MJ/kg). Fuel was
commercial grade, **>93% methane**; this is an important chemical-composition
limitation for extrapolation to pure methane. Planar LII was calibrated to
extinction measurements. Figure 4 supplies ensemble means from 3500 samples
per station; the authors estimate **30% overall uncertainty**.

The [figure-read axial profile](data/fire_r252/soot_xin2005_axial_figure4.csv)
is obtained by inverting each strip's published RGB color bar. It is approximate,
rounded to 0.01 ppm, with a separate ±0.02 ppm reading allowance:

| H/D | Height, m | Centerline mean f_v, ppm | Radial maximum of mean f_v at that H, ppm |
|---|---:|---:|---:|
| 0.5 | 0.0355 | 0.17 | 0.21 |
| 1 | 0.0710 | 0.36 | 0.38 |
| 2 | 0.1420 | 0.37 | 0.38 |
| 3 | 0.2130 | 0.14 | 0.16 |
| 4 | 0.2840 | 0.06 | 0.06 |

These exact-height rows do not capture every peak between station heights.
Figure 4's mean maps reach approximately **0.5 ppm** locally; Figure 2 shows
instantaneous soot streaks on a **0–5 ppm** scale. An instantaneous maximum
must not become the target for an ensemble mean. The paper's observed
increase then depletion supports formation plus burnout; its smaller scale,
fuel purity and uncertainty preclude transplanting one peak as the 33 kW value.

## What is established, and what is still missing

Both original publications were downloaded and inspected. The closest-scale
raw experimental profile is complete, with source hashes; the smaller flame
supplies direct volume fractions and spatial intermittency. Together they
support nonzero in-flame soot despite very low escaping smoke. They do not
provide a uniquely identified universal gross formation yield, a complete
subgrid joint temperature–soot PDF, or a direct pure-methane volume-fraction
profile at exactly 0.30 m/33 kW.

No formation coefficient has been fitted in this research-only task. A
published 0.1–1 ppm range is not a replacement for these source-specific
means, peaks, units, geometry and uncertainty. The design below makes this
measurement gap explicit and names a finite calibration task for review.

## Bounded follow-up search: direct volume data at 10–40 kW

A further primary-source search followed Xin's pool-fire references and the
NIST medium-scale measurements. No directly measured methane in-flame f_v
axial profile at 0.15–0.37 m burner diameter /10–40 kW was located in this
bounded search. This is a search result, not proof that none exists.

The [authors' Purdue publication list](https://engineering.purdue.edu/GRG/publications)
identifies Xin references [3–5] as toluene pool-fire work (1992–1993).
The publisher's [Sivathanu–Faeth 1990 underfire abstract](https://doi.org/10.1016/0010-2180(90)90061-U)
lists acetylene, propylene, ethylene and propane, not methane; its full paper
was not acquired. Shaddix–Smyth 1996 offers direct methane LII in steady and
forced flickering coflow flames, but its publisher abstract supplies no new
near-scale pool-fire profile; no numbers were silently substituted from it.
The NIST historical flame-data page links the legacy numeric files through
Wayback, whose axisymmetric-data link did not return accessible content in
this session. Larger natural-gas radiation studies and heptane/toluene/JP-8
pool data are not methane 33 kW in-flame f_v measurements.

There is useful **independent net-smoke evidence** at the right scale:
[NIST TN 2162r1 (2024), §3.3 and Table 4](https://doi.org/10.6028/NIST.TN.2162r1)
reports methane 34.5 kW escaping soot below detection in eight repeats.
The table footnote prints `<1E-4`; because the yield-column heading is
`10^-3 g/g` and the footnote omits a unit, this transcription is retained
without silently turning it into a precise adopted kg/kg threshold.
The original table image and paper were inspected and retained.
The [NIST calorimetry record methane_gas_burner22](https://www.nist.gov/el/fcd/structure-medium-scale-pool-fires/methanegasburner22)
also reports soot yield below detection; its adjacent **0.00044 kg/kg is in
the uncertainty column**, not a detection-limit field. This supports an
independent low escaping-yield test while leaving formation calibrated to
local in-flame data. It does not close the direct-volume-profile gap.
