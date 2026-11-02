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

## 6. Side and top open boundaries do not limit the inflow (owner item c)

Code comparison and data are in [`boundary_code.txt`](data/fire_r271/boundary_code.txt), [`boundary_budget.txt`](data/fire_r271/boundary_budget.txt) and [`boundary_profile.txt`](data/fire_r271/boundary_profile.txt). Scripts are `tools/fire/r271/boundary_*`.

- **Same boundary physics.**
  - Both codes hold total head at zero on inflow faces: RISE p̃_b = −½ρ∞|u|² and FDS H_b = 0. Both hold static pressure at zero on outflow faces.
  - Both remove ambient hydrostatics and bring in ambient scalars.
  - Domain and grid are identical: 69×69×106, dx 0.0306 m, centred.
- **Second-order differences.**
  - RISE zeroes tangential velocity on inflow faces where FDS uses free slip. `FIRE_SMOKE_DESIGN.md` §3.6 says zero normal gradient, so the code disagrees with the design.
  - RISE's boundary-face momentum is not advected or viscously forced before projection.
  - The inflow/outflow switch differs: RISE uses an active set, FDS the sign of the current velocity.
- **Measured fluxes.** Face-level budgets, r270, time-mean kg/s: side net 0.823 and top net out 0.825. The outermost-cell estimate is within 1% of these.
  - Top inflow is the same in both codes: 0.113 against FDS's 0.120.
  - Only the plume's upward export differs: 0.938 against 1.277.
  - The side inflow deficit (r270/FDS 0.64–0.81 by height band) is already fully present 0.3 m inside the boundary, with shell ratios 0.796 / 0.710 / 0.696 for z < 1, 1–2 and 2–3.24 m.
  - 79% of the deficit enters above 1 m.
- **Bound on any boundary effect.**
  - RISE's instantaneous side outflow is higher: 4–12% of boundary cells against 0–4%, about 0.02–0.05 kg/s. Even with no outflow at all, its gross inflow of 0.884 kg/s stays far below FDS's 1.172.
  - The inflow-head difference implied by RISE's larger fluctuations is about 0.001 Pa. The plume's own pressure deficit is 0.07–0.27 Pa.
- **Verdict.** The boundaries pass what the plume demands; the deficit is plume demand. Zero tangential inflow velocity plausibly explains RISE's weaker vertical flow along the walls and is worth fixing to match the design. It is not the entrainment cause.

## 7. Compact viscous stencil, prepared for A/B (not adopted)

**Change.** The change is uncommitted, in `src/Library/Utilities/FireProductionForceMac.mm` and `tests/FireSequenceTest.cpp`. Setting `RISE_FIRE_COMPACT_VISCOUS_STENCIL=1` selects three new Metal kernels when the force context is built: `build_stress_compact`, `build_stress_fixed_mu_compact` and `build_face_force_compact`. They follow FDS 6.11 `velo.f90`:
- **Vreman ν_sgs** uses compact face differences on the diagonal gradients and keeps the averaged central differences off the diagonal.
- **Normal stresses** are computed at cell centres.
- **Shear stresses** are computed on cell edges, with edge μ the mean of the adjacent cells.
- **Ghosts:** a wall gives −u (no slip), an open boundary zero gradient, a periodic boundary wraps.

**Unchanged.** The legacy kernels, the pipeline lines checked by `FireProductionSolverTest`, the CPU reference, the scalar SGS diffusivity (still the legacy gradient) and the viscous stability bound. With λ = 24·μ_max·(1/ρ)_max/Δ², the bound still covers the compact operator, whose spectral radius is about 16ν/Δ². The launch receipt records `viscous_stencil r271-compact-mac-v1` when the switch is set. The kernel-set hash is source-based and does not distinguish the two.

**Checks.**
- **Fourier check** ([`compact_stencil_check.cpp`](../tools/fire/r271/compact_stencil_check.cpp), output in [`compact_stencil_check.txt`](data/fire_r271/compact_stencil_check.txt)). On a periodic 8³ box the production Metal RHS gives:

