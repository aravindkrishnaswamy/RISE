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

## 10. Scalars on the compact ν_t: a small step, not the fix (2026-11-01)

The owner approved the change and the run. The change is still uncommitted, pending owner review (`docs/data/fire_r271/scalar_nu_t.patch`, SHA-256 `e0b57430…`).

**Change.** Under the same opt-in switch, heat and species (and the combustion mixing time) use the compact-diagonal ν_t: du_i/dx_i = (u_{i+1/2} − u_{i−1/2})/Δ, off-diagonals unchanged.
- **GPU:** `evaluate_resident_transport_compact`, selected on the host when `FireProductionForceMetalCompactViscousStencil()` is true.
- **CPU mixing time:** `BuildOpenStageTransportEvaluations3D(..., compactDiagonal)`.
- **Checks:** fp64 agreement 3.5×10⁻⁷ of the ν_t change on an r270 state (`tools/fire/r271/scalar_kernel_check.cpp`, `docs/data/fire_r271/scalar_kernel_check.txt`); switch off, the budget data rows are byte-identical over 25 steps; switch-on 25-step smoke clean; build warning-free; independent review (three P3s left open, all outside the A/B path).
- **Harmonic face mean kept**, so the arm isolates ν_t.

**Run.** Same harness and checkpoint as §8: r270 step 9223 → 4 s to 18.984 s, floor on, 4.5 h wall time, peak RSS 1.9 GB, no errors. The receipt is `r271-compact-mac-v2` (`docs/data/fire_r271/ab_scalar_result.txt`). Note: the driver manifest says `production_changed: false`; that field means the harness rewrote no production source; the production objects it links were built from the working tree, which holds the patch above (`ab_build_v2/patch_sha256.txt`). Data: `docs/data/fire_r271/ab_compare_scalar.txt`.

| | r270 (w0 / w2 / same window) | compact | compact + scalar | FDS r263 |
|---|---|---|---|---|
| Grid-scale share of w, 0.29 m | 0.27 / 0.26 / 0.27 | 0.18 | 0.18 | 0.075–0.20 |
| Grid-scale share of T, 0.29 m | 0.11 / 0.12 / 0.12 | 0.22 | **0.21** | |
| Grid-scale share of T, 0.50 m | 0.067 / 0.071 / 0.080 | 0.12 | **0.11** | |
| Total HRR rms, 2–3.5 Hz, % of mean | 3.5 / 3.9 / 3.1 | 2.2 | **2.3** | |
| 90% heat-release height (m) | 0.43 / 0.43 / 0.46 | 0.83 | **0.80** | 0.43 |
| 99% heat-release height (m) | 0.86 / 0.86 / 0.86 | 1.25 | **1.22** | 0.77 |
| θ_c at 0.50 m (K) | 792 / 796 / 825 | 970 | **960** | 631 |
| θ_c at 0.99 m (K) | 337 / 348 / 345 | 509 | **479** | 228 |
| Upward mass flux, 0.75 m (kg/s) | 0.176 / 0.208 / 0.196 | 0.168 | **0.171** | 0.244 |
| Plume width b_T, 0.75 m (m) | 0.107 / 0.105 / 0.103 | 0.083 | **0.084** | 0.119 |
| Upward mass flux, 3.01 m (kg/s) | 0.876 / 0.843 / 0.863 | 1.021 | **1.038** | 1.176 |

