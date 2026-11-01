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
- **The puffing peak is also off.** The r264 capstone summary gives 3.62 Hz against about 2.74 Hz expected, while r266 found 2.75 Hz for axisymmetric puffing at 0.29 m in both codes. The two measures disagree, and that is worth resolving first, because the puffing instability at the base drives the large eddies.

## Next (needs owner choice)

- **Read-only diagnostics, no runs:**
  - (a) Rebuild FDS's Deardorff eddy viscosity from RISE checkpoints and compare it with RISE's own, at the base and at 0.5–1 m.
  - (b) Reconcile the two puffing measures, using the same signal for both codes.
  - (c) Compare side-boundary pressure and inflow.
- **Then, if (a) or (c) points somewhere,** run a short A/B from the r270 state (about 4 s of simulated time, as in r260/r262). That needs approval.
