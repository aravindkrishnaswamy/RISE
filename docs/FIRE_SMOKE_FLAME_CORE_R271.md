# r271 — why the RISE flame core is too hot

2026-10-31. Read-only analysis by Claude of r264 and r270 (RISE tier 8, floor on in r270) against FDS r263 (no-slip). Existing data only; no run, no code change. Window 10.466–25.091 s.

**Files.** Scripts are in [`tools/fire/r271/`](../tools/fire/r271/) and run beside the r270 scripts. Outputs are in [`data/fire_r271/`](data/fire_r271/).

## 1. Combustion happens at the same heights

Cumulative heat release from the floor, as a share of the total (FDS r263 HRRPUV slices against RISE `budgets/heat_release_height.csv`). Totals: 32.99, 32.85 and 32.84 kW.

| Share released | FDS r263 | r264 | r270 |
|---|---:|---:|---:|
| 50% by z = | 0.083 m | 0.069 m | 0.071 m |
| 75% by z = | 0.251 m | 0.220 m | 0.229 m |
| 90% by z = | 0.423 m | 0.412 m | 0.426 m |
| Above 0.765 m | 0.8% | 1.4% | 1.6% |

- **RISE burns at least as low as FDS.** Its fuel is not burning late.
- **The longer RISE "flame height" (H99) is set only by a tail of about 1% of the heat** above 0.75 m. H99 is very sensitive to that tail.
- **The radiative fraction is the same:** 0.199 for r264 against FDS's 0.20. A hot core is not under-radiation.

## 2. The same heat is carried by too little air

Upward plume properties from time means. Mass flux is the plane integral of ρw where it is positive. The heat column is the mean-flow flux cp·ρw·θ only; turbulent flux is excluded. RISE density is taken as 352.6/T, as in r269.

| z (m) | θ_c (K) FDS / r264 / r270 | b_w (m) FDS / r264 / r270 | Up mass (kg/s) FDS / r264 / r270 | Mean-flow heat (kW) FDS / r264 / r270 |
|---:|---|---|---|---|
| 0.29 | 926 / 1079 / 1096 | 0.123 / 0.101 / 0.110 | 0.143 / 0.076 / 0.106 | 19.3 / 20.3 / 20.7 |
| 0.50 | 631 / 775 / 807 | 0.134 / 0.114 / 0.118 | 0.184 / 0.123 / 0.140 | 22.6 / 23.3 / 23.9 |
| 0.99 | 228 / 315 / 350 | 0.177 / 0.156 / 0.152 | 0.314 / 0.240 / 0.243 | 22.1 / 23.6 / 24.5 |
| 2.00 | 72 / 105 / 112 | 0.280 / 0.237 / 0.230 | 0.686 / 0.523 / 0.509 | 21.6 / 22.9 / 23.7 |
| 3.01 | 39 / 58 / 59 | 0.384 / 0.311 / 0.305 | 1.176 / 0.865 / 0.847 | 22.0 / 22.0 / 23.2 |

- **RISE carries the same heat upward (within 5–10%) in 23–47% less air at every height,** from 0.29 m up. It does so in a plume about 15% narrower. The same heat in less air means a hotter core: θ_c is 17–50% above FDS.
- **RISE's mean-flow heat flux is slightly higher.** That fits weaker turbulent transport: FDS moves more of its heat by fluctuations.
- **r270's floor heating added air below 0.5 m (0.076 → 0.106 kg/s at 0.29 m)** but made the core slightly hotter, since the same flame now also burns in pre-warmed air. Above 1 m it changes nothing.

## 3. What it means

- **The hot, long core is not a combustion or radiation problem. It is under-entrainment that starts at the flame base,** and it is the same deficit as r266/r267's missing large, slow eddies. The low-frequency w power is 0.56–0.83 of FDS's at 0.5–1 m, and w_rms/w_c is 0.23–0.25 against 0.31–0.33.
- **The McCaffrey failures follow from it.** With entrainment fixed, the centreline excess should drop with it, since the heat carried is already right.
- **The remaining candidates are r267's list, now narrowed to mixing.** Floor radiation, combustion and radiation are ruled out:
  1. how RISE's SGS model operates in practice (same constants as FDS, possibly different eddy viscosity);
  2. damping of the large eddies by the scalar (MC + FCT) density transport at the plume edge;
  3. the side open-boundary pressure condition, which sets how freely ambient air is drawn in.
- **The puffing peak looked off,** at 3.62 Hz in the capstone summary. §4 resolves it.

## 4. Puffing: the frequency is right, the strength is low (owner item b)

The capstone's 3.62 Hz and r266's 2.75 Hz came from different signals. The same signals were measured in both codes, from 16 Hz frames and slices (235 samples each), against an expected 1.5/√D = 2.74 Hz.

