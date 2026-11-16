# r272: methane soot formation switched on, one-parameter fit, full window launched

Last updated 2026-11-15. Design and evidence: [formation–oxidation draft](FIRE_SMOKE_SOOT_FORMATION_OXIDATION_R252.md),
[measurements](FIRE_SMOKE_METHANE_SOOT_DATA_R252.md). Data: `docs/data/fire_r272/`. Tools: `tools/fire/r272/`.

## Result

- Gross soot yield **0.03 kg per kg of fuel burned** is the least-squares fit to the NIST 0.37 m centreline profile.
- **The fit is poor, and that is a model limit, not a tuning gap.** The simulated centreline soot mass fraction is flat
  (about 3e-4 from the burner to 0.3 m, 1.6e-4 at 0.75 m); the measurement peaks at 8e-4 at 0.14 m and is zero-rounded
  by 0.60 m.
- **Burnout is about half.** Of the soot formed, about 45% leaves the flame unburned (net smoke yield about 0.0135);
  NIST reports escaping methane soot below detection.
- The flow is unchanged by soot: centreline temperature with and without soot agrees within 2% at every height.
- The full 25 s tier-8 window with the accepted r271 candidate plus this yield finished 2026-11-16 and confirms these numbers; the preview movie is rendered (§5). It does not yet look like a sooty fire.

## 1. Closure (uncommitted opt-in)

`RISE_FIRE_METHANE_SOOT_YIELD=<y>` (0 to 0.1), read in `FireProductionSourceKernel.h`. Unset, the solver is
bit-identical to before and the methane record's yield of 0 stands.

- Formation is the exact inverse of the record's carbon oxidation: per kg of fuel reacted, `y` kg of carbon is held
  back, with the oxygen it would have used, the CO₂ it would have made and its oxidation heat. Mass, elements and
  energy balance with the existing record constants; the frozen-packet ledger check is updated to match.
- Oxidation is the existing model, untouched: above 1300 K cell temperature, mixing-limited, sharing oxygen with the fuel.
- Radiative fraction stays 0.20 of gas plus soot-oxidation heat.
- Not changed: the oxygen allocator and the flame-temperature trial still use the full fuel stoichiometry and heating
  value (about 2% inconsistent at y = 0.03, on the conservative side).
- The methane record, its generator and its digest are not edited. Replacing the record's enforced zero is the
  draft's eventual route and needs owner review.

Tests with the variable unset: all five fire test programs pass. With it set, `FireSimulationSolverTest` fails five
checks. Four pin the zero-yield ledger exactly (three V4 checks, and r75 which requires gas heat = reacted fuel × LHV).
The fifth, "V2 reacting 3-D owner derives S_div…", is a manufactured-solution check; its cause was not traced.

## 2. Calibration run

`soot_y010`: restart of r270 tier 8 at 14.984 s, 2 s, candidate settings, y = 0.01. Every step accepted.
Profiles are the mean of the last 16 frames (15.98–16.98 s).

| Height, m | NIST Y_soot, kg/kg | Model at y = 0.03, kg/kg |
|---|---:|---:|
| 0.01 | 0.0001 | 0.00028 |
| 0.06 | 0.0005 | 0.00031 |
| 0.14 | 0.0008 | 0.00031 |
| 0.20 | 0.0005 | 0.00029 |
| 0.30 | 0.0002 | 0.00028 |
| 0.45 | 0.0001 | 0.00024 |
| 0.60 | 0 | 0.00019 |
| 0.75 | 0 | 0.00016 |

- Model values are the y = 0.01 run scaled by 3. Soot is close to a passive tracer here, so the scaling is linear;
  the full window is the direct check.
- Fit functional as the draft prescribes: residuals weighted by the larger of the published uncertainty and
  0.00005 kg/kg. Weighted y = 0.0300, unweighted 0.0364; with heights scaled by burner diameter, 0.0283 and 0.0348.
