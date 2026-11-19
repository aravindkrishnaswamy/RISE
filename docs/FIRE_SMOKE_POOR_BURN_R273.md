# r273: poor-burn feedback for sooty flames (two opt-ins, evaluated offline and at low tier)

Last updated 2026-11-19. Follows [r272 soot](FIRE_SMOKE_SOOT_R272.md) §6 and the
[formation–oxidation draft](FIRE_SMOKE_SOOT_FORMATION_OXIDATION_R252.md). Tools: `tools/fire/r273/`.
Data: `/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn/`.

## Result

- **Two opt-in closures are implemented and conserve exactly; unset, the solver is unchanged.** Soot radiation
  (`RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP`) and a sub-grid flame-sheet oxidation gate
  (`RISE_FIRE_SOOT_SHEET_OXIDATION`).
- **Soot radiation works as intended.** The radiative fraction comes out at 0.22, 0.26 and 0.40 for gross yields
  0.03, 0.1 and 0.3 on retained tier-8 states, with standard constants and no fitting. In a tier-6 run the
  yield-0.3 flame is about 230 K cooler at its peak than the yield-0.03 flame.
- **The sheet gate removes most of the escaping soot at every yield.** Tier-6, 0.6 s restarts: the unburned share
  at yield 0.3 falls from 62% (old closure) to 11%; at yield 0.03 it is 7%. For methane that is the right
  direction (NIST measures none).
- **The feedback is reproduced in direction but is weak.** With both on, the unburned share rises from 7% to 11%
  between yields 0.03 and 0.3. A real sooty fuel needs something like 35% or more at the top of that range.
  Net smoke yield at gross 0.3 would be about 0.03 (heptane-like), not the 0.14 the old closure gave.
- **Decision needed before a tier-8 run** (§4): the two opt-ins pull the sooty-fuel look in opposite directions.
  Soot radiation alone should keep the heavy smoke and add the cooling (expected, not yet run in the solver);
  adding the sheet gate fixes methane and thins the smoke of a sooty fuel.
- **Tier-8 run A (§5): soot radiation alone runs away.** The flame cools to a 1350 K peak, oxidation stops and
  nearly all the soot escapes. Too strong; a lower-cap run is in progress.
- **Why (§6):** not missing self-absorption and not the cap. The collapse starts at a radiative fraction of 0.31–0.36
  with soot optical depth 0.2–0.4. Soot emission at yield 0.3 is large enough to cool the reaction cells below the
  1300 K oxidation gate, soot then accumulates, and the gas-normalised budget pushes the escape factor above 1 (1.26 by
  0.3 s), so cooling no longer lowers the loss.
- **Third opt-in (§6), `RISE_FIRE_SOOT_SELF_ABSORPTION=1`:** soot radiates its own emission attenuated by its
  six-flux column, outside the gas budget; the gas escape factor is bounded by 1. Tier 6, yield 0.3: the flame holds
  at about 1450–1480 K with 1.1–1.6 L above 1300 K (soot radiation alone: no gas above 1300 K by 0.3 s), and the
  unburned share is 0.78 (old closure 0.62, soot radiation alone 0.94). A tier-8 run is in progress.
- Not run: single-opt-in tier-6 arms, any window longer than 0.6 s. The tier-6 numbers are start-up
  transients from a soot-free state and overstate the settled escaping share (old closure: 62% here, 46% settled at
  tier 8).

## 1. Physics: what a sooty flame does that the solver does not

Real flames, from cleanest to sootiest (values quoted from memory of Tewarson's SFPE Handbook tables; check before
citing outside this note):

| Fuel | Radiative fraction | Net smoke yield, kg/kg | Combustion efficiency |
|---|---:|---:|---:|
| Methane | 0.14–0.21 | about 0 | 0.99 |
| Propane | 0.27–0.29 | 0.024 | 0.95 |
| Heptane, kerosene | 0.31–0.35 | 0.037–0.042 | 0.92 |
| Toluene, benzene | about 0.40 | 0.18 | 0.68 |
| Polystyrene | about 0.40 | 0.16 | 0.69 |

The chain that links the columns:

1. **Soot is the main emitter in a luminous flame.** Gas bands (CO₂, H₂O) give methane its 0.15–0.2. Soot adds
   continuum emission in proportion to its volume fraction and roughly T⁵.
2. **More radiation means a cooler flame.** The heat radiated away is not available to keep the reaction zone hot.
3. **Soot oxidation stops below about 1300 K.** Kent and Wagner (1984, "Why do diffusion flames emit smoke?",
   Combust. Sci. Technol. 41) found smoke is emitted when the burnout region falls below about 1300 K, linked the
   temperature drop to soot radiation, and stopped the smoke by warming that region with a laser. (Abstract checked
   on the web 2026-11-18.)
4. **So the escaping share rises with sootiness.** Below the smoke point all soot burns out; above it the flame tip
   is too cold and soot breaks through. Markstein (1984) and Orloff, de Ris and Delichatsios (1992, Combust. Sci.
   Technol. 84) correlate the radiant fraction of buoyant turbulent flames with the fuel's laminar smoke point; the
   fraction saturates for very sooty fuels (Delichatsios, Orloff and Delichatsios). (Titles and the saturation
   statement checked on the web; the correlation's form was not retrieved.)
5. **Large fires reverse part of this** (cold smoke shields the flame and the radiative fraction falls with
   diameter above about 1–2 m). Not relevant at 0.3 m; not modelled.

Soot is oxidised by OH and O₂ at the flame sheet (Neoh, Howard and Sarofim 1981; Nagle and Strickland-Constable
1962; from memory). Both rates fall steeply with temperature, which is why a threshold near 1300 K works as a
first model.

What fire codes usually do: FDS prescribes both the net soot yield and the radiative fraction per fuel and has no
soot oxidation by default (from memory). RISE's r272 closure is more ambitious (gross formation, then burnout decided by the
flow), which is why it needs the feedback to be right.