| Mode | Legacy | Compact | Compact symbol | Continuum |
|---|---:|---:|---:|---:|
| Shear, 8Δ wave | 50.0 | 58.58 | 58.58 | 61.7 |
| Shear, 2Δ checkerboard | **0** | 400.0 | 400.0 | 987 |
| Normal, 8Δ wave | 66.7 | 78.1 | 78.1 | 82.2 |
| Normal, 2Δ checkerboard | **0** | 533.3 | 533.3 | 1316 |

  This confirms that the production operator applies no viscous damping at all to a 2Δ checkerboard.
- **Bounded check.** With uniform u_x, open sides, a no-slip floor and an open top, the compact stencil gives −2/Δ² in the floor layer and 0 elsewhere. Legacy gives −1/Δ² at the floor and a spurious +0.5/Δ² in the second layer.
- **Switch off.** 25 tier-8 production steps are byte-identical in every budget data row to the r264/r270 binary. Only the identity hashes change, because the kernel source is hashed (`FireProductionAdvectionMac.mm:9432`).
- **Switch on, floor on.** 25 production steps ran without error. The maximum velocity matches r270 to the fourth digit, as expected from a fluid starting at rest.
- **`FireProductionSolverTest`** shows the same 12 failures at HEAD (a baseline built from a detached HEAD worktree) and with the change. No new failures.

**Independent review.** No P1 findings. Fixed:
- On wall edges du_n/dx_t is now zero, as in FDS (`DUIDXJ_USE=0`). Before this, the burner rim added a stress of order μ·w_inlet/Δ on floor edges.
- The periodic transverse face wraps to 0.
- The legacy-only velocity-trace qualification now refuses with the switch on, rather than failing a memcmp.
- The receipt line now comes from the force context's actual selection (`FireProductionForceMetalCompactViscousStencil()`) and is absent when the switch is off, so legacy receipts are byte-unchanged.
- `--fire-production-manifest-rebuild` refuses a stencil that disagrees with the run's receipt.

Known limits of the A/B, recorded rather than changed:
- **It is "compact momentum operator only".** The scalar SGS diffusivity still uses the legacy gradient, so effective Pr_t and Sc_t differ from 0.7 where grid-scale content is high.
- **The wall is no-slip,** as RISE has always had it. FDS's LES default is a log-law wall model.
- **The normal stress uses the current discrete divergence,** not FDS's constraint divergence.
- **Stability.** The diffusive step limit dt ≤ Δ²/(8ν) gives dt·λ ≤ 2 against the compact spectral radius of 16ν/Δ², which is the RK2 boundary in the worst case. Production steps are source-limited at 0.00165 s, about 70× below that limit.
- **CPU and FP64 reference owners stay legacy.** They are qualification-only, so CPU-vs-Metal parity tests would fail if the variable leaked into a test run.

The patch for owner review is [`compact_stencil.patch`](data/fire_r271/compact_stencil.patch). It is uncommitted in `src/` and `tests/` until approved.

## 8. Compact-stencil A/B: grid noise fixed, flame worse (2026-11-01)

The owner approved the patch (committed as `5cdace134`) and the run.

**Set-up.** One arm, `RISE_FIRE_COMPACT_VISCOUS_STENCIL=1` with the floor on. It resumes r270's retained checkpoint at step 9223 (14.984 s) and runs for 4 s, to 18.986 s: 2,449 accepted steps (9223 → 11672), 4.7 h wall time, peak RSS 1.8 GB.
- **Harness:** `tools/fire/r271/ab_driver.cpp`, built by `build_ab.py` against the committed production objects. As in r260, only an external copy of the test harness is patched: the 64-step cap on retained-production probes is lifted, and the run stops at a physical-time window.
- **Baseline:** r270's own frames 73–136.
- **Restart check:** the same harness with the stencil off, run from the same checkpoint, reproduced r270's frame 73 byte for byte (SHA-256 `fc172809…`). Restart is exact, so any difference is the stencil.
- **Window-to-window variability:** r270 windows w0 (10.47–14.40 s) and w2 (19.03–22.97 s) show how much a 4 s window varies with no change at all.
- **Data:** `docs/data/fire_r271/ab_compare.txt` (script `tools/fire/r271/ab_compare.py`).