- Weighted misfit is 14 per point (a no-soot model scores 35). RMS residual 2.2e-4 kg/kg.
- Soot volume fraction on the centreline at y = 0.03 is about 0.04 ppm, against the 0.106 ppm NIST proxy and
  Xin–Gore's 0.37 ppm in a smaller flame.

Soot budget at y = 0.01 (`soot_y010_budget_15.98_16.98s.txt`):

- Formation is 6.6 mg/s (0.66 g/s of fuel at 33 kW). Upward soot flux is 1.8 mg/s at 0.14 m and levels at 3.0 mg/s
  from 0.9 to 1.2 m. Above 1.3 m the 2 s run has not yet filled the plume.
- Share of the soot inventory in cells hotter than 1300 K: 35% at the burner, 19% at 0.14 m, 5% at 0.5 m, 0 from
  0.75 m. Soot leaves the oxidising cells early, which is why burnout stops.

## 3. Why the constant yield cannot fit

- Soot is formed where fuel burns, at the flame sheet, and in proportion to it. The measured peak sits on the axis at
  0.14 m, inside the fuel-rich core, where little fuel burns. A yield per kg burned has no way to put soot there.
- Oxidation needs the cell-mean temperature above 1300 K. At 3 cm cells the sheet is under-resolved and most soot
  sits in cells whose mean is cooler, so it stops burning.
- The draft's instruction for this case is to report the failure before adding a richness or temperature formation
  function, and not to add a cold-cell eraser. No such term was added.

## 4. Full window (finished 2026-11-16)

`/Volumes/SSD/RISE/fire_production_calibration/r272_soot/tier8_y030`, launcher `tools/fire/r272/run_tier8.sh`:
`FireSequenceTest --fire-production-temporal-capstone 8 0.0625`, compact stencil, limited carrier,
`RISE_FIRE_VREMAN_ZONE=below:16:0.2857`, floor emissivity 1, yield 0.03.

- Completed: 15,367 steps to 25.091 s in 28.3 h, no retries, 235 frames, sealed summary and manifest
  (`sequence_manifest_sha256=ad6b3eb8…`).
- Heat release 32.2 kW over the statistics window (10.47–25.09 s); radiative fraction 0.201; puffing 3.41 Hz.
- **The linear scaling held.** Mean of the last 10 s (160 frames): centreline Y_soot 3.06e-4 at 0.14 m (predicted
  3.1e-4), 1.8e-4 at 0.63 m, 1.5e-4 at 0.75 m; centreline volume fraction 0.039 ppm in the flame.
- **Escaping smoke.** Soot formed 19.7 mg/s (0.657 g/s of fuel); upward flux is level at 8.8 mg/s from 1.1 m to the
  top. 45% escapes; net smoke yield 0.0134 kg/kg.
- Data: `tier8_y030_centreline_last10s.txt`, `tier8_y030_budget_last10s.txt`.

## 5. Movie

`r272_soot/movie/r272_soot_128x192.mov` (235 frames, 16 fps, 14.69 s; decode check passed), rendered by
`tools/fire/r272/render_movie.py` with the r242 spectral preview scene pointed at the new manifest.

- Soot now gives a bright white-pink flame core at the base, about 0.5 m tall. It is overexposed at the scene's
  fixed exposure.
- The column above it is blue, as it was in the soot-free r251 preview. The blue is therefore not soot; its source
  in the renderer (gas emission channels or exposure) was not investigated.
- The escaping smoke does not read as dark smoke: the scene has no light for it to absorb or scatter.

## Open

- The production change is uncommitted (owner review); `docs/data/fire_r271/s29_optins.patch` carries it.
- Calibration used the 0.30 m / 33 kW scene, not a reproduction of the NIST 0.37 m / 34.5 kW burner, and a 1 s mean.
- A second resolution was not run, so whether the fitted yield moves with cell size is unknown.
- Next modelling step, if the look or the smoke level is not acceptable: a sub-grid oxidation temperature (flame-sheet
  rather than cell-mean) or a formation term tied to rich mixture. Either needs its own evidence and review.