### What the r272 solver is missing

| Step in the chain | r272 solver | Consequence |
|---|---|---|
| Soot emits | No. Radiation is the gas emission scaled so the total is 0.20 of the heat release | Radiative fraction is 0.20 at every yield |
| Radiation cools the soot-laden gas | Only through that fixed total | Temperatures do not respond to yield |
| Burnout needs a hot reaction zone | Gate is the cell-mean temperature above 1300 K | At 3 cm cells the sheet is sub-grid: only 3% of the soot inventory sits in cells whose mean is above 1300 K, so half the soot escapes at any yield |
| Formed carbon withholds heat | Yes (r272) | A small effect: at yield 0.3 the primary heat is 20% lower |

Two mechanisms matter most at this resolution:

- **(a) Soot radiation.** Without it nothing can cool a sooty flame. It supplies the dependence on yield.
- **(b) A sub-grid sheet temperature for the oxidation gate.** Without it burnout is decided by where soot happens
  to sit relative to cells that are hot on average, which is a resolution artefact and gives 45% escape for
  methane where the measurement is zero. It supplies the right low-yield limit, and gives (a) something to quench.

Not added: a formation law tied to rich mixture or temperature (the r272 misfit in the soot profile's shape), a
kinetic oxidation rate, spectral or optically thick soot radiation, and smoke shielding. Each needs its own
evidence.

## 2. Implementation (opt-in, unset = bit-identical)

Both are in `src/Library/Utilities/FireProductionSourceKernel.h`, read once per process like
`RISE_FIRE_METHANE_SOOT_YIELD`. No other production source is changed, no mirrored source is touched, and the
methane record is not edited.

### `RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=<c>` (0 < c ≤ 1; unset = off)

- Soot's optically thin emission is added to each cell's radiative exchange: `C·f_v·(T⁵ − T∞⁵)`, the law the
  solver test already validates against a wavelength integral (`EvaluateSyntheticHotCarbonExchange`), with fixed
  constants E(m) = 0.26 and 1800 kg/m³. That is a Planck-mean absorption of about 1300·f_v·T per metre, at the low
  end of the usual 1200–1900 range.
- The budget stays partitioned. The case radiative fraction χ (0.20) is what the gas alone radiates. With soot the
  budget becomes `min(max(c, χ), χ·(1 + S/G))`, where S and G are the volume integrals of soot and gas emission
  after the reaction step. Below the cap the escape factor is exactly the gas-only one, so soot simply emits on top
  of the gas with the same calibration. At the cap the escape factor falls.
- The cap is the bound from measured fuels. 0.45 covers the sootiest fuels in the table with some margin.
- Cooling goes where the emission is: sooty hot cells cool more than clean ones.
- Conservation: only the radiative sink changes. The per-cell backward-Euler solve keeps its certificate (the soot
  term's derivative is at least 5·C·f_v·T_lower⁴ ≥ 0), and the frozen-packet ledger closes with the larger sink.

### `RISE_FIRE_SOOT_SHEET_OXIDATION=1` (unset or 0 = off)

- In a cell that is burning fuel (ignition-eligible, fuel and oxygen present), soot oxidation is allowed if an
  estimate of the flame-sheet temperature reaches 1300 K, even when the cell mean is cooler. Cells already above
  1300 K, and cells not burning fuel, are unchanged.
- The estimate is the cell's stoichiometric parcel, the same construction as the existing critical-flame-temperature
  trial, burned with the heat that stays in the gas: `(1 − loss) × (LHV − yield × carbon heat)`. It includes the
  carbon the yield holds back.
- `loss` is the case radiative fraction. With soot radiation on it is raised by the cell's own soot-to-gas emission
  ratio evaluated at 1300 K, up to the same cap. A sooty cell therefore has a cooler sheet: this is the quench.
- With loss 0.20 an ignition-eligible cell passes (its parcel reaches 1700 K adiabatically). The gate then
  reduces to "soot can burn wherever fuel is burning".
- Rate, oxygen sharing with the fuel and the packet arithmetic are the existing ones. Conservation is untouched.

Assumptions to review: the sheet loses the flame-average radiative fraction (scaled by the local emission ratio);
all of a burning cell's soot is exposed to its sheet at the mixing rate; the lean/rich test still uses the full
fuel stoichiometry (as r272).

### Tests

`FireSimulationSolverTest` gains ten checks ("r273 …"): parsing and off-when-unset; unset arguments reproduce the
default packets byte for byte; the soot emission equals the validated hot-carbon law; the applied fraction equals
χ(1 + S/G) from an independent evaluation, equals the cap when capped, and the realised sink matches it; chemistry
is unchanged by soot radiation; mass, elements, oxygen and energy close in every arm; the sheet gate oxidises only
in an eligible burning cell, never at loss 1, never with oxidation disabled, and leaves hot cells byte-identical;
the sheet estimate is monotone in loss and yield; malformed settings fail closed.

Results, 2026-11-18, variables unset, `RISE_TEST_EXTENDED=0`:

| Test | Result |
|---|---|
| FireSimulationSolverTest | passed (including the ten new checks) |
| FireProductionSolverTest | passed |
| FireProductionCalibrationTest | passed (no mirrored source edited; generators not re-run) |
| FireSequenceTest | passed |
| FireSimulationRecordsTest | passed |
| FireCaseTest | 0 failures |

With both variables set, `FireSimulationSolverTest` fails one check, "V5 beta-active accepted radiation stays
within two percent…", which pins the fixed-fraction budget (as the yield opt-in fails the checks that pin zero
yield). A malformed value fails every source build with a named error.

The capstone check "statistics-window radiative fraction matches the methane default" (0.20 ± 0.02, artifact runs
only) would need to follow the applied fraction if soot radiation were ever made a default.

## 3. Evaluation

### 3.1 Frozen-field response on retained tier-8 states (seconds per state)

`tools/fire/r273/frozen_response.cpp` loads a retained checkpoint, rebuilds mixing times and ignition eligibility as
`cft_check.cpp` does, and runs one production source step (`BuildFrozenMethaneSourcePackets`) over the whole grid
under each arm. It shows what each opt-in does to radiation and oxidation on a real flame state before the flow
has had time to respond. Three states of `r272_soot/tier8_y030` (21.9, 24.2, 25.0 s) at yield 0.03; for higher
yields the last state with its soot multiplied by 3.33 and 10 (the added carbon taken out of CO₂). r272 found soot
close to a passive tracer, so this is a fair stand-in for the soot field; the temperature field is the 0.03 one.

Self-check: with both opt-ins off, oxidised/formed is 0.54–0.59 on the three states, an escaping share of 41–46%.
The full run measured 45%.

| Yield (state) | Arm | Radiative fraction | Radiative cooling of soot-bearing gas above 1300 K, K/s | Soot oxidised / formed, this step | Cells oxidising |
|---|---|---:|---:|---:|---:|
| 0.03 | off | 0.200 | 2,700 | 0.58 | 188 |
| 0.03 | soot radiation | 0.218 | 3,030 | 0.58 | 188 |
| 0.03 | sheet gate | 0.200 | 2,810 | 2.58 | 1,418 |
| 0.03 | both | 0.218 | 3,150 | 2.58 | 1,418 |
| 0.1 (×3.33) | off | 0.200 | 2,700 | 0.57 | 188 |
| 0.1 (×3.33) | soot radiation | 0.262 | 3,820 | 0.57 | 188 |
| 0.1 (×3.33) | both | 0.262 | 4,310 | 2.54 | 1,418 |
| 0.3 (×10) | off | 0.200 | 2,650 | 0.55 | 188 |
| 0.3 (×10) | soot radiation | 0.403 | 6,190 | 0.55 | 188 |
| 0.3 (×10) | sheet gate | 0.200 | 3,700 | 2.43 | 1,418 |
| 0.3 (×10) | both | 0.401 | 8,570 | 2.43 | 1,389 |

(Cooling rates use the certified lower bound of the heat capacity, so they are upper estimates; compare them
between arms, not as absolutes. The 0.03 rows are the 25.0 s state; the other two states give 0.217–0.218 and
2.6–3.2.)

What this shows:

- **Soot radiation gives a radiative fraction that rises with yield without tuning:** 0.20, 0.22, 0.26, 0.40 at
  gross yields 0, 0.03, 0.1, 0.3. The cap of 0.45 is not reached. Measured fuels run 0.14–0.21 (methane), about
  0.3 (heptane) and about 0.4 (toluene, polystyrene). The constants are the standard soot emission law, not a fit.
- **It more than doubles the cooling of the soot-laden hot gas at yield 0.3** (6,200 against 2,650 K/s) and adds
  12% at 0.03. This is the feedback the r272 solver lacks.
- **Within one step it changes no oxidation** (same 188 cells): the effect has to build up as temperature falls
  over the residence time. A frozen step cannot show that; §3.3 does.
- **The sheet gate is a large change at every yield.** The number of oxidising cells goes from 188 to about 1,400
  and the instantaneous oxidation rate rises 4.4 times, to 2.4–3.2 times the formation rate (the inventory would be
  burned down until a new balance is reached). 18% of the soot inventory sits in burning cells against 3% in cells
  whose mean is above 1300 K. Burnout is spread up the flame (a third of it between 0.5 and 1 m) instead of
  sitting at the base.
- **The sheet quench barely engages.** At yield 0.3 with the cap at 0.45, 29 of the 1,230 cells the sheet estimate
  admits (2%) fail the derated estimate. A parcel that passes the 1700 K ignition test still reaches 1300 K after losing 45% of its
  heat unless it starts cold and dilute. So the sheet gate by itself lowers the escaping share at all yields and
  does not make it depend on yield; any dependence has to come from soot radiation cooling the flow.

### 3.2 Closed product parcel (same tool)

The mean soot-bearing cell above 1300 K (1511 K, 0.052 ppm of soot at yield 0.03, mixing time 0.038 s), closed,
no fuel, cooled with each arm's escape factor for 0.4 s.

| Yield | Arm | Time to fall to 1300 K, s | Soot left unburned at 0.4 s |
|---|---|---:|---:|
| 0.03 | off | 0.115 | 4.7% |
| 0.03 | soot radiation | 0.112 | 5.1% |
| 0.1 | off | 0.123 | 3.8% |
| 0.1 | soot radiation | 0.112 | 5.1% |
| 0.3 | off | 0.147 | 2.1% |
| 0.3 | soot radiation | 0.112 | 5.1% |

- **Without soot radiation the feedback has the wrong sign:** a sootier parcel stays hot longer, because burning
  soot releases heat and nothing takes it away, and it burns out more completely.
- **Soot radiation removes that** (the time above 1300 K no longer grows with yield) but in this parcel it does not
  go further: the unburned share is 5% at every yield. The parcel spends three mixing times above 1300 K either
  way, so most of its soot burns.
- The parcel is not the flame. In the flame 97% of the soot is in cells below 1300 K. The parcel result says the
  closure responds in the right direction and that the response is modest.

### 3.3 Tier-6 A/B in the full solver (three 0.6 s restarts, 27–30 min each)

`tools/fire/r273/run_ab.sh`: resume the retained r270 tier-6 state (step 1466, 2.414 s, dx 0.041 m, floor on,
default stencil, no soot) for 0.6 s. Numerics are left as in that state so only the soot settings differ. Every
step was accepted in all three arms, including both opt-ins together, so the frozen-packet ledger closes in the
production path. Frames at 16 Hz from 2.5 s; statistics are the mean of the nine frames 2.5–3.0 s
(`ab_frames.cpp`, `ab_summary.py`, `t6_ab_summary.txt`).

Nothing leaves the domain in 0.6 s, so the rise of the soot inventory is formed minus oxidised, and the fuel rate
follows from the heat release. "Unburned share" is that rise over the soot formed in the window.

| Arm | Yield | Heat release, kW | Fuel burned, g/s | Soot formed, mg/s | Inventory rise, mg/s | Unburned share | Peak T, K | Centre T at 0.1 m, K | Gas above 1300 K, litres |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| off (r272 closure) | 0.3 | 25.8 | 0.588 | 177 | 110 | 0.62 | 1724 | 1347 | 3.4 |
| both opt-ins, cap 0.45 | 0.3 | 25.7 | 0.524 | 157 | 16.6 | 0.106 | 1464 | 1203 | 2.0 |
| both opt-ins, cap 0.45 | 0.03 | 31.5 | 0.630 | 18.9 | 1.33 | 0.070 | 1697 | 1390 | 4.1 |

Reading it:

- **A sootier flame now runs cooler.** Between yields 0.03 and 0.3 with both on: peak 1697 → 1464 K, centre at
  0.1 m 1390 → 1203 K, and the volume above 1300 K halves. Against the old closure at the same yield the peak is
  260 K lower. r272 saw under 5% change. The radiative fraction was not recorded by this driver; the frozen-field
  value for this soot level is 0.40.
- **It burns less of its fuel.** 0.524 g/s against 0.630 g/s at yield 0.03 and 0.588 g/s with the old closure
  (17% and 11% less). The cause was not traced; the likely one is that cooler cells fail the ignition test. Real
  sooty fuels do have combustion efficiencies near 0.7, but this needs a longer window before it is believed.
- **The unburned share rises with yield, 7.0% to 10.6%.** That is the poor-burn feedback, and it is weak: a
  toluene-like fuel would need roughly 35% or more.
- **The level is set by the sheet gate.** 62% → 11% at yield 0.3. These are start-up values: the flame zone is still
  filling with soot, so all three overstate the settled share (the old closure settles at 46% at tier 8).
- **Limits of this A/B:** 0.6 s, one realisation per arm, puffing phase not averaged (the centre temperatures
  swing ±150 K frame to frame), tier 6, default stencil rather than the r271 candidate, and no arm with one opt-in
  at a time.

## 4. What to decide, and the next run

What the evidence supports:

- **Soot radiation: adopt as an opt-in for sooty-fuel work.** It supplies a missing piece of physics, its
  radiative fractions land on measured fuels without tuning, and it conserves. Its single setting is a cap taken
  from measured fuels, which was not reached at yield 0.3.
- **Sheet gate: keep as an opt-in, not yet for sooty fuels.** It corrects the low-yield limit, which is a real
  defect, but as built it burns out a sooty flame's soot almost as completely as a clean one's. Its quench term is
  too weak: a parcel that can ignite at 1700 K nearly always stays above 1300 K after a 45% loss.
- **The yield dependence is not yet strong enough to call the poor-burn regime reproduced.**

Options for the owner:

1. Sooty-fuel movies now: yield 0.3 with soot radiation only. Keeps the dark column, adds the cooler flame and
   whatever extra escape the cooling gives through the cell-mean gate. Not yet measured in the solver.
2. Methane fidelity now: yield 0.03 with both. Expected to bring escaping methane soot from 45% toward a few
   percent. The in-flame soot level will fall too, so the yield fit of r272 would have to be repeated.
3. One closure for both ends: needs a stronger, evidenced quench in the sheet gate. Candidates, none added here:
   judge the sheet at the temperature of the burnout zone (lean side, diluted) rather than the stoichiometric
   parcel; let the local loss exceed the flame-average cap in soot-rich cells; or replace the 1300 K step with a
   temperature-dependent oxidation rate.

Next tier-8 run (not started; each is a 2.5 s restart of `tier8_y030` at step 13,769, about 3 h, the same window
as r272 §6 so `sooty_y300` is the control):

- **Run A:** yield 0.3, `RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP=0.45`, sheet gate off.
- **Run B (only if both ends matter):** the same with `RISE_FIRE_SOOT_SHEET_OXIDATION=1`, and its yield-0.03 twin.
- Measure as r272 §6: soot formed, upward soot flux at 1 m (`soot_flux.cpp`), centreline temperature
  (`soot_col.cpp`), plus the radiated and chemical energy in the step budgets.
- **Confirms soot radiation:** realised radiative fraction 0.35–0.45; flame-zone centreline temperature at least
  100 K below `sooty_y300`; escaping share above its 46% by more than the frame-to-frame spread; every step
  accepted.
- **Refutes it:** radiative fraction pinned at the cap from the first steps (the emission law is too strong at
  this resolution), or temperatures and escaping share within 5% of `sooty_y300` (the cooling does not reach the
  cells that decide oxidation).
- **Confirms the pair (run B):** escaping share at 0.03 below 15% and at 0.3 at least twice that.
  **Refutes:** the two shares within a few points of each other, as at tier 6.

## 5. Tier-8 run A: soot radiation alone (2026-11-18)

`t8_y300_rad045` (`tools/fire/r273/run_tier8_a.sh`): resume of `tier8_y030` at step 13,769 for 2.5 s, yield 0.3,
cap 0.45, sheet gate off. 1,520 steps, every one accepted, 4.3 h (the GPU was shared). Control: r272 `sooty_y300`,
same start, same frames. Means of 23.97–24.90 s (16 frames) unless noted.

| | `sooty_y300` (control) | Run A |
|---|---:|---:|
| Heat release, kW | 26.4 | 26.6 |
| Peak temperature, K (settled frames) | 1720–1810 | 1340–1410 |
| Centreline temperature at 0.14 m, K | 1572 | 1021 |
| Gas above 1300 K, litres | about 4.4 | about 0.1 |
| Soot share in cells above 1300 K, at the burner | 0.29 | 0.04 |
| Upward soot flux at 1 m, mg/s | 91 | 198 |
| Centreline soot volume fraction in the flame, ppm | 0.42 | 1.9–2.0 |

- **The feedback runs away.** Within 0.25 s the peak temperature falls from 1700 K to about 1350 K and the volume
  above 1300 K falls forty-fold. Soot oxidation then all but stops and nearly everything formed escapes: about
  198 mg/s at 1 m against about 197 mg/s formed at the control's fuel rate (the control lets 46% escape).
- **Fuel still burns.** Heat release is unchanged, so the cold flame is not an extinction; it is the reaction heat
  being radiated from the soot-laden cells.
- **Against the run's criteria:** temperature drop and escaping share both confirm that the cooling reaches the
  cells that decide oxidation. The realised radiative fraction was not recorded by the driver; with five times the
  frozen-field soot level it is very likely pinned at the cap, which was the refuting sign for the emission law.
- **Why it is probably too strong.** The emission law is optically thin. At 2 ppm and 1000 K the absorption
  coefficient is about 2.6 per metre, an optical depth near 0.8 across the 0.3 m flame, so self-absorption is no
  longer negligible and the cap is the only thing limiting the loss. Escape near 100% and a 1350 K peak are more
  extreme than a toluene-like fuel (net yield about 0.18 of a larger gross yield; from memory).
- **Look.** Still at 24.84 s, the r272 sooty scene: a short dull-red flame under a column about twice as dark as
  the control (`r273_poor_burn/rad045_seq`, comparison sheet `movies/r273_soot_radiation_compare.png`).
- **Next, started:** `t8_y300_rad030`, the same run with the cap at 0.30, to see whether there is a stable
  intermediate state or the flame falls to the cold branch at any cap above the gas value.
- Data: `docs/data/fire_r273/t8_y300_rad045_{budget,centreline}_23.96_24.90s.txt`.

## 6. Why run A collapses, and a self-absorption closure (2026-11-19)

### 6.1 Diagnosis

Tools: `radiation_census.cpp` (one frozen production source step on a checkpoint; per-cell census of reaction heat
against realised radiative loss, the budget arithmetic, soot optical depth, a six-flux column escape) and
`frame_tau.cpp` (soot optical depth from frames, which carry carbon and temperature only). Run A kept no checkpoints,
so `run_snap.sh` replayed it from step 13,769 with a checkpoint every 30 steps (0.049 s) up to 0.30 s
(`t8_snap_rad045`, stopped there). The replay is the same build and settings as run A. Table:
`docs/data/fire_r273/r273_collapse_census.txt`.

| State | t after start, s | Applied fraction | Escape factor β | Gas above 1300 K, L | Hot cells: share of loss / of heat | Hot cells: loss / heat | Soot oxidised / formed | Largest soot chord τ | Peak T, K |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| y300, no soot radiation (25 s run, 5.70 s) | – | 0.418 | 0.74 | 3.9 | 0.66 / 0.42 | 0.64 | 0.54 | 0.32 | 1744 |
| replay s30 | 0.05 | 0.307 | 0.78 | 4.2 | 0.64 / 0.44 | 0.44 | 0.31 | 0.19 | 1635 |
| replay s90 | 0.15 | 0.359 | 0.96 | 2.4 | 0.45 / 0.31 | 0.51 | 0.32 | 0.32 | 1505 |
| replay s150 | 0.25 | 0.416 | 1.18 | 0.8 | 0.22 / 0.16 | 0.56 | 0.18 | 0.38 | 1450 |
| replay s180 | 0.30 | 0.437 | 1.26 | 0.7 | 0.21 / 0.16 | 0.57 | 0.17 | 0.41 | 1460 |

(Gas alone, χ 0.20, on the same states: hot-cell loss/heat 0.24–0.30, and 2/3 of the loss in cells above 1300 K.)

- **Hypothesis 1 (missing self-absorption, only the cap limiting): not the cause.** The collapse happens while the
  fraction is 0.31–0.36, well under the cap; the cap is approached only at 0.3 s, after the volume above 1300 K has
  fallen fivefold. Soot optical depth across the flame stays at 0.2–0.4 during the collapse (column escape 0.82–0.92).
  In run A's settled frames (23.97–24.90 s) it is 0.30–0.48 across the axis at 0.05–0.3 m, peak κ about 3 per metre,
  column escape of the axis cell 0.70 (the vertical column dominates); the control is 0.11–0.24 and 0.86. The
  self-absorption the note estimated (τ ≈ 0.8) is about twice what the frames give, and it is a consequence of the
  collapse, not its trigger.
- **Hypothesis 2 (the partition dumps the capped loss on the few hottest cells): not as stated.** The distribution of
  the loss over temperature is close to the gas-only one (hot cells take 64–72% of the loss with soot, 67% without).
  Hot cells lose 0.44–0.64 of their own heat in aggregate, against 0.24–0.30 with gas alone; 8–33% of them
  individually lose more than they release (none once collapsed). The hot cells lose more because the total roughly doubles, not because
  the share moves.
- **What the numbers do show:**
  1. *The onset is the soot emission itself.* Soot formed in a burning cell sits in that cell at its temperature and
     emits as f_v T⁵. At yield 0.3 a hot, sooty flame emits 0.42–0.45 of its heat release (frozen, y300 state);
     hot cells then lose about half their heat instead of a quarter, which by itself is enough to pull their mean
     down by a few hundred kelvin.
  2. *The amplifier is the cell-mean oxidation gate.* Oxidised/formed falls from 0.54 (hot sooty state) to 0.17 as
     cells drop below 1300 K, so soot accumulates (peak f_v 0.7 → 1.5 ppm by 0.25 s, about 2 ppm on the centreline when settled).
  3. *The budget arithmetic removes the temperature feedback.* The escape factor is χQ/G; as the flame cools the gas
     emission G falls and β rises: 0.78 → 0.96 → 1.18 → 1.26. From 0.2 s on, every cell emits more than the
     optically thin limit at its temperature, so cooling no longer reduces the loss: it stays at a fixed fraction of the
     heat release and the cold state is self-sustaining. An escape factor above 1 is not physical.

### 6.2 Closure: `RISE_FIRE_SOOT_SELF_ABSORPTION=1` (needs the cap; unset = off)

Soot leaves the gas budget and radiates its own emission:

- Gas: unchanged calibration (β = χQ/G with χ = 0.20), but β is bounded by 1, since it is an escape probability.
- Soot: each cell loses P·C·f_v·(T⁵ − T∞⁵), the same thin law, with P its six-flux escape probability: the mean over
  ±x, ±y, ±z of the slab-mean transmission out of the cell, (1 − e^(−κΔx))/(κΔx), times e^(−τ) of the soot column
  beyond it to the domain edge, with the Planck-mean κ = 3.83·6π·E(m)·f_v·T/C₂ of the same law (3.83 = 15·24ζ(5)/π⁴,
  checked). No length scale is chosen: the column is the solver's own soot field.
- The cap now only scales soot down, never up: if gas plus escaping soot exceed cap × Q the soot term is scaled to fit;
  otherwise the loss is whatever the physics gives. With no heat release the soot scale follows the gas's quiescent
  factor γ.
- Why this and not the alternatives: (a) a flame-scale mean beam length (3.6 V/A) is ill-defined here (the region
  above 1000 K is a wrinkled shell with 3.6 V/A ≈ 0.06–0.07 m) and would need a per-scene length; the column uses the
  field the solver has. (b) A per-cell loss bound (loss ≤ local heat) has no physical basis (a cell may radiate heat
  convected into it). A temperature floor or soot eraser was ruled out. The two defects the evidence found are
  physical-law violations (β > 1, and soot emission scaled by a gas calibration), and this closure removes both; the
  column factor is the self-absorption the owner asked about, kept because it costs one pass over the grid and is
  measurable (0.7 in run A's settled state).
- What it does not change: the onset. At the start the gas escape factor is 0.74–0.82 and the column escape 0.84–0.92,
  so soot emits about the same either way (frozen fraction 0.447 against 0.418 on the y300 state, 0.327 against 0.307
  at s30). It acts once the flame cools: on the s150/s180 states the fraction is 0.32/0.31 against 0.42/0.44, and the
  hot cells lose 0.40–0.42 of their heat instead of 0.56–0.57. At yield 0.03 it gives 0.224 against 0.219.
- Conservation: only the radiative sink changes. The backward-Euler map takes separate gas and soot weights (folded
  into the exchange with the solver's factor set to 1); its derivative enclosure becomes β·g′_lower + w·5C·T_lo⁴ and
  the existing positivity test on C_T + Δt·F′ is unchanged. The frozen-packet ledger is checked per cell in the kernel
  as before, and every tier-6 step was accepted.
- Production call (`FireProductionSource.cpp`) passes the grid dimensions; a call without them, a malformed value, or
  the variable without the cap fails with a named error.

### 6.3 Tests

`FireSimulationSolverTest` gains six checks ("r273 self-absorption …", "r273 six-flux …", "r273 separate soot
weight …"): parsing and off when unset; the column escape is 1 in clear gas, the slab transmission behind a slab and
the slab mean inside it (closed forms); the separate soot weight with no gas weight gives the weighted soot law,
bracketed by its end values; on a 2-cell grid the applied fraction equals an independent oracle (gas at χ with β ≤ 1
plus column-escaping soot), the gas escape factor is the gas-only one, the cap scales only soot, and the realised sink
matches within 2%; chemistry unchanged and the ledger closes; fails closed without the cap, on a malformed setting,
without grid dimensions or with non-uniform cells. Unset, the existing soot-radiation arm is byte-identical
(checked against the explicit-0 call).

Results, 2026-11-19, variables unset, `RISE_TEST_EXTENDED=0`: FireSimulationSolverTest, FireProductionCalibrationTest
(no mirrored source touched; it prints an informational `UNBOUND_R186_OWNER` line for the kernel header and passes),
FireSequenceTest, FireSimulationRecordsTest and FireCaseTest pass. FireProductionSolverTest (3) and
FireProductionProjectionTest (1) fail only their Metal timing checks (tier-10 ms allocations) while the GPU is shared.

### 6.4 Tier-6 A/B (0.6 s restarts, `run_ab.sh`, same state and frames as §3.3)

Soot radiation alone had never been run at tier 6; it is the collapse control. Means of the nine frames 2.5–3.0 s
(`r273_poor_burn/t6_ab_selfabs_summary.txt`, `docs/data/fire_r273/r273_t6_selfabs_summary.txt`); the radiative
fraction is not recorded by this driver.

| Arm | Yield | Heat release, kW | Unburned share | Peak T, K (last 5 frames) | Centre T at 0.1 m, K | Gas above 1300 K, L (mean; last 5 frames) |
|---|---:|---:|---:|---:|---:|---:|
| off (r272 closure) | 0.3 | 25.8 | 0.62 | 1724 (mean) | 1347 | 3.4 |
| both (cap 0.45 + sheet gate) | 0.3 | 25.7 | 0.11 | 1464 (mean) | 1203 | 2.0 |
| soot radiation alone, cap 0.45 | 0.3 | 25.2 | 0.94 | 1188–1253 | 1027 | 0.42; 0 |
| soot radiation + self-absorption | 0.3 | 25.6 | 0.78 | 1447–1477 | 1162 | 1.52; 1.1–1.6 |
| soot radiation alone, cap 0.45 | 0.03 | 31.4 | 0.55 | 1658–1723 | 1382 | 3.99 |
| soot radiation + self-absorption | 0.03 | 31.4 | 0.55 | 1652–1714 | 1378 | 3.94 |
| both (§3.3) | 0.03 | 31.5 | 0.07 | 1697 (mean) | 1390 | 4.1 |

- **Soot radiation alone reproduces the tier-8 collapse at tier 6.** The volume above 1300 K goes 2.2 → 0.07 L in
  0.2 s and to zero after, the peak settles at 1190–1250 K, and 94% of the formed soot stays unburned.
- **With self-absorption the flame stops falling.** The peak drops from 1560 to about 1450 K in the first 0.2 s and
  then holds at 1450–1480 K; the volume above 1300 K settles at 1.1–1.6 L (old closure 3.4 L, soot radiation alone
  0); centre temperature at 0.1 m 1060–1180 K. Soot oxidation continues: 22% of the formed soot burns in the window
  (6% with soot radiation alone, 38% with the old closure).
- **At yield 0.03 it stays close to soot radiation alone** (unburned share 0.548 against 0.546, peak and hot volume
  within 1–2%); the column escape there is 0.98 and the gas escape factor stays below 1.
- These are 0.6 s start-up windows from a soot-free state (§3.3); they rank the arms, they do not give settled shares.

### 6.5 Tier-8 run (launched, not finished)

`t8_y300_rad045_sa` (`run_tier8_a.sh t8_y300_rad045_sa 0.45 0 1`, driver `build_r273c/ab_driver_tier`): run A with
self-absorption on; same start (step 13,769) and frame origin, so `sooty_y300` and `t8_y300_rad045` are controls. A
full checkpoint every 190 steps goes to `t8_y300_rad045_sa.snap/` (`RISE_R273_SNAPSHOT_EVERY`), so the realised
radiative fraction can be measured with `radiation_census`. Launched 2026-11-19 02:33 PST. Its first steps took
about 20 s each with three other GPU jobs running (run A averaged 10 s); expected end roughly 08:00–11:00.
Provenance in `t8_y300_rad045_sa.commit`. Its first three frames (to 22.59 s) match run A's within 1–4% (peak 1703,
1576, 1515 K against 1704, 1592, 1516 K), as the frozen analysis predicts for the onset. Frames 3–5 (to 22.78 s)
then diverge: peak 1485–1497 K against 1419–1448 K, gas above 1300 K 1.9/1.5/1.2 L against 1.5/0.7/0.7 L. Its first
snapshot (step 190, 22.78 s, `diag/census2_t8sa_step190.txt`): applied fraction 0.364, not at the cap; gas escape
factor at its bound of 1; soot oxidised 50 mg/s against 29 mg/s in the run-A replay at 22.76 s; largest soot chord τ
0.35. Early, single snapshot.

To judge it (as §4): peak temperature 1500 K or more with a persistent volume above 1300 K; realised fraction
0.3–0.45 and not at the cap; soot oxidation continuing; escaping share between `sooty_y300`'s 46% and run A's ~100%.

### 6.6 Not verified

- The tier-6 arm did not reach the 1500 K peak the owner asked for (1450–1480 K); whether tier 8 does is open.
- The radiative fraction in the solver runs is inferred from frozen steps, not measured in the dynamic runs (the tier-8
  snapshots will give it).
- The onset is unchanged: soot formed in a burning cell emits at that cell's temperature, so the first 0.1–0.2 s still
  cool the reaction cells. Whether a real yield-0.3 flame's cell-mean reaction zone at 3 cm should sit near 1450 K or
  1700 K was not checked against data. Klassen and Gore's 30 cm toluene and heptane pool-fire measurements (NIST
  reports, found on the web 2026-11-19; values not retrieved) are the dataset to check against.
- The six-flux escape uses only the soot column (not the gas bands), drops the absorbed energy instead of depositing it
  in the absorbing cells (so cool soot in the plume is not heated by flame radiation), and treats the domain edges as
  black and cold. The Planck mean is used for absorption of radiation emitted at a different temperature.
- The gas escape factor bound (β ≤ 1) changes the gas calibration once a sooty flame has cooled; with the opt-in the
  gas fraction can drop below 0.20.
- The dynamic gain was not split between its three parts. Frozen, on s180: gas-normalised thin 0.437 → soot on its
  own emission 0.389 → with the column 0.354 → with the gas factor bounded 0.313.
- Single realisations, 0.6 s windows, one resolution at tier 6.

## 7. Tier-8 cap 0.30, no self-absorption (2026-11-19)

`t8_y300_rad030`: run A with the cap at 0.30 (`run_tier8_a.sh t8_y300_rad030 0.30`). 1,520 steps, all accepted.
Mean of 23.97–24.90 s (16 frames), same frames as the control and run A.

| | Control (`sooty_y300`) | Cap 0.30 | Cap 0.45 (run A) |
|---|---:|---:|---:|
| Heat release, kW | 26.4 | 26.3 | 26.6 |
| Peak temperature, K | 1720–1810 | 1514–1608 | 1340–1410 |
| Centreline temperature at 0.14 m, K | 1572 | 1315 | 1021 |
| Gas above 1300 K, litres | about 4.4 | 2.5 | about 0.1 |
| Upward soot flux at 1 m, mg/s | 91 | 116 | 198 |
| Escaping share (flux over 197 mg/s formed) | 46% | about 59% | about 100% |
| Centreline soot volume fraction in the flame, ppm | 0.42 | 0.66–0.67 | 1.9–2.0 |

- **A stable intermediate state exists.** The flame stays hot, soot keeps oxidising, and the soot inventory levels
  off (76, 130, 183, 226, 223 mg at 0.5 s intervals). The poor-burn feedback is present at a believable strength:
  about 200 K cooler and 13 points more soot escaping than the control.
- The realised radiative fraction was not recorded; whether it sits at the 0.30 cap is not known. If it does, the
  cap is acting as the model, which is what §6's opt-in is meant to replace.
- Formed soot is taken as 197 mg/s (the control's value); heat release is equal in all three runs.
- Look: flame shorter and redder than the control, column darker (`rad030_seq`, sheet
  `movies/r273_soot_radiation_compare4.png`: yield 0.03, control, cap 0.30, cap 0.45).
- Data: `docs/data/fire_r273/t8_y300_rad030_{budget,centreline}_23.96_24.90s.txt`.

## 8. Tier-8 with the self-absorption closure, cap 0.45 (2026-11-19)

`t8_y300_rad045_sa`: run A plus `RISE_FIRE_SOOT_SELF_ABSORPTION=1`. 1,520 steps, all accepted. Same 16 frames.

| | Control | Cap 0.30 (§7) | Cap 0.45 + closure | Cap 0.45 (run A) |
|---|---:|---:|---:|---:|
| Heat release, kW | 26.4 | 26.3 | 26.2 | 26.6 |
| Peak temperature, K | 1720–1810 | 1514–1608 | 1485–1539 | 1340–1410 |
| Centreline temperature at 0.14 m, K | 1572 | 1315 | 1261 | 1021 |
| Gas above 1300 K, litres | about 4.4 | 2.5 | 1.7 | about 0.1 |
| Upward soot flux at 1 m, mg/s | 91 | 116 | 136 | 198 |
| Escaping share (over 197 mg/s formed) | 46% | about 59% | about 69% | about 100% |
| Centreline soot volume fraction, ppm | 0.42 | 0.66–0.67 | 0.86 | 1.9–2.0 |

- **The flame stays lit at the cap that collapsed run A.** Peak holds near 1500 K to the end, the hot volume is
  steady (1.2–1.7 L from 1.0 s on) and the soot inventory levels off (81, 143, 206, 258, 256 mg at 0.5 s intervals).
- **The radiative fraction is not set by the cap.** Frozen census on the run's own checkpoints at 1.56 s and 2.5 s:
  the closure radiates 9.6–9.7 kW where the thin law would radiate 14.4–14.7 kW (at its 0.44 budget). Against the
  26 kW the run releases that is about 0.37. (The census step itself ran at zero yield and reports 0.28–0.29 of its
  own 33–34 kW.) The gas escape factor would have been 1.33–1.37 without the bound; the soot column escape is 0.79.
- Peak temperature is at the 1500 K target, not clearly above it; escaping share 69% is above the control and
  well below 100%.
- Look: close to the cap-0.30 run, a short orange-red flame under a dark column
  (`rad045sa_seq`, sheet `movies/r273_self_absorption_compare.png`: control, cap 0.30, closure, run A).
- **Started:** `tier8_y300_sa`, the 25 s window from zero with these settings (`run_tier8_showcase.sh`), for the
  showcase movie. The capstone's closing 0.20 ± 0.02 radiative-fraction check is expected to fail; the frames are
  the product.
- Data: `docs/data/fire_r273/t8_y300_rad045_sa_{budget,centreline}_23.96_24.90s.txt`.

## Files

- Solver: `src/Library/Utilities/FireProductionSourceKernel.h`; §6 also passes the grid dimensions from
  `src/Library/Utilities/FireProductionSource.cpp`. Tests: `tests/FireSimulationSolverTest.cpp`.
- Tools: `tools/fire/r273/` (`frozen_response.cpp`, `radiation_census.cpp`, `frame_tau.cpp`, `ab_driver_tier.cpp`,
  `ab_frames.cpp`, `ab_summary.py`, `build_tool.py` (optional output directory; ab tools gain the
  `RISE_R273_SNAPSHOT_EVERY`/`_DIR` checkpoint patch, inert when unset), `run_ab.sh`, `run_tier8_a.sh`, `run_snap.sh`).
- Data: `r273_poor_burn/frozen_*.txt`, `t6_*` (frames, logs, `result.txt`), `t6_*.frames.txt`, `t6_ab_summary.txt`.
  `t6_y300_both_noframes_killed` is a first attempt stopped because it wrote no frames. §6: `diag/` (census outputs,
  test logs), `t8_snap_rad045.snap/` (replay checkpoints), `t6_ab_selfabs_summary.txt`, `build_r273b/` and
  `build_r273c/` (tool builds kept apart from the running `build/ab_driver_tier`).
- Tables in git: `docs/data/fire_r273/r273_collapse_census.txt`, `docs/data/fire_r273/r273_t6_selfabs_summary.txt`.