In 1 s blocks the 90% height goes 0.64 → 0.76 → 0.86 → 0.86 m (§8's arm: 0.64 → 0.83 → 0.89 → 0.89).

**Result.**
- **Every near-field measure moves the right way, but only slightly.** The 90% height recovers 0.03 m of the 0.37 m lost in §8; θ_c at 1 m recovers 30 K of 164 K. Both shifts are about the size of the window-to-window spread in r270.
- Temperature grid noise falls 4–6%, still well above r270. Velocity noise and puffing are unchanged.
- The far field improves a little more (3 m mass flux 1.04 kg/s).
- ν_t from frames (wide formula, `docs/data/fire_r271/frame_nut_scalar_arm.txt`) matches §8's arm within 3% below 1 m (up to 10% in the outer rings at 1.5 m), so the velocity field is statistically the same; the arm differs only in the scalar diffusivity actually applied, which the kernel check confirms rose by the expected 30–80%.

**Reading.**
- §9's explanation is not borne out. Raising the scalar ν_t by 30–80% is real but small in effect, because below 0.5 m the modelled flux is only 0.1–0.34 kW (§9). The heat that FDS moves out of the core is mostly resolved transport, and smooth RISE velocity does not do it.
- So the under-entrainment with the compact stencil is a resolved-flow problem: too little resolved radial inflow and turbulent exchange near the burner, once the 2Δ noise that was standing in for it is gone.
- The scalar change is FDS-consistent and cheap. It is worth keeping inside the opt-in path, but it does not make the compact stencil adoptable.

## 11. Where the near-field mixing is lost (read-only, 2026-11-02)

The owner reviewed §10's patch; it is committed as `7aa9953bf` (opt-in, off by default).

**Method.** `tools/fire/r271/near_planes.cpp` dumps T, u, v, w on the lowest 18 cell layers (to 0.54 m) of retained frames: r270 (235 frames, 10.47–25.09 s, the FDS window), and frames 73–136 of r270 and both compact arms. `tools/fire/r271/near_field.py` compares them with FDS r263's slices at the same cell-centred heights and the same 0.0625 s cadence (235 samples). On cylinders r = R it gives, per metre of height, the mean inflow −2πR⟨ρu_r⟩, the resolved radial heat flux 2πR c_p⟨ρu_rθ⟩, split into the mean-flow part ⟨ρ⟩⟨u_r⟩⟨θ⟩ and the turbulent rest, and the fluctuation rms. Data: `docs/data/fire_r271/near_field.txt`.

**Turbulent heat flux out of the core** (kW per m of height; the mean-flow part is inward here):

| R (m) | z (m) | r270 | compact | compact + scalar | FDS r263 |
|---|---|---|---|---|---|
| 0.10 | 0.11 | 9.8 | 2.9 | 2.9 | 10.3 |
| 0.10 | 0.29 | 8.6 | 2.9 | 3.0 | 10.7 |
| 0.10 | 0.50 | 9.6 | 5.3 | 5.5 | 12.9 |
| 0.15 | 0.29 | 2.6 | 0.29 | 0.25 | 3.3 |
| 0.15 | 0.50 | 4.7 | 0.82 | 0.73 | 7.4 |
| 0.20 | 0.50 | 0.96 | 0.091 | 0.074 | 1.41 |

**Fluctuations and inflow:**

| | r270 | compact | compact + scalar | FDS r263 |
|---|---|---|---|---|
| rms u_r at R = 0.10 m, z = 0.29 m (m/s) | 0.29 | 0.14 | 0.14 | 0.34 |
| rms w at R = 0.10 m, z = 0.05 m (m/s) | 0.23 | 0.14 | 0.13 | 0.45 |
| rms w at R = 0.10 m, z = 0.29 m (m/s) | 0.50 | 0.38 | 0.37 | 0.75 |
| Core w rms, wavelength > 0.24 m, z = 0.11 m (m/s) | 0.081 | 0.059 | 0.059 | 0.132 |
| Core w rms, wavelength > 0.24 m, z = 0.29 m (m/s) | 0.193 | 0.136 | 0.137 | 0.294 |
| Mean inflow at R = 0.20 m, z = 0.29 m (kg/s per m) | 0.155 | 0.116 | 0.115 | 0.173 |

r270's same-window values (frames 73–136) are close to its full-window values: within about 10% for the rms and inflow, and up to 30% for the small turbulent fluxes at R ≥ 0.15 m.

**Result.**
- **FDS moves heat out of the core by turbulent eddies.** At R = 0.10 m it carries 10–13 kW/m, against 0.1–0.3 kW/m modelled (§9). r270 carries 65–95% of FDS's turbulent flux; the compact arms carry 25–40% at R = 0.10 m and only 5–15% at R = 0.15–0.20 m. This is the missing near-field mixing. The scalar patch could not touch it.
- **The eddies are too weak at every scale, from the burner up.** At 0.05 m, the rms w at the burner edge is 0.23 m/s in r270 and 0.13 in the compact arms, against 0.45 in FDS. Large eddies (wavelength over 0.24 m) in the core are about two-thirds of FDS's in r270 and 37–50% in the compact arms. So the compact stencil did more than remove grid-scale noise: the whole fluctuation field weakened.
- **Even r270 is short of FDS here.** Its rms u_r matches FDS, partly through grid noise (§5), but its w fluctuations near the base are only about half. r270's turbulent heat flux looks right because of that noise.
- **Mean inflow follows.** It is 10–20% below FDS in r270, and about 30% below in the compact arms at R ≥ 0.15 m.

**Reading.**
- The flame core's under-entrainment is a fluctuation deficit. The plume leaves the burner too steady, and the eddies that should exchange heat and air grow too slowly.
- It is not the SGS model or its stencil (§§8–10), not the boundaries (§6) and not momentum advection, which is centred since r264. The rest of the step is still a candidate:
  - **damping of resolved velocity** by the projected-Heun step, the projection or the source/forcing split;
  - **weak forcing:** the burner-edge shear layer and the flame's density gradient seed the instability, and MC/FCT scalar transport smooths the density field that drives it;
  - **heat release that is too smooth in time,** which damps puffing.
- §5's grid-scale noise and this deficit are two symptoms. The noise partly stood in for the missing eddies; fixing the noise alone exposed them.

## 12. Weak forcing, not damping (2026-11-02)

> **Correction (§16, 2026-11-03):** FDS's cell-centred velocity slices are face values, while RISE's frames are cell averages, so the rms w ratios below understate RISE. Measured like for like, RISE's burner-edge rms w is 0.74–0.80 of FDS's from 0.06 to 0.5 m (not about 0.6), and 1.1 at the burner face. The temperature and heat-release comparisons are unaffected.

The owner asked for the read-only forcing/damping split, and for the operator test if it pointed at damping.

**Forcing and response at the base** (`tools/fire/r271/forcing_damping.py` on `near_planes.cpp` dumps with heat release; `docs/data/fire_r271/forcing_damping.txt`). On the burner-edge ring r = 0.06–0.12 m:

| | r270 | compact | FDS r263 |
|---|---|---|---|
| rms T at 0.05 m (K) | 211 | 139 | 216 |
| rms heat release at 0.05 m (MW/m³) | 1.37 | 0.80 | 1.21 |
| rms w at 0.05 m (m/s) | 0.26 | 0.13 | 0.40 |
| corr(w, T) at 0.11–0.50 m | 0.75–0.79 | 0.83–0.85 | 0.71–0.77 |
| Growth of peak rms w, 0.05 → 0.50 m | 3.5× | 7.3× | 3.2× |
| Production/TKE at 0.20–0.50 m (1/s) | 14–16 | 13–19 | 12–14 |
| Base heat release, 2–3.5 Hz rms (% of mean) | 2.3 | 2.4 | 3.7 |

- In r270 the temperature and heat-release fluctuations at the base match FDS, but the velocity there responds with about 60% of FDS's rms w. Above the base, r270's eddies grow with height at FDS's rate, and the energy removal rate is within 5–17% of FDS's.
- So the deficit is set within the first few cells above the burner, where heat release should turn into velocity fluctuations. It is not a progressive loss on the way up.
- The puffing-band part of the base heat release is weaker than FDS's (2.3% against 3.7%).
- A divergence check (expansion against heat release) was not conclusive: 2Δ differences of cell-centred frames at 0.0625 s are too noisy.

**Operator test.** The removal rate was slightly higher than FDS's, so, as the owner asked, the production momentum operator was tested directly on a decaying Taylor–Green vortex array.
- **Set-up:**
  - `tools/fire/r271/tg_driver.cpp`, `build_tg.py`, `run_tg.sh` run the real tier-8 capstone from t = 0. The fire runs as normal.
  - Only an external copy of the harness is patched (r260 pattern). At t = 0 it superposes a 2D Taylor–Green array, U = 1 m/s, in ambient air at z = 1.6–3.0 m. The array has zero normal flow on the sides and is exactly divergence-free on the MAC grid.
  - The patched copy can also override C_v.
- **Arms:** 0.5 s each, 306 steps, all clean:
  - 8.1-cell wavelength with C_v 0.07;
  - 8.1-cell wavelength with C_v 0;
  - 15.3-cell wavelength with C_v 0.07.
- **Measurement:** `tools/fire/r271/tg_mode.cpp` projects the MAC face velocities of the retained checkpoints onto the mode over z = 1.9–2.7 m. The plume never reached the band (ΔT = 0). Data: `docs/data/fire_r271/tg_decay.txt`.

| arm | measured decay (1/s) | molecular (1/s) | Vreman the field implies (1/s) | excess (1/s) |
|---|---|---|---|---|
| 8 cells, C_v 0 | 0.040 | 0.020 | — | 0.020 |
| 8 cells, C_v 0.07 | 0.644 | 0.020 | 0.594 | 0.030 |
| 15 cells, C_v 0.07 | 0.140 | 0.006 | 0.118 | 0.016 |

These are first-interval rates (0.06–0.07 s), before the array breaks up. After about 0.2 s, energy moves into other modes (15–27% by the end, with w up to 0.8 m/s): the array becomes unstable, as Taylor–Green arrays do at this Reynolds number.

**Result.**
- **The production momentum operator adds almost no numerical damping.** At C_v 0 an 8-cell eddy decays at the molecular rate plus 0.02 1/s, an effective numerical viscosity of about 1.6×10⁻⁵ m²/s. That is the same size as air's molecular viscosity, and three orders of magnitude below the eddy removal rates in the plume.
- **With C_v 0.07 the SGS damping is what the Vreman formula gives** for the field, plus the same small excess. FDS uses the same formula and constant.
- So damping does not explain the weak eddies. **The deficit is in forcing:** at the burner, the same temperature and heat-release fluctuations produce too little velocity fluctuation, and the puffing-band heat release is weaker.
- **Limits of the test:** it is non-reacting, at uniform density, with no buoyancy acting on the mode and a low CFL (0.05). It does not test the variable-density projection, or how heat release enters the divergence. Those are exactly where the next step should look.

**Environment note.** Homebrew moved OpenVDB from 13.1 to 14.0 on 2026-11-02 at 06:18. The committed production objects were built against 13.1, and `bin/tests/FireSequenceTest` no longer loads (`libopenvdb.13.1.dylib` not found). The test tools pin the 13.1 copy that is still in the Cellar. The main build needs either a rebuild against 14.0 or 13.1 pinned in homebrew; that is the owner's decision.

## 13. Heat release becomes expansion correctly; the base heat release is too steady (read-only, 2026-11-02)

**OpenVDB rebuild.** Done before this check, as the owner asked. RISE now builds and links against OpenVDB 14.0.
- **Source change:** one OpenVDB 14 API change in `src/Library/Utilities/FireSequence.cpp`. `io::Stream(is, delayLoad)` lost its flag, and a stream is always read in full, which is what `false` asked for. The fix is version-guarded, so 13.1 builds still compile.
- **Rebuilt objects:** only `FireSequence.o` and `tests/FireSequenceTest.o` carried 13.1 symbols. They were rebuilt, and every binary relinked.
- **Checks:** `make all` and `make tests` are warning-free, and every binary in `bin/` and `bin/tests/` links `libopenvdb.14.0`. The default `FireSequenceTest` suite passes its VDB frame load and preflight gates, with only its one known failure (the r136 gate, listed below).

**Method** (`tools/fire/r271/expansion_cv.py`; `docs/data/fire_r271/expansion_cv.txt`).
- Control volumes are cylinders r < R from the floor to z_top. The outflow Q(t) is the flux through the top and side, from cell-centred velocities, computed the same way for both codes. It is compared with the heat released inside, S(t).
- For FDS, the direct integral of its `DIVERGENCE` slice checks the method.
- Data: r270's 235 frames and FDS r263's slices over the same window.

| R = 0.15 m, z_top (m) | 0.06 | 0.12 | 0.31 | 0.49 |
|---|---|---|---|---|
| Mean expansion per watt (10⁻⁶ m³/J): RISE / FDS (DIVERGENCE) | 2.43 / 2.08 | 2.25 / 2.07 | 2.12 / 2.04 | 2.06 / 2.05 |
| Fluctuation slope dQ/dS (10⁻⁶ m³/J): RISE / FDS (DIVERGENCE) | 2.85 / 2.60 | 2.41 / 2.27 | 1.86 / 1.73 | 1.89 / 1.78 |
| corr(Q, S) at zero lag: RISE / FDS from velocities / FDS DIVERGENCE | 0.84 / 0.75 / 0.95 | 0.82 / 0.55 / 0.93 | 0.84 / 0.42 / 0.97 | 0.84 / 0.56 / 0.98 |
| rms of heat release in V, % of mean: RISE / FDS | 2.6 / 3.5 | 2.7 / 3.3 | 3.7 / 5.2 | 4.7 / 7.0 |

R = 0.20 and 0.30 m give the same picture. The ideal-gas value R/(p c_p) is about 2.4×10⁻⁶ m³/J, less what radiation removes.

**Result.**
- **The heat-release-to-expansion step is right.** RISE's mean and fluctuating expansion per watt match FDS's own divergence within 1–17% (17% only in the lowest 6 cm). The response is immediate: in both codes the correlation peaks at zero lag, with no smoothing at 0.0625 s.
- **The heat release itself is too steady on the large scale.** Integrated over the base volumes, RISE's heat release fluctuates 18–33% less than FDS's, although cell by cell its fluctuations match FDS (§12). So RISE's heat-release fluctuations are less coherent: they cancel over the volume instead of pulsing together. This matches the weaker puffing band (§12) and the weaker large eddies (§11).
- So the chain is: weaker coherent heat-release oscillation at the base, which drives less large-scale velocity, which gives weaker eddies, less entrainment and a hot, long core. The open question is why the heat release does not pulse together.

**A modelling difference that bears on it (code reading).** RISE's mixing time (`ComputeMixingTimeS`, `FireProductionSourceKernel.h`) is not FDS's (`fire.f90:809–821`, FDS r263 source on disk). Both define k_sgs = (ν_t/(0.1Δ))².

| | FDS | RISE |
|---|---|---|
| τ_u | 0.4 Δ / √(2/3 k_sgs) | Δ / √(2 k_sgs): **44% longer** |
| τ_g | √(2Δ/g), full gravity: 0.079 s here | √(2Δ/g′), reduced gravity: shorter in hot gas, longer near ambient density |
| τ_d | Δ²/D_fuel, molecular | Δ²/(D_mol + D_sgs): shorter |
| Lower bound | max(τ_chem = 10⁻⁵ s, Δ/100 m/s = 3×10⁻⁴ s) | τ_chem = 10⁻⁴ s |

The design (`FIRE_SMOKE_DESIGN.md` §3.3, line 486) chose these forms on purpose. They change how fast, and where, mixed fuel burns as the flow changes. That makes them the first candidate for the less coherent, steadier heat release. This has not yet been measured.

## 14. Mixing time: RISE burns at the buoyancy rate, FDS at the turbulence rate (2026-11-02)

The owner asked for the read-only sizing, and for the A/B to be prepared if the difference was large.

**Sizing** (`tools/fire/r271/mixing_time.cpp`; `docs/data/fire_r271/mixing_time_r270_states.txt`).
- **Method:** RISE's τ_mix comes from `ComputeMixingTimeS` on exactly the transport the run used. The tool checks that it reproduces it. FDS's formula (`fire.f90:809–821`) is applied to the same states.
- **Data:** 8 retained r270 checkpoints, 14.98–18.47 s, all ignition-eligible cells.
- **Measure:** the reactant-limited extent per step, (1 − e^(−Δt/τ)) min(ρY_F, ρY_O₂/s), the form RISE's source kernel uses.

| band (m) | extent, FDS formula / RISE | RISE: share under τ_u / τ_g | FDS formula: share under τ_u / τ_g | mean τ, RISE / FDS (s) |
|---|---|---|---|---|
| 0–0.06 | 1.03 | 0.02 / 0.98 | 0.76 / 0.24 | 0.056 / 0.055 |
| 0.06–0.12 | 1.03 | 0.03 / 0.97 | 0.91 / 0.09 | 0.047 / 0.046 |
| 0.12–0.21 | 1.09 | 0.08 / 0.92 | 0.96 / 0.04 | 0.044 / 0.041 |
| 0.21–0.31 | 1.07 | 0.08 / 0.92 | 0.97 / 0.03 | 0.044 / 0.041 |
| 0.31–0.49 | 1.21 | 0.21 / 0.79 | 0.99 / 0.01 | 0.044 / 0.037 |
| 0.49–1.00 | 1.33 | 0.48 / 0.52 | 0.99 / 0.01 | 0.049 / 0.037 |

**Result.**
- **The mean burning rate is almost the same:** FDS's formula is 3–9% faster below 0.31 m, and 21–33% faster above. That is why RISE's combustion heights match FDS's.
- **The two formulas are controlled by different terms.** In RISE, 92–98% of the base burning is set by the buoyancy time √(2Δ/g′). That depends only on the local density, not on the turbulence. Under FDS's formula, 76–97% is set by the turbulence time τ_u ∝ Δ²/ν_t, so the burning rate rises and falls with the local eddies.
- That is a direct mechanism for §13's finding. In FDS the heat release follows the large eddies and reinforces them, so it pulses together. In RISE it mostly ignores them, so it fluctuates cell by cell without pulsing together.
- The cause is two choices in RISE's design formula: the reduced gravity in τ_g makes it fast in hot gas, and τ_u = Δ/√(2k) without FDS's 0.4/√(2/3) factor makes it 44% slower.

**Prepared change** (committed opt-in after owner review, `fa316ab86`; `docs/data/fire_r271/fds_mixing_time.patch`, SHA-256 `3c1448ef…`).
- **The switch:** opt-in `RISE_FIRE_FDS_MIXING_TIME=1`, read once. In `ComputeMixingTimeS` it uses FDS's LES formula: molecular τ_d, k_sgs from the total viscosity with C = 0.1, τ_u = 0.4Δ/√(2k/3), τ_g with full gravity, and the lower bound max(τ_chem, Δ/100 m/s). The DNS path is unchanged.
- **Recording:** the launch receipt records `mixing_time r271-fds-v1`, and the manifest rebuild refuses a mismatch. `ab_driver.cpp` logs and records the setting.
- **Checks:**
  - With the switch on, the production function reproduces the independent FDS formula exactly in all 10,960 eligible base cells of the 8 states.
  - Switch off: 28 steps from r270 step 9223 reproduce r270's frame 73 value for value in every grid (`tools/fire/r271/vdb_compare.cpp`). The file bytes differ only because OpenVDB 14 encodes differently.
  - Switch on: 28 steps clean.
  - Builds warning-free. Fire test failures are unchanged (1/1/12/13), and `FireParcelIgnitionTest`, which includes the header, passes.
- **Independent review:** no P1.
  - P2, addressed: keep the compact-stencil setting the same in both arms, because the mixing-time ν_t follows it. `run_ab.sh` now takes both switches explicitly.
  - P3, minor: the switch changes only τ_mix, not FDS's whole combustion scheme; τ_d uses the mixture thermal diffusivity, which is close to methane's and never the active term; and two diagnostic tools do not log the switch.
  - The reviewer's main caveat: the mean-rate change is small, so the A/B result may sit near the window-to-window spread. The expected effect is in the fluctuations, not the means.

## 15. FDS mixing-time A/B: the switch does not fix the base forcing (2026-11-03)

> **Correction (§16):** the FDS velocity column compares FDS face values with RISE cell averages. The A/B arm-to-arm comparisons are unaffected (RISE against RISE). But the "55–65% of FDS" and "first-cell rms w 0.15 against 0.27" statements are a measurement artifact: like for like, RISE's ring rms w is 0.74–0.80 of FDS's above 0.06 m, and 1.1 at the burner face.

One arm, `ab_fdsmix_floor_on`: `RISE_FIRE_FDS_MIXING_TIME=1`, compact stencil off, floor radiation on, 4 s from r270 step 9223 (`fa316ab86` build, 4.1 h wall, exit 0). It is compared with r270's own frames 73–136, and with r270's windows w0 (frames 0–63) and w2 (137–200) for the spread a 4 s window has with no change at all. Data: `docs/data/fire_r271/fdsmix_ab_compare.txt` (`ab_compare.py … fm 'fds mixing'`) and `fdsmix_near.txt` (`fdsmix_compare.py`). The extraction tools were rebuilt against OpenVDB 14 and reproduce the r270 extracts byte for byte.

| measure | r270 w0 / 73–136 / w2 | FDS mixing | FDS r263 |
|---|---|---|---|
| base heat release (z < 0.31 m) rms, % of mean | 3.64 / 3.14 / 3.75 | 3.64 | 5.17 |
| same, 2–3.5 Hz band % | 2.36 / 1.73 / 2.49 | 1.81 | 3.71 |
| burner-edge ring rms w at 0.02 / 0.11 / 0.29 m (m/s) | 0.15 / 0.36 / 0.58 (all windows) | 0.15 / 0.38 / 0.58 | 0.27 / 0.57 / 0.85 |
| corr(w, T) on the ring at 0.02 m | 0.12 / 0.14 / 0.17 | 0.32 | 0.45 |
| total heat release, 2–3.5 Hz % | 3.5 / 3.1 / 3.9 | 2.6 | 6.3 (§4) |
| 90% / 99% heat-release height (m) | 0.428–0.459 / 0.857 | 0.428 / 0.795 | 0.428 / 0.765 |
| centre excess T at 0.29 / 0.50 m (K) | 1080–1101 / 792–825 | 1133 / 877 | 926 / 631 |
| upward mass flux at 0.50 / 0.99 m (kg/s) | 0.129–0.163 / 0.231–0.265 | 0.152 / 0.250 | 0.184 / 0.314 |

**Result.**
- **The base heat release is no more coherent.** Its rms and its puffing band sit inside r270's window-to-window spread, still about 30% short of FDS. The burner-edge velocity response is unchanged at 55–65% of FDS's.
- **The switch does what it was meant to do locally.** At the burner face, heat release now follows the velocity: corr(w, T) rises from 0.12–0.17 to 0.32, toward FDS's 0.45. But it does not feed back into stronger eddies.
- **The flame does not improve.** The core is 30–50 K hotter at 0.29–0.50 m (further from FDS), the entrained mass flux is unchanged, and total-heat puffing is slightly weaker. Only the 99% heat height moves toward FDS (0.857 → 0.795 m).
- **Conclusion:** the mixing-time formula is not why RISE's base forcing is weak. The switch stays committed but off, and is not adopted. The deficit is already present in the first cell above the burner (rms w 0.15 against FDS's 0.27 at 0.02 m), where neither the stencil nor the mixing time has had room to act.

