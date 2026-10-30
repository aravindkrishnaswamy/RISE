# r270 — floor radiation absorption

2026-10-30. Design by Claude.

**Status.**
- **Decision:** the owner chose this direction on 2026-10-30 (make RISE's floor absorb flame radiation, the physically based route) and approved the §6 recommendations the same day.
- **Implementation:** done (§7), and verified up to the short tier-6 run.
- **The tier-8 window has not been run.** It needs owner approval.

**Motivation.** See [r269](FIRE_SMOKE_AMBIENT_INFLOW_R269.md):
- FDS's `ADIABATIC=T` floor absorbs radiation and returns about 0.5 kW to the gas by convection.
- That warms the near-floor air out to about 1 m (3–30 K) and drives an annular updraft of 0.02–0.06 kg/s.
- RISE's floor is adiabatic only to the gas, so the radiated energy (χ_r = 0.20, about 6.6 kW) never heats it.

## 1. Physics

Treat the bed outside the burner patch as a gray, opaque, zero-heat-capacity surface: adiabatic in the FDS sense, with zero *net* flux. On each floor face the energy balance is

> ε·(G − σT_w⁴ + σT∞⁴) = h·(T_w − T₁)

The terms:
- **G** is the irradiance from the gas, above the ambient background (W/m²).
- **ε** is the emissivity, which also serves as the absorptivity.
- **T_w** is the wall temperature.
- **T₁** is the gas temperature in the first cell above the face.
- **h** is a convective coefficient.
- **σT∞⁴** is the ambient irradiance through the open faces.

The convective flux **q_c = h(T_w − T₁)** enters the gas through that face. The floor's re-emission leaves the domain unabsorbed, which is consistent with RISE's optically thin gas model.

The left side falls with T_w and the right side rises with it, so the root is unique. It is bracketed by [T₁, T_max], where T_max solves εσ(T⁴ − T∞⁴) = εG.

**Irradiance (optically thin, consistent with §3.5).** Each cell already has a certified net emission field, `radiativeCoolingWPerM3` (e_c). With isotropic emission and no gas absorption along the path:

> G(x_f) = Σ_c e_c·V_c·z_c / (4π·|x_c − x_f|³)

This is the cosine-weighted point-kernel. z_c is the cell-centre height above the floor.
- The kernel depends only on horizontal offset and z_c.
- So for each z layer, G is a 2D convolution: 106 layers, each zero-padded from 69×69 to 138×138.
- That is about 1e8 flops per step by FFT. Alternatively, a direct sum over emitting cells only (e_c > 0) would be bit-reproducible without an FFT.

**Coefficient h.** Use FDS 6.11's default LES coefficient, read from the source (`func.f90` `HEAT_TRANSFER_COEFFICIENT`, upward-facing floor, IOR = 3):

> h = max(Nu_free, Nu_forced, 2L/Δz)·k/L, with L = 1 m

- **Free convection:** 0.54 Ra^¼ when Ra < 1e7, otherwise 0.15 Ra^⅓, if the wall is hotter than the gas; 0.52 Ra^0.2 if it is cooler.
- **Forced convection:** Nu_forced = 0.0296 Re^0.8 Pr^⅓.
- **Rayleigh number:** Ra = g|ΔT|L³(ρ/μ)²·0.7/T_film.
- **Properties:** μ and k are evaluated at the film temperature T_w + (T₁ − T_w)/3.
- **Expected value:** for a heated floor, free convection dominates at about 6.5 W/m²K. Resolving the conduction directly as k/(Δz/2) gives about 1.7 W/m²K at Δz = 3 cm, so it would under-predict badly. A correlation is required at this resolution.

**Expected size.** For a point emitter of 6.6 kW at 0.35 m height over the 2.1 m square floor:
- The floor intercepts about 36%, or about 2.4 kW.
- Near r = 0.3 m: G ≈ 1.9 kW/m², T_w ≈ 390 K, q_c ≈ 0.9 kW/m².
- Integrated q_c is of order 0.5–0.9 kW.

This matches FDS's floor Q_COND of +0.51 kW. FDS's figure is net of its isothermal 300 K burner, which absorbs heat from the gas.

## 2. Time integration: frozen per step, like the radiation sink

- **Inputs from the step start.** The authority already builds the radiation field once per step attempt, from the beginning state, before R0 (`FireProductionCanonicalSourceAuthority::Build`). From that field and the beginning-state T₁ and velocity it would compute G, T_w, h and q_c per floor face, on the CPU in fp64.
- **q_c is then a prescribed wall flux.** It is constant over R0, R1 and the Heun-averaged commit, exactly like the burner's imposed flux.
- **Why explicit is stable.** The first cell's thermal time constant is ρc_pΔz/h ≈ 36/10 ≈ 3.6 s, and dt ≈ 1.6 ms.
- **What this avoids:**
  - fp32/fp64 disagreement between the CPU and Metal paths, because both read the same host array;
  - a Newton solve inside the stages;
  - any new checkpoint state, because everything is recomputed from the step-start state. That also keeps a future resume consistent.
- **The cost:** q_c lags the gas by up to one step (about 1.6 ms). That is negligible against the 3.6 s time constant.

## 3. Where it plugs in (code map)

- **Case parameter `floor_emissivity`** (authored), with derived echo `floor_emissivity_effective`. It follows the `chi_r` path (`FireCase.h:34`, `FireCase.cpp:309/404/414/496–563`).
  - **Use 1.** FDS's `ADIABATIC=T` forces `EMISSIVITY=1` (`read.f90:8931–8935`), so 0.9 is not "the FDS default" for r263. This also means r269's proposed FDS `EMISSIVITY=0` rerun would have been silently overridden.
  - **0** reproduces today's model exactly. That gives an exact regression switch.
- **Records:**
  - The transport record's `wall_heat_flux = "adiabatic"` pin (`FireSimulationRecords.cpp:3573`, generated by `tools/generate_fire_simulation_records.py:1513`) becomes `"radiatively-adiabatic-gray"`, in a new record version.
  - The burner policy preimage (`FireSequenceTest.cpp:1884`) gets a version bump.
  - These change `caseRecordId`, so the pinned IDs move: `FireSequenceTest.cpp` 10682/12262/13052/13125, `r245_budget_audit.py:239`, `r244_verify_baseline.py:28`, `r247_source_admission.py:26`.
- **Authority:** compute `floorConvectiveFluxWPerM2[face]` next to the radiation pass (`FireProductionSource.cpp:294–347`). Store it in the seal, hash it in a bumped `SourceGlobalRadiationIdentity`, and add it to `Matches`.
- **Transport request:** add a per-face imposed wall energy flux on `FireProductionScalarPhysicalFluxPrerequisiteRequest` (`FireProductionTransport.h:220`), hashed in `HeunStageInputIdentity` next to `HashPrescribedInlet`. Set `physicalEnergyFluxWPerM2` for z = 0 wall faces:
  - on the CPU at `FireProductionTransport.cpp:2184`;
  - in Metal at `FireProductionAdvectionMac.mm:2652` (`evaluate_resident_physical_flux`).
  The flux is energy only, with zero mass and species flux, and the faces stay no-slip.
- **Budget:** no reducer change. The heat appears as `bottom_nonburner_in` in the step CSVs, and closure stays `ending − beginning − source + out − in`.
  - Add a per-step monitored diagnostic: floor q_c total (W), maximum T_w, and floor-intercepted irradiance (W).
  - `radiated_J` keeps meaning gas emission (χ_r·Q). Net domain loss is `radiated − floor_in`.
- **Tests to update or add:**
  - The legacy "unmasked bed remains adiabatic" asserts (`FireSimulationSolverTest.cpp:1873`, `:1935`) apply only at ε = 0.
  - Add a `FireCaseTest` identity pair for the new field.
  - Add a seal round-trip field (`FireSequenceTest.cpp:17187+`).
  - Keep the copies under `tests/fire_production_fp64/` and `tests/fire_production_trace/` in sync.

## 4. Verification plan

1. **Kernel fixture.** A single emitting cell must reproduce G = P·z/(4π r³) and the total intercepted power from the analytic solid angle of a square.
2. **Surface solve fixture.** Check the bracket and root, and the limits ε → 0 (q_c = 0) and G → 0 (T_w = T₁).
3. **ε = 0 bit-identity.** A two-step production witness at ε = 0 must match the current binary's fields and budgets exactly.
4. **Short tier-6 run at ε = 1.** Budget closure stays in its roundoff envelope. The step budgets' source term includes the q_c total (§7). Floor-cell air outside the burner warms.
5. **Full tier-8 window from zero.** This needs owner approval: about 23 h, like r264. Compare against r263 on:
   - floor-cell ΔT(r) (FDS 30/15/9.8/4.6/3.0/1.9 K);
   - annulus updraft (0.02–0.057 kg/s);
   - near-floor side inflow;
   - flame height (FDS 0.81 m);
   - the McCaffrey stations.

## 5. Mismatches that remain against FDS (out of scope)

- **Burner surface.** FDS's burner is isothermal at 300 K; RISE injects at its fuel record temperature. FDS's burner pulls heat out of the gas, so its +0.51 kW is net of a negative burner term.
- **Gas self-absorption.** FDS's RTE solver lets the gas absorb some radiation; RISE stays optically thin, as decided in §12.
- **How emission is spread.**
  - FDS applies its χ_r correction only in cells with significant HRR. Elsewhere, hot products emit at their own κσT⁴, so r263's net Q_RADI of 6.87 kW is about 4% above χ_r·Q.
  - RISE rescales every cell's emission uniformly to exactly χ_r·Q.
  - So the split of emission between flame and plume differs, and that shifts G(r) on the floor.

## 6. Decisions for the owner

1. **Frozen per-step q_c (§2),** rather than a per-stage implicit solve. Recommended.
2. **ε = 1** in production, which is what FDS `ADIABATIC=T` forces (§3; corrected from 0.9).
3. **FDS's default h correlation,** to compare like with like.
4. **Irradiance by a direct sum over emitting cells,** with FFT only if profiling shows it is needed.
5. **The FDS `EMISSIVITY=0` rerun** proposed in r269 becomes optional. It would confirm the attribution independently, but no longer gates this work.
6. **Approval of the tier-8 run** in §4 step 5, once steps 1–4 pass.

## 7. Implementation (2026-10-30)

These points differ from §3:

- **Floor heat enters as a source.** The floor's convective heat goes into the first gas layer as a frozen energy dose q·dt/Δz, added inside `FireProductionCanonicalSourceAuthority::Build`. This happens before the per-cell expansion target is derived, and the packet is then re-represented in binary32.
  - It is exactly equivalent to a face flux for the finite-volume energy update. FDS also applies its wall flux as Q_CON_F·RDN.
  - It leaves the resident CPU/Metal transport untouched.
- **Budget routing.** In the step budgets the heat appears in the **source** term, not in `bottom_nonburner_in`. Energy closure still holds, because the source term is the sealed dose. The chemical/radiated/pilot component line does not include it; `budgets/floor_radiation.csv` reports it per step.
- **No record bump.** The floor is an optional authored case field. When it is absent, the case bytes are unchanged; when it is present, they gain `floor_emissivity` and a derived `floor_radiation` block with model version `r270_fds_adiabatic_gray_floor_thin_point_kernel_v1`. Because of this, the transport-record pin and the burner policy stay as they were.
- **Switch:** `RISE_FIRE_FLOOR_EMISSIVITY=<0..1>` on the production commands.
  - It is refused on any path other than the canonical source authority.
  - The seal hashes the floor inputs and arrays only when they are present.
- **Irradiance:** a direct sum over cells with nonzero net emission.
  - Most pairs use the face-centre point kernel.
  - Pairs within 2.5 cells horizontally and below 3Δz use the exact solid angle of the face. Without this, a first-layer emitter would send 1/π instead of 1/6 of its power through the face beneath it.
- **Surface balance:** bisection between T₁ and the radiative-equilibrium temperature.
- **Build fix.** `FireCase.cpp` is compiled with fast-math. Under LTO, edits to the executables linking it could perturb the case derivation's floating-point results and move canonical case IDs. That happened with the first build of this change: the tier-8 case went 67cc1791 → 1f798f77 with the floor off. `FireCase.o` is now built outside LTO with its historical flags, and the floor helpers are `noinline`. The production case ID is back to `67cc1791`.

### Verification

1. **Kernel and surface-balance fixtures** in `FireSimulationSolverTest`, all passing:
   - single far emitter against P·z/(4πr³) at 1e-12;
   - intercepted power against the square's analytic solid angle;
   - first-layer emitter: exactly 1/6 through the face beneath it, under 0.5P in total;
   - results identical across worker counts;
   - balance limits;
   - lower ε absorbs less;
   - refusals.

   The one failing check in that test, V6 Newton convergence, fails at HEAD as well.
2. **`FireCaseTest`:** identity pair, envelope round-trip and refusals all pass. The canonical source-pattern digest is unchanged.
3. **Floor off: bit-identical.** 25 tier-8 production steps from the new binary match the r264 binary (`2ca55c10`) byte for byte. That covers every `budgets/step_*.csv` apart from its build-ID and executable lines, and every non-timing column of the trajectory, including the state payload Merkle digests. The case ID is identical.
4. **Short tier-6 run with the floor on (ε = 1), from zero to 2.50 s.**
   - 1,523 steps, no retries; peak RSS 1.1 GB. I stopped it deliberately.
   - Data: [`data/fire_r270/tier6_eps1_floor_radiation.csv`](data/fire_r270/tier6_eps1_floor_radiation.csv), produced by [`run_tier6.sh`](data/fire_r270/run_tier6.sh). The run outputs stay in `/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/`.
   - Tier 6 writes no per-step budgets or fields. Energy closure holds by construction (the budget's source term is the sealed dose), and the floor-off run was checked bit for bit (step 3).
   - Floor heating and floor-cell gas temperature rise outside the burner:

| t (s) | Absorbed (W) | Convected (W) | Max T_w (K) | ΔT r 0.3–0.5 (K) | ΔT r 0.5–0.7 (K) | ΔT r 0.7–1.0 (K) |
|---|---:|---:|---:|---:|---:|---:|
| 0.5–1.0 | 1,688 | 460 | 625 | 2.5 | 1.0 | 0.3 |
| 1.0–1.5 | 1,764 | 507 | 596 | 4.4 | 2.0 | 0.9 |
| 1.5–2.0 | 1,513 | 398 | 594 | 5.6 | 2.9 | 1.3 |
| 2.0–2.5 | 1,542 | 532 | 558 | 6.1 | 3.6 | 1.7 |

   - **Convected heat** is about 0.5 kW, matching FDS r263's floor Q_COND of +0.51 kW.
   - **Floor-level air outside the burner now warms,** and the layer is still developing at 2.5 s. r264 had exactly 0 K here. FDS's statistically steady values are 9.8–15 K at 0.2–0.4 m, 4.6 K at 0.5–0.7 m, and 1.9–3.0 K at 0.7–1.0 m.
   - The 0.15–0.3 m band is not tabulated. It contains the pilot annulus, which is held at its setpoint until the pilot ends (about 2 s).
5. **Cost.** The irradiance sum on the tier-8 grid with every cell emitting (worst case) takes 0.35 s per step on 8 workers, about +6% on r264's 5.45 s/step. The real cost is lower, since only cells above ambient emit. An FFT convolution would bring it to milliseconds if needed.
6. **Independent reviews** of the physics (against the FDS 6.11 source) and the code found no P1 issues. Their P2 and P3 findings are fixed or recorded here: ε = 1, the near-field solid angle, budget routing, emission-spreading mismatch, binary32 re-representation of the dose, near-tie bracket tolerance, and logging absorbed rather than intercepted power.

## Next (owner approval needed)

**The tier-8 full window from zero with `RISE_FIRE_FLOOR_EMISSIVITY=1`:** about 23–25 h, the same command as r264. Compare against r263 on:
- the floor-cell ΔT(r);
- the r = 0.2–0.5 m annulus updraft;
- near-floor side inflow;
- flame height;
- the McCaffrey stations.