| Signal | Method | FDS r263 | r264 | r270 |
|---|---|---:|---:|---:|
| Total heat release | Capstone method (single 512-point Hann periodogram) | 2.53 | 2.93 | 2.39 |
| Total heat release | Welch, 8 s segments | 2.62 | 2.88 | 2.88 |
| 3×3-cell centre column | Welch, 8 s segments | 2.62 | 2.88 | 2.88 |
| Single centre-cell column (the capstone's signal) | Capstone method | 2.46 | 0.07 | 3.69 |
| Single centre-cell column | Welch, 8 s segments | 2.50 | 3.50 | 4.63 |

- **The puffing frequency is right in RISE,** at 2.6–2.9 Hz on every broad signal. r264's per-step total heat release, rebuilt from its accepted-stage budgets (8,902 steps), gives 2.87–2.93 Hz by the capstone method at any resampling.
- **The 3.62 Hz comes from the capstone's signal choice.** It watches a single centre-cell column. In FDS that column still puffs at 2.5 Hz, but in RISE it is dominated by broadband cell-scale fluctuation, so its peak wanders from 0.07 to 4.6 Hz.
- **Puffing is weak and small-scale fluctuation strong.** RMS as a share of the mean:

| Signal | Band | FDS | r264 | r270 |
|---|---|---:|---:|---:|
| Total | 2–3.5 Hz | 6.3% | 4.1% | 3.6% |
| Total | 3.5–8 Hz | 2.0% | 2.6% | 2.7% |
| 3×3 column | 2–3.5 Hz | 13.4% | 9.7% | 8.0% |
| 3×3 column | 3.5–8 Hz | 5.1% | 7.2% | 7.7% |

  RISE puffs at 57–72% of FDS's strength, while its 3.5–8 Hz fluctuation is 1.3–1.5× FDS's.

## 5. SGS model: same model, different discretisation (owner item a)

- **Model and constants match.** Both codes use Vreman with C_v = 0.07 and Pr_t = Sc_t = 0.7 (`r263_fds.fds`; FDS 6.11 `velo.f90:200-247`; RISE `FireProductionForce.cpp:626-680`).
- **The discretisation differs in two places.** Both reduce how hard RISE damps the smallest resolved scales.

| | FDS 6.11 | RISE |
|---|---|---|
| ∂u_i/∂x_i for ν_sgs | Compact face difference, (u_{i+½} − u_{i−½})/Δ | Central difference of cell-averaged velocity across 2Δ (`FireProductionForce.cpp:1150-1195`) |
| Off-diagonal gradients for ν_sgs | Central difference of averages, 2Δ | Same |
| Viscous stress divergence | Stresses on cell centres and edges from compact face differences | Full stress at cell centres from the 2Δ gradients, then differenced to faces (`:1218-1290`) |

  - For a Fourier mode with wavenumber k, the RISE stress operator damps by a factor of about cos²(kΔ/2) relative to a compact one. That is half at a 4Δ wave and zero at a 2Δ (checkerboard) wave.
  - Its ν_sgs is also blind to 2Δ content, since a 2Δ checkerboard has zero central-difference gradient. RISE therefore puts neither eddy viscosity nor stress on grid-scale velocity noise.
- **RISE's velocity carries 1.4–3× FDS's grid-scale share.** The table gives the share of w′ variance in horizontal planes at kΔ ≥ 0.75π, over 235 frames, with FDS's cell-centred slices against RISE's cell-centred frames:

| z (m) | FDS | r251 (MC momentum) | r264 | r270 |
|---:|---:|---:|---:|---:|
| 0.29 | 0.195 | 0.402 | 0.267 | 0.276 |
| 0.50 | 0.158 | 0.402 | 0.216 | 0.218 |
| 0.99 | 0.129 | 0.414 | 0.213 | 0.220 |
| 2.00 | 0.075 | 0.356 | 0.226 | 0.237 |

  - The large-scale share, at kΔ < 0.25π, is lower in RISE by the same pattern: at 0.99 m it is FDS 0.453, r251 0.214, r264 0.403, r270 0.371.
  - Temperature planes differ much less (grid-scale share 1.1–1.6×).
  - **Centered momentum (r264) cut the grid-scale share from about 0.40 to about 0.22,** and it was also the change that partly restored entrainment. So the noise does not come from too little advective dissipation, and its level tracks the large-eddy deficit across runs.
- **What this does and doesn't show.** The FDS discretisation damps 2Δ–4Δ velocity content and RISE's does not, and RISE carries much more of that content. Whether the noise is the cause of the weak puffing and low entrainment is not proven. It could also be a symptom of a grid-scale source, such as the per-cell expansion, which is noisier in RISE (§4).

## Next (owner decisions)

1. **A/B test, a production solver change, so for owner review first:** compute the Vreman gradients and the viscous stress divergence on FDS's compact MAC stencil. Then run about 4 s from the r270 state, and compare the grid-scale share, puffing strength, entrainment and θ_c with the current operator.
2. **Item (c): side open-boundary pressure and inflow,** read-only. It can run before or alongside the A/B.