## 16. Burner face: the boundary matches; the core holds a third of FDS's oxygen (read-only, 2026-11-03)

The owner asked for a read-only check of where FDS's velocity fluctuation at the burner comes from, and of what RISE's burner boundary imposes there.

**Tools.**
- `tools/fire/r271/burner_face.py`: radial profiles in the first layers, from the frames and FDS slices (`docs/data/fire_r271/burner_face.txt`).
- `tools/fire/r271/first_layer.cpp`: checkpoint state on 44 r270 states, 14.98–18.47 s (`first_layer_r270_states.txt`). It reports composition, the radiative loss, the exact MAC divergence and face velocities.
- `tools/fire/r271/cft_check.cpp`: sizes the extinction difference on the same states (`cft_r270_states.txt`).
- A read-only code comparison of both boundaries, with file:line citations on each side.

**Measurement correction.** FDS writes `CELL_CENTERED=T` velocity slices as raw staggered face values: U on the cell's +x face, V on +y, W on the top face (`dump.f90` GAS_PHASE_OUTPUT and DUMP_SLCF). RISE's frames hold cell averages of two faces. Two checks confirm it:
- The first-layer volume budget closes for FDS only when its W is read as the top face.
- RISE's checkpoint cell averages reproduce the frame values.

So every velocity comparison in §11–§15 set a RISE cell average against an FDS face value half a cell away, and averaging removes grid-scale variance. Like for like, on the burner-edge ring 0.06–0.12 m:

| face height (m) | 0.031 | 0.061 | 0.122 | 0.214 | 0.306 | 0.398 | 0.520 |
|---|---|---|---|---|---|---|---|
| RISE rms w, top face | 0.30 | 0.32 | 0.42 | 0.53 | 0.62 | 0.73 | 0.84 |
| FDS rms w | 0.27 | 0.40 | 0.57 | 0.72 | 0.85 | 0.98 | 1.11 |
| RISE / FDS | 1.11 | 0.80 | 0.74 | 0.73 | 0.73 | 0.74 | 0.76 |

- **RISE's mean axis w on the first interior face is 0.88 m/s, against FDS's 0.68.** The apparent "half of FDS" was the same artifact.
- **The velocity deficit is real but about 25%, not 40%, and it is absent at the burner face.** It builds over the first 0.1 m.
- **Unaffected:** temperature, density, mass fractions, heat release and divergence are true cell values in both codes, as are §13's heat-release and expansion results. Mean mass fluxes higher up (§2) are at most slightly affected, because the plume varies slowly over half a cell.

**The burner boundary itself matches FDS.**
- Fuel mass flux, injected composition and temperature are the same: about 0.0102 kg/m²s of pure methane at 300 K.
- The prescribed normal velocity and the pressure condition are the same.
- The divergence profile in the first layer is the same: rim 14.2 s⁻¹ against 11.6–13.3, core −1.7 to +3.0 against −0.45 to +5.1.

The differences found are small:
- **FDS's burner is a 300 K isothermal surface; RISE's is adiabatic.** That is about 4–6 kW/m², or roughly 20–35 K in the first cell. FDS's net Q_COND of +0.51 kW includes the floor's return of about 0.5–0.6 kW, so the burner loss is small.
- **RISE's burner-face density is the mean of the injected and cell densities,** where the design says the gas density. The face velocity is about 0.01 m/s higher, which is negligible.
- **RISE's default stencil applies about half of FDS's no-slip wall drag.** The compact stencil applies the full drag and removes RISE's 3× horizontal velocity noise near the axis (rms u_r 0.63 → 0.14, against FDS's 0.18), but it does not change the core temperature or w (§8 arm).

**The real difference: oxygen does not reach the core.** Over the burner centre (r < 0.06 m), in layers 0–3 (z = 0.015–0.11 m):