| | r270 (w0 / w2 / same window) | compact | FDS r263 |
|---|---|---|---|
| Grid-scale share of w, 0.29 m | 0.27 / 0.26 / 0.27 | **0.18** | 0.075–0.20 (§5) |
| Grid-scale share of w, 0.99 m | 0.20 / 0.23 / 0.21 | **0.078** | |
| Grid-scale share of w, 2.00 m | 0.23 / 0.27 / 0.24 | **0.055** | |
| Grid-scale share of T, 0.29 m | 0.11 / 0.12 / 0.12 | **0.22** | |
| Total HRR rms in the 2–3.5 Hz band, % of mean | 3.5 / 3.9 / 3.1 | **2.2** | |
| Height below which 90% of the heat is released (m) | 0.43 / 0.43 / 0.46 | **0.83** | 0.43 |
| Height below which 99% of the heat is released (m) | 0.86 / 0.86 / 0.86 | **1.25** | 0.77 |
| Centre excess temperature θ_c, 0.75 m (K) | 504 / 517 / 533 | **735** | 355 |
| Upward mass flux, 0.75 m (kg/s) | 0.176 / 0.208 / 0.196 | **0.168** | 0.244 |
| Plume width b_T, 0.75 m (m) | 0.107 / 0.105 / 0.103 | **0.083** | 0.119 |
| Upward mass flux, 3.01 m (kg/s) | 0.876 / 0.843 / 0.863 | **1.021** | 1.176 |

**Result.**
- **Velocity noise is fixed.** The grid-scale share of w falls 2–4×, into FDS's range at every height. This confirms §5's diagnosis of the velocity noise.
- **The near field gets worse, not better.** The plume is narrower and hotter, and it entrains less air below 1 m. The heat release moves upward: on the 3×3 centre column it rises from 8.8 to 12.0 kW. The 90% and 99% heights grow from 0.43 m to 0.83 m and from 0.86 m to 1.25 m.
- **Puffing is weaker,** not stronger.
- **Only the far field improves:** at 3 m the upward mass flux is 1.02 kg/s, against 0.86 for r270 and 1.18 for FDS.
- **Not a transient.** In 1 s blocks, the 90% height goes 0.64 → 0.83 → 0.89 → 0.89 m, so it settles at the longer flame within about 2 s.
- **Temperature grid noise doubles** at 0.29–0.50 m, while velocity noise falls.

**Reading.**
- The undamped grid-scale velocity in r270 was doing some of the near-field mixing. That noise is numerical, not physical. Removing it exposes how little resolved mixing RISE has near the burner.
- Temperature grid noise rises because the scalars are now under-mixed. Scalar transport is untouched by this patch: heat and species still mix through the old cell-centred SGS diffusivity and MC/FCT advection.
- So the velocity stencil is not, by itself, the cause of the under-entrainment, and on its own it moves the flame further from FDS.
- **Decision: not adopted.** The patch stays opt-in and off by default.

## 9. How RISE mixes heat and species (read-only, 2026-11-01)

**Formulation.** RISE's turbulent heat and species diffusivity comes from the scalar kernel `evaluate_resident_transport` (`FireProductionAdvectionMac.mm`):
- D = D_mol + ν_t/Sc_t and k = k_mol + ρ c_p ν_t/Pr_t, with Pr_t = Sc_t = 0.7 and Vreman C_v = 0.07. FDS r263 uses the same values (`PR=0.7, SC=0.7, C_VREMAN=0.07`).
- There are two differences in discretisation:
  1. **ν_t is recomputed in the scalar kernel** from 2Δ central differences of cell-centred velocity, for every gradient component. FDS reuses the momentum μ_t: `RHO_D_TURB=(MU-MU_DNS)/SC_T`, `KP+=(MU-MU_DNS)*CP/PR_T` (`divg.f90:122`, `:476`). That μ_t comes from compact diagonal gradients. The r271 compact patch changed only the momentum ν_t, so in the A/B the scalars kept the wide-stencil ν_t.
  2. **Face average.** RISE uses the harmonic mean of the two cells' ρD and k. FDS uses the arithmetic mean (`divg.f90:175`, `:516`).