| | RISE r270 | FDS r263 |
|---|---|---|
| Y_O₂ | 0.022–0.044 | 0.063–0.072 |
| Y_CH₄ on the axis, layer 0 → 3 | 0.14 → 0.08 | 0.17 → 0.10 |
| mean T on the axis, layer 0 | 1382 K | 1095 K |
| mean molecular weight | 25.1–26.9 | 24.8–27.2 |
| heat release at the rim, r = 0.09 m, layer 0 | 7.6 MW/m³ | 6.2 MW/m³ |

- **Both codes hold the same fuel-to-rest ratio, but FDS's core holds about three times the unburnt oxygen,** and is 230–290 K cooler and denser.
- **RISE burns the oxygen at the rim** (about 20% more heat release there), so the core fills with hot products. In FDS, about three times as much oxygen gets past the rim unburnt.

**Ruled out as the cause.**
- **Extinction formulation:** the two are equivalent. FDS's EXTINCT_2 (`fire.f90:1420-1490`) is called after the step on the mixed parcel (`fire.f90:999`). Its PHI_TILDE is the parcel's burnout fraction, so its test parcel is RISE's stoichiometric parcel (`FireProductionSourceKernel.h:1485`).
- **Critical flame temperature value:** RISE uses the generic hydrocarbon 1700 K (`tools/generate_fire_simulation_records.py:1510`), and FDS r263 uses methane's 1780 K. Only 1.9% of RISE's burning below 0.49 m, and under 1% at the rim, happens in cells that would fail at 1780 K. That is not enough.
- **Mixing time** (§15) and **wall drag and stencil** (§8 arm): neither moves the first-layer core temperature (1473–1489 K in all three RISE runs, against FDS's 1252 K over r < 0.065 m).
- **Radiation distribution:** RISE scales every cell's thin emission by one global β of 0.69. FDS scales only flame cells (`radi.f90:3964-4083`), and its other cells emit κ·4σT⁴ and absorb κG. RISE's core already radiates 0.66–0.90 MW/m³. FDS's rule would add at most about 0.3–0.4 MW/m³ there before absorption, which is a minor contributor.

**What it leaves.** The core composition is set by how much rim oxygen survives into the core unburnt. With the burner boundary, the extinction rule and the mixing time ruled out, the remaining difference is the rate at which RISE's rim cells consume the oxygen that reaches them, against what FDS's partially stirred reactor does with the same mixture. FDS mixes only the fraction 1 − e^(−Δt/τ) of the cell each step and burns that fraction to completion, which is the same form as RISE's extent. So the rate difference has to come from the inputs, τ, Δt, or the cell-state history, and the sizing so far does not show it.

## 17. Like-for-like velocities, rim oxygen, and the step back (read-only, 2026-11-03)

The owner asked for §16's next items 1 and 2, then for a step back on what the bigger cause could be.

**1. §11–§13 redone like for like.** `near_field.py`, `forcing_damping.py` and `expansion_cv.py` now read FDS through a shared `fds_layers`. With `R271_FDS_CELL_AVERAGE=1`, it turns FDS's face values into the same two-face cell averages that RISE's frames hold. The layer-0 bottom face is recovered from FDS's DIVERGENCE slice, which the face divergence reproduces exactly (correlation 1.000, zero residual). With the switch off, all three scripts reproduce their committed outputs. Results are in `docs/data/fire_r271/*_cellavg.txt`.

| burner-edge ring 0.06–0.12 m, z (m) | 0.02 | 0.05 | 0.11 | 0.20 | 0.29 | 0.50 |
|---|---|---|---|---|---|---|
| rms w, r270 / FDS | 1.13 | 0.82 | 0.74 | 0.73 | 0.73 | 0.75 |
| TKE, r270 / FDS | 4.9 | 2.0 | 0.92 | 0.71 | 0.66 | 0.64 |
| corr(w,T), r270 / FDS | 0.15 / 0.45 | 0.51 / 0.78 | 0.75 / 0.80 | 0.77 / 0.80 | 0.77 / 0.81 | 0.79 / 0.82 |
| buoyancy production g⟨w′T′⟩/T, r270 / FDS | 0.44 | 0.47 | 0.65 | 0.67 | 0.66 | 0.68 |

- **§12 survives, smaller.** The forcing (rms T and q) matches FDS. The velocity response is about 25% weak above 0.1 m, not 40%. Below 0.05 m, RISE's velocity is mostly incoherent: 2–5× FDS's TKE at low correlation with T. The compact stencil removes that noise (§16).
- **§11 below 0.5 m is only modestly short:**
  - mean entrainment through r = 0.15–0.20 m: 89–94% of FDS's;
  - turbulent heat flux out of the core (r = 0.10 m): 74–93%;
  - rms w: 66–88%;
  - rms u_r: at or above FDS's.
- **§13 holds:** FDS's velocity-based expansion gain becomes 2.38 (r270 2.36), and the heat-release fluctuation results are unchanged.

**2. Rim oxygen** (`tools/fire/r271/rim_oxygen.cpp`, 44 r270 states, `rim_oxygen_r270_states.txt`).
- **The test:** on each retained state, RISE's O₂ consumption rate was set against FDS's reactor applied to the same cell. FDS's reactor mixes 1 − e^(−Δt/τ) of the cell each step and burns it to completion; it uses FDS's τ, its 2.75 ms step, extinction at 1780 K, and treats every fuel-and-oxygen cell as eligible.
- **Result:** at the rim, below 0.12 m, the ratio FDS/RISE is 0.99–1.03.
- **The core is the only exception.** FDS's 1780 K extinction makes it 10–45% slower there, but the core carries little burning.
- **The check:** RISE's computed rim heat release agrees with the frames (5.9 MW/m³ in layer 0).
- **So the chemistry is not the difference; the state reaching the rim is.**

**3. Step back: the missing air is mostly not at the burner.** Upward mass flux from §8's tables (kg/s), with Heskestad's correlation for 32.8 kW and 80% convective:

| z (m) | 0.29 | 0.50 | 0.75 | 0.99 | 1.51 | 2.00 | 3.01 |
|---|---|---|---|---|---|---|---|
| FDS r263 | 0.143 | 0.184 | 0.243 | 0.314 | 0.489 | 0.686 | 1.176 |
| r270 | 0.127 | 0.152 | 0.196 | 0.251 | 0.383 | 0.527 | 0.863 |
| RISE / FDS | 0.89 | 0.83 | 0.80 | 0.80 | 0.78 | 0.77 | 0.73 |
| FDS / Heskestad | | | | 1.28 | 1.08 | 0.98 | 0.87 |
| RISE / Heskestad | | | | 1.02 | 0.84 | 0.75 | 0.64 |

- **Below 0.3 m, RISE entrains about 90% of FDS's air.** From 0.3 m to 3 m, its entrainment rate dṁ/dz is a steady 63–79% of FDS's, including far above the flame (99% of heat is released below about 0.8 m). At 3 m both codes are near the 3.24 m top boundary.
- **FDS sits on the empirical far-plume correlation and RISE does not.** RISE has the textbook signature of a low entrainment coefficient: at 2 m the plume is narrower (b_T 0.21 against 0.26 m), hotter on the axis (105 against 72 K) and faster (2.95 against 2.68 m/s), with the same convective heat.
- **The same grid, domain, SGS model and constants, and matched heat release and expansion:** a deficit this uniform with height, in pure plume, points away from the burner and the chemistry, toward how RISE resolves a turbulent buoyant plume.

**Hypotheses, ranked by the evidence.**
1. **Velocity energy sits at the wrong scales.**
   - RISE's momentum advection is centred and energy-conserving by design (FIRE_SMOKE_DESIGN.md, momentum flux). Its default stress stencil cannot damp 2Δ waves (§5), and Vreman's ν_t cannot see them either. Nothing removes grid-scale velocity energy, which the FDS discretisation does.
   - *For:* RISE carries 2–3× FDS's grid-scale w′ at every height, including 1–2 m, where FDS is 0.075–0.13 and RISE 0.21–0.24 (§5; horizontal spectra, so unaffected by the face/average issue). It carries less in large eddies (0.37 against 0.45 at 0.99 m). The compact-stencil arm (§8) closed about half the far-field gap at 3 m (0.86 → 1.02, FDS 1.18).
   - *Against or unexplained:* the compact arms made the near field hotter and quieter, with base TKE far below FDS's. The Taylor–Green test shows that resolved 8Δ eddies are not over-damped, so the issue is distribution, not dissipation.
2. **The plume inherits a wrong structure from the flame zone and does not recover by 3 m.** The core is hot and narrow (§16's rim oxygen), the base perturbations are incoherent, and the far plume carries that forward.
   - *Test:* whether RISE's turbulence intensity and large-eddy share approach FDS's with height (inherited) or stay low (local).
3. **A grid-scale source rather than a missing sink:** per-cell expansion noise (§4) injected through the projection.
   - *Test:* the grid-scale share of the divergence field, RISE against FDS's DIVERGENCE slices.
4. **Already bounded:**
   - boundaries (§6);
   - burner boundary, chemistry, extinction, mixing time (§14–§16);
   - averaging artifacts (this section; mean mass fluxes are face/average-insensitive to about 1%).

## 18. Two problems, not one: the far plume is numerics, the near-source plume is too quiet (read-only, 2026-11-03)

The owner asked for §17's read-only discriminators, and for the non-reacting plume test (§19).

**Tools and data.**
- `tools/fire/r271/far_field.py`: r270 frames and the compact arms (§8, §10), against FDS converted to cell averages.
- `tools/fire/r271/div_planes.cpp`: exact MAC divergence planes from 44 r270 checkpoints.
- Output: `docs/data/fire_r271/far_field.txt`.

**Far-plume turbulence, like for like.**

| z (m) | measure | r270 | compact | FDS |
|---|---|---|---|---|
| 0.29–0.50 | centre rms w / w_c | 0.20–0.27 | 0.13–0.22 | 0.22–0.34 |
| 0.29–0.50 | ∫w′² dA / (w_c b_w)² | 0.12–0.17 | 0.06–0.12 | 0.17–0.26 |
| 0.99 | rms w / w_c; large / grid share of w′ | 0.29; 0.37 / 0.22 | 0.37; 0.40 / 0.08 | 0.37; 0.46 / 0.12 |
| 2.00 | rms w / w_c; ∫w′²; ⟨u_r′θ′⟩/(w_c θ_c) at r = b_w | 0.26; 0.32; 0.014 | 0.32; 0.39; 0.026 | 0.34; 0.44; 0.025 |
| 2.00 | large / grid share of w′ | 0.51 / 0.24 | 0.68 / 0.055 | 0.68 / 0.067 |

- **Above 1 m, the deficit is local numerics, and the compact stencil removes it.** In r270, a fixed 20–24% of w′ variance sits at grid scale at every height, and the large eddies, the turbulent intensity and the turbulent heat flux stay low. With the compact stencil, all of them match FDS from 1.5 to 3 m. Its entrainment rate is 0.92 of FDS's from 1.5 to 2 m and 0.98 from 2 to 3 m, against r270's 0.73 and 0.69.
- **Below 1 m, the compact arm is too quiet.** Its intensity and fluctuation energy are about 60% of FDS's at 0.3–0.5 m, and its core is hot. r270's grid-scale noise had partly stood in for the missing near-source fluctuation. The far plume of the compact arm inherits a narrower, hotter start (b_w 0.248 against 0.279 m at 2 m), so its total mass flux stays short (0.54 against 0.69 kg/s at 2 m) even though its local entrainment rate is right.

**Divergence (hypothesis 3).** The fluctuation about the mean, over r < 0.3 m:

| z (m) | 0.05 | 0.11 | 0.29 | 0.50 | 0.99 | 2.00 |
|---|---|---|---|---|---|---|
| rms, r270 / FDS (s⁻¹) | 1.25 / 1.05 | 0.81 / 0.65 | 0.59 / 0.71 | 0.49 / 0.60 | 0.33 / 0.39 | 0.13 / 0.12 |
| grid-scale share, r270 / FDS | 0.50 / 0.35 | 0.49 / 0.42 | 0.44 / 0.41 | 0.44 / 0.44 | 0.43 / 0.46 | 0.50 / 0.42 |

RISE's expansion field has FDS's magnitude and nearly its grid-scale content, with a modest excess only at the base. A grid-scale expansion source is not the main cause.

**Conclusion: there are two separate problems.**
1. **Far plume:** RISE's default velocity discretisation keeps grid-scale velocity energy and starves the large eddies, which lowers the entrainment coefficient. The compact stencil fixes it.
2. **Near source:** with that noise removed, resolved fluctuations develop too weakly over the first metre. This sets the plume's hot, narrow start and most of the total mass deficit.

Whether problem 2 is specific to the flame, or holds for any area heat-source plume, is what the non-reacting test (§19) decides.

## 19. Non-reacting plume pair: setup (running, 2026-11-03)

The owner asked for the test. It decides whether §18's too-quiet near-source plume is a flame effect, or holds for any heat-source plume in RISE.

**Physics, identical in both codes.**
- The case: the production methane case and its domain, grid, burner, floor and boundaries.
- Methane inflow unchanged: 0.0102 kg/m²s at 300 K.
- No combustion and no radiation.
- A frozen volumetric heat source of 26.4 kW, the fire's convective share 0.8 × 33 kW. It is uniform over the 69 burner cells of each layer 0–26 (z 0–0.83 m) and split between layers by FDS r263's time-mean heat release. That carries 99.5% of the fire's heat, 29% of it in layer 0 (`docs/data/fire_r271/inert_source_profile.json`).
- **Why the heat is spread:** a first design put all of it in layer 0, about 13 MW/m³. That would heat the core to around 10⁴ K, past RISE's 2300 K accepted-state ceiling, so it was abandoned before any long run.
- **Startup:** a fixed W/m³ source runs away in still air until the plume moves. In 1.25 s smoke runs the peak cell reached 2810 K in FDS (at 0.38 s) and 1993 K in RISE (at 0.31 s). Both then settled, FDS at 1750–2150 K.

**FDS.** `tools/fire/r271/make_inert_fds.py`, from r263's input:
- `&REAC` becomes `&SPEC METHANE`;
- the burner SURFs use MASS_FLUX = HRRPUA/HOC;
- `&RADI RADIATION=F`;
- 243 `&INIT HRRPUV` boxes;
- the oxygen slices are dropped.

The generator asserts that RISE's compiled-in table equals the profile file. The run covers 25.09 s, so the statistics window is r263's own.

**RISE.** Opt-in `RISE_FIRE_INERT_PLUME=1`, uncommitted pending owner review (`docs/data/fire_r271/inert_plume.patch`).
- **The source authority:**
  - ignition eligibility is forced false and soot oxidation is off;
  - the pilot setpoint is 0;
  - `BuildFrozenMethaneSourcePackets` gains a `gasRadiation` flag that skips the exchange;
  - the dose is added to each packet's energy before the expansion target is derived, as the r270 floor dose is.
- **Recording:** receipt `inert_plume r271-v1` with a rebuild guard. The ignition-window gate is skipped, and the legacy source path refuses the inert case.
- **Pilot code:** the pilot arithmetic moved unchanged into an out-of-line helper; its disassembly is identical to HEAD's, all 155 instructions.
- **Driver:** `tools/fire/r271/inert_driver.cpp` with `build_inert.py`, a harness copy that stops at the window and anchors 16 Hz frames at an origin > 0.

**Checks.**
- Builds are warning-free and the fp64/trace mirrors are regenerated.
- Switch off: a resume from r270 step 9223 reproduces its frame 73 value for value in every grid. The fire test failure sets are unchanged; FireProductionSolverTest showed 3 extra timing-allocation failures under load, and the baseline 12 when rerun alone.
- Switch on: zero reaction everywhere, and the receipt is recorded.
- **Independent review: no P1.**
  - Fixed: the frame off-by-one for an origin at the start time; noinline for the new FireCase code; no gas-exchange evaluation when radiation is off.
  - Documented: the heat source appears in no heat-release diagnostic; the child exits non-zero at the end by design; resume paths do not check the flag.

**Runs** (started 2026-11-03):
- FDS 25.09 s;
- RISE `inert_compact` (compact stencil) and `inert_legacy` (default stencil), each 14 s from t = 0 with frames from 2 s.

Statistics will be compared over matching windows, with FDS's long window to check stationarity.

## 20. Non-reacting plume result: RISE's plume does not transition near the source (2026-11-04)

**Runs.**
- FDS `r271_inert`: 25.09 s in 10,688 s wall, exit 0.
- RISE `inert_legacy` (default stencil) and `inert_compact` (compact stencil): 14 s each from t = 0, run side by side, 14.6 h and 15.8 h. Each has 193 frames from 2 s.
- **Exits:** both RISE arms exit 98 by design (§19). Their hottest cells stayed at 1680–1840 K.

**Data and windows.** `tools/fire/r271/inert_compare.py`, like for like (FDS converted to cell averages); results in `docs/data/fire_r271/inert_vs_fds.txt`. Comparisons use matched 6–14 s windows. FDS's own 6–14 s and 10.47–25.09 s windows differ by 8–10% in mass flux at 1–3 m (`inert_fds_windows.txt`), which is the noise yardstick.

| z (m) | quantity | FDS | RISE compact | RISE default |
|---|---|---|---|---|
| 0.29 | centre rms w / w_c | 0.079 | 0.013 | 0.141 |
| 0.50 | centre rms w / w_c | 0.147 | 0.024 | 0.163 |
| 0.99 | centre rms w / w_c | 0.312 | 0.094 | 0.236 |
| 2.00 | centre rms w / w_c | 0.318 | 0.282 | 0.235 |
| 0.29 | mass flux (kg/s) | 0.064 | 0.051 | 0.065 |
| 0.99 | mass flux (kg/s) | 0.211 | 0.121 | 0.199 |
| 3.01 | mass flux (kg/s) | 0.922 | 0.454 | 0.737 |
| 2.00 | centre excess T (K); b_w (m) | 101; 0.224 | 317; 0.131 | 133; 0.203 |
| 2.00 | w′ grid-scale share | 0.090 | 0.096 | 0.272 |
| 0.3–1.0 | entrainment rate, RISE / FDS | | 0.45–0.50 | 0.87–0.93 |
| 1.0–3.0 | entrainment rate, RISE / FDS | | 0.42–0.54 | 0.71–0.84 |

**Result.**
- **The flame drives most of FDS's near-field turbulence.** FDS's inert plume is much quieter near the source than its fire: rms w/w_c at 0.29 m is 0.08 against 0.22, and the mass flux at 2 m is 0.51 against 0.69 kg/s.
- **With the compact stencil, RISE's inert plume stays nearly laminar for the first metre** and only transitions by 2–3 m. It is narrow and hot, and entrains about half of FDS's air at every height. FDS's plume, with the same physics, grid and inputs, develops fluctuations from the start (0.08 at 0.29 m, 0.31 by 1 m).
- **With the default stencil, the near source looks right** (mass flux within 2% to 0.5 m). But its fluctuation is partly grid noise, with a grid share of 0.29–0.37 against 0.21–0.28. The far field under-entrains exactly as in the fire, with 2–4× FDS's grid-scale energy and half the edge turbulent flux.

**What it means.** Combined with §18, there is one underlying cause, which is generic to RISE's hydrodynamics and not to combustion. **Resolved instabilities do not grow in RISE as they do in FDS.**
- The default stencil hides this near the source by injecting grid noise, which seeds fluctuation but starves the large eddies.
- The compact stencil removes the noise and exposes a plume that is too stable.
- In the fire, the flame's fluctuating heat release supplies some forcing, so the compact fire arm was quiet but not laminar (§8, §18).

**Candidates for the over-stability, ranked:**
1. **SGS eddy viscosity in the thin laminar shear layer near the source.** Vreman's model does not vanish in pure shear, and RISE's ν_t is discretised differently from FDS's (§5, §9). Too much ν_t in the shear layer damps the Kelvin–Helmholtz growth that starts transition.
2. **Numerical diffusion of the temperature shear layer** by the limited scalar advection (MUSCL-MC FCT, against FDS's CHARM). A thicker layer grows instabilities more slowly.
3. **Missing seed.** Both codes have the same symmetric inputs. FDS may carry more round-off- or scheme-level perturbation, so its transition is seeded and RISE's is not.

## 21. Not over-damped, not smeared; seed test set up (2026-11-04)

The owner asked for §20's next items in order.

**1. Eddy viscosity and shear layers, read-only** (`tools/fire/r271/inert_shear.py`, `docs/data/fire_r271/inert_shear.txt`).
- **Data:** inert arms over 6–14 s, against FDS over the same window, converted to cell averages.
- **Measures:**
  - shear-layer vorticity and thermal thicknesses from the mean profiles;
  - Vreman ν_t from each code's resolved velocity by one formula (Cv 0.07, 2Δ gradients, as FDS `velo.f90:244`);
  - FDS's actual (VISCOSITY − MOLECULAR VISCOSITY)/ρ;
  - the shear-layer Reynolds number w_c d_w / (ν + ν_t).

| z 0.05 → 0.50 m | FDS | RISE compact | RISE default |
|---|---|---|---|
| ν_t, formula (10⁻⁴ m²/s) | 3.1 → 8.1 | 4.5 → 2.3 | 4.2 → 10.1 |
| ν_t, FDS's actual output | 3.6 → 10.9 | | |
| vorticity thickness d_w (m) | 0.110–0.123 | 0.094–0.126 | 0.116–0.137 |
| thermal thickness d_T (m) | 0.087–0.132 | 0.074–0.123 | 0.111–0.123 |
| shear-layer Re | 495–820 | 340–1380 | 310–640 |

- **The formula reads FDS's actual ν_t 12–25% low,** so it is a fair estimator of the resolved-field eddy viscosity.
- **Candidate 1, excess SGS viscosity, is ruled out.** Above 0.1 m the compact plume carries less ν_t than FDS and has a higher shear-layer Reynolds number (770–1380 against 600–820).
- **Candidate 2, smearing, is ruled out.** RISE's shear layers are as thin as FDS's or thinner.
- A thinner, higher-Re layer should be more unstable, not less. That it stays laminar points to **candidate 3: there is no disturbance for the convective instability to amplify.** RISE's GPU solver is deterministic and its inputs are symmetric. FDS's discretisation evidently carries enough perturbation to trigger transition near the source.

**2. Seeded compact arm** (opt-in `RISE_FIRE_INERT_SEED=ε`, only with `RISE_FIRE_INERT_PLUME=1`; uncommitted with §19's patch).
- **The seed:** each burner cell's dose is multiplied by 1 + ε·ξ, with ξ uniform in [−1, 1] from `SplitMix64` of the cell and a 10 ms time bucket. It is renormalised per layer, so the total is exactly 26.4 kW.
- **Determinism:** Heun stages and retries share the bucket, so they see the same seed.
- **Recording:** the receipt records `inert_seed`, the rebuild guard checks it, and it enters the source identity hash.
- **The run:** ε = 0.2, compact stencil, 14 s from t = 0. Result in §22: a seed helps but is not the main cause.

## 22. A seed helps by a third; the deficit forms at the floor (2026-11-05)

**Seeded compact arm** (ε = 0.2, compact stencil, 14 s from t = 0, frames from 2 s). Finished at t = 14 s, 193 frames, peak RSS about 2 GB. Exit 98 is the harness's designed stop.

Like for like over 6–14 s (`inert_compare.py`, `docs/data/fire_r271/inert_seed_vs_fds.txt`):

| Measure | FDS | seeded compact | compact | default |
|---|---|---|---|---|
| centre rms w/w_c at 0.29 m | 0.079 | 0.022 | 0.013 | 0.141 |
| centre rms w/w_c at 0.99 m | 0.312 | 0.136 | 0.094 | 0.236 |
| θ_c at 0.99 m (K) | 339 | 675 | 714 | 363 |
| mass flux at 3.01 m (kg/s) | 0.922 | 0.608 | 0.454 | 0.737 |
| entrainment rate / FDS's, 0.3–3 m | 1 | 0.55–0.77 | 0.42–0.54 | 0.71–0.93 |

- **The seed moves every measure the right way,** but it closes only a third of the 3 m mass-flux gap.
- **The near-source plume stays nearly laminar:** a ±20% cell-by-cell heat-source perturbation raises rms w/w_c at 0.29 m only from 0.013 to 0.022, against FDS's 0.079. That perturbation is far larger than any numerical noise.
- **The seeded run is statistically steady:** 6–10 s and 10–14 s agree within FDS's own spread, for example 0.601 and 0.620 kg/s at 3 m. The unseeded compact arm was still drifting (0.394 and 0.517).
- **Shear layers are unchanged** (`docs/data/fire_r271/inert_seed_shear.txt`): same thicknesses, ν_t up by about 20% at 0.3–0.5 m, Re 1100–1260.
- **Conclusion:** a missing seed is a contributor, not the main cause.

**Read-only follow-ups:**
- **Puffing is present** (`tools/fire/r271/inert_spectra.py`, `docs/data/fire_r271/inert_spectra.txt`). Every arm's centre w energy sits mostly in the 2–4 Hz band. FDS peaks at 3.0 Hz at 0.29 m, against the 1.5/√D estimate of 2.8 Hz. RISE has the mode at the right frequency, at about a quarter of the amplitude.
- **The deficit forms at the floor** (`tools/fire/r271/inert_growth.py`, `docs/data/fire_r271/inert_growth.txt`). Rms per near-field layer, 6–14 s:

| z (m) | | FDS | seeded | compact |
|---|---|---|---|---|
| 0.015 | burner-edge w′ (m/s) | 0.050 | 0.011 | 0.011 |
| 0.015 | burner-edge θ′ (K) | 12.6 | 5.6 | 1.9 |
| 0.015 | centre w′ (m/s) | 0.019 | 0.008 | 0.005 |
| 0.29 | burner-edge w′ (m/s) | 0.263 | 0.148 | 0.131 |
| 0.29 | centre w′ (m/s) | 0.303 | 0.090 | 0.055 |

- **Growth is not the problem.** From the first cell to 0.29 m, RISE's burner-edge w′ grows ×12–13 against FDS's ×5. RISE's instabilities amplify at least as fast; they start about 4–5× smaller in the first cell.
- **The source is fixed in both codes,** frozen heat and fixed fuel inflow. So FDS's first-cell w′ and θ′ have to come from the flow arriving at the burner: the floor inflow flapping with the puffing cycle, or the pressure field feeding the upper plume's unsteadiness back to the base.
- **Leading hypothesis:** RISE's base does not feel the plume's unsteadiness, through weaker pressure feedback or a damped near-floor inflow. This fits the fire case's burner-edge deficit (§16, §17) and is generic to the hydrodynamics.

## 23. The missing seed is small-scale convection over the heated base (read-only, 2026-11-05)

The owner approved §22's next item. RISE stores no pressure in frames or checkpoints, so pressure is compared on the FDS side only. Velocity and temperature are compared like for like over 6–14 s.

**1. The base does not respond to the puffing cycle, in FDS either** (`tools/fire/r271/inert_floor.py`, `docs/data/fire_r271/inert_floor.txt`).
- **Signals:** azimuthal means, so the axisymmetric puffing mode:
  - the radial floor inflow just outside the burner;
  - the burner-edge w in the first cell;
  - FDS's floor pressure.
- **Comparison:** each against centre w and pressure at 0.5 and 0.99 m, by coherence in the 2–4 Hz band.
- **FDS's coherences are all at the noise floor** (0.01–0.13, floor about 0.15).
- **FDS's floor pressure barely moves:** rms 0.0125 Pa, against 0.32 Pa at 0.99 m.
- **The axisymmetric floor inflow fluctuates about as weakly in FDS as in RISE** (rms 0.004 against 0.002–0.003 m/s).
- §22's hypothesis is ruled out: neither pressure feedback nor a puffing-driven floor inflow seeds FDS's base.

**2. The disturbance is not brought in by the room** (`tools/fire/r271/inert_ambient.py`, `docs/data/fire_r271/inert_ambient.txt`). Local rms of u, v, w and θ by radial band:

| layer 0 (z 0.015 m) | FDS | seeded | compact | default |
|---|---|---|---|---|
| burner interior u′ (m/s) / θ′ (K) | 0.054 / 34 | 0.017 / 19 | 0.009 / 6 | 0.34 / 101 |
| burner edge w′ / θ′ | 0.047 / 8.4 | 0.010 / 3.6 | 0.010 / 1.3 | 0.071 / 20 |
| just outside u′ | 0.017 | 0.006 | 0.005 | 0.070 |
| ambient 0.25–0.45 m u′ | 0.004 | 0.002 | 0.002 | 0.029 |
| ambient 0.6–1.0 m u′ | 0.001 | 0.000 | 0.000 | 0.002 |

- **FDS's base unsteadiness is largest inside the burner,** over the heat source, and falls off outward. The room is quiet in both codes.
- **It is small-scale:** in layers 0–3, 55–65% of FDS's θ′ energy is at 2.7–8 cells and only 13–14% at grid scale.
- **RISE compact has the same spectral shape at 4–6× lower amplitude.** The default stencil's base is grid noise (20–22% grid-scale θ′, with 101 K interior rms).

**Reading.** FDS's seed is local buoyant convection in the heated gas over the burner, at 3–8 cell scales. It is self-sustained, independent of the plume above, and grows into the near-source shear layer. RISE damps it. §12's fire finding, that base heat-release fluctuations produce about 60–75% of FDS's velocity response, is the same effect seen in the flame. A ±20% heat-source seed does not restore it (§22), so RISE's small-scale response to buoyancy is weak, not merely unforced.

**Candidates, in RISE's discretisation at 3–8Δ:**
1. buoyancy-to-velocity response: cell-to-face buoyancy interpolation and the projection;
2. temperature advection: RISE's scalar scheme against FDS's CHARM limiter, damping 3–8Δ temperature structure;
3. the volumetric heat-source coupling to divergence at small scales.

The Taylor–Green test (§12) covers only momentum dissipation of 8Δ eddies, not these.

## Next (owner decisions)

1. **Two small synthetic tests through the real production capstone** (new runs, short; harness copies only, as `tg_driver.cpp`):
   - **Buoyancy response:** in quiescent ambient air, impose a small sinusoidal temperature perturbation at 3, 4, 6 and 8Δ. Compare the first-interval w growth against the linear analytic rate, g θ′/T · k_h²/(k_h²+k_z²). This tests candidates 1 and 3.
   - **Scalar advection:** advect a 3–8Δ temperature pattern in uniform flow. Compare its decay per cell crossed against a CHARM reference. This tests candidate 2.
2. Whichever test fails points to the operator for an owner-reviewed opt-in fix and an inert A/B.
3. The seed and inert-plume opt-ins stay uncommitted pending owner review. The §16 small fixes stay optional.