**Size on r270 states** (`tools/fire/r271/scalar_mix.cpp`, 8 retained checkpoints, 14.98–18.47 s; `docs/data/fire_r271/scalar_mix_r270_states.txt`):
- The FDS-style compact diagonal raises ν_t by 30–80%.
- The arithmetic face mean adds only about 6% to the lateral diffusive heat flux. Both together add about 40%.
- **Below 0.5 m, the modelled diffusion is the only way heat leaves the core.** Through cylinders of r = 0.10–0.15 m, the resolved ρc_p u_r θ is net inward. The SGS + molecular flux is 0.1–0.34 kW outward.
- **Above 0.7 m, resolved transport dominates:** 0.6–1.4 kW, against 0.1–0.2 kW modelled.

**Compared with FDS's actual ν_t** (FDS `VISCOSITY − MOLECULAR VISCOSITY`, divided by density; `tools/fire/r271/fds_nut.py`) and with RISE's scalar ν_t from frames (`tools/fire/r271/frame_nut.cpp`). The frame method reproduces the checkpoint values within 3%. Units are 10⁻³ m²/s.

| band (m) | ring | FDS r263 | r270 | compact A/B |
|---|---|---|---|---|
| 0.06–0.16 | r < 0.1 m | 0.71 | 0.82 | **0.49** |
| 0.24–0.34 | r < 0.1 m | 0.98 | 0.87 | **0.42** |
| 0.45–0.55 | r < 0.1 m | 1.22 | 1.00 | **0.59** |
| 0.70–0.80 | r < 0.1 m | 1.29 | 1.08 | **0.80** |
| 0.24–0.34 | 0.1–0.2 m | 0.34 | 0.35 | **0.16** |
| 0.70–0.80 | 0.1–0.2 m | 0.66 | 0.60 | **0.31** |
| 0.70–0.80 | 0.2–0.3 m | 0.19 | 0.23 | **0.10** |

**Result.**
- **In r270, RISE's scalar ν_t is about FDS's.** It runs up to 20% higher at the base and in the outer rings, and up to 20% lower on the axis from 0.3 to 1 m. It matches only because the undamped grid-scale velocity inflates the wide-stencil gradients.
- **In the compact A/B, the velocity is smooth and the wide stencil no longer sees the shear.** The scalar ν_t falls to 40–55% of FDS's in the near field.
- So the A/B ran FDS-like momentum mixing with heat and species mixing at about half of FDS. That is consistent with §8: temperature grid noise doubled, and the core was hotter, narrower and longer.
- The fix that matches FDS is the one FDS uses: **heat and species take the same eddy viscosity as momentum**, from compact gradients.

## Next (owner decisions)

1. **Make the scalar ν_t match the momentum ν_t** under the same opt-in switch. In `evaluate_resident_transport`, take the diagonal gradients from the face velocities already bound there: (u_{i+1/2} − u_{i−1/2})/Δ, as the compact momentum kernel does. Off-diagonals stay as they are. This is a production solver change, so it needs owner review.
   - Verify it as before: Fourier/bounded checks, switch-off byte-identical, a 25-step smoke run and an independent review.
   - Then repeat the 4 s A/B with the existing harness: one arm, about 4.7 h, compared with r270 and with §8's compact arm.
   - **Option:** also switch the face mean to arithmetic, as FDS does. It is about a 6% effect and could go in the same arm or be left out to keep attribution clean.
2. **Possible follow-up from (c):** make the inflow tangential ghost zero-gradient, as the design says. It is minor.
