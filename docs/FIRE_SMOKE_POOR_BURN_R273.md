# r273: poor-burn feedback for sooty flames (two opt-ins, evaluated offline and at low tier)

Last updated 2026-11-18. Follows [r272 soot](FIRE_SMOKE_SOOT_R272.md) §6 and the
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
- Not run: a tier-8 arm, single-opt-in tier-6 arms, any window longer than 0.6 s. The tier-6 numbers are start-up
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

## Files

- Solver: `src/Library/Utilities/FireProductionSourceKernel.h`. Tests: `tests/FireSimulationSolverTest.cpp`.
- Tools: `tools/fire/r273/` (`frozen_response.cpp`, `ab_driver_tier.cpp`, `ab_frames.cpp`, `ab_summary.py`,
  `build_tool.py`, `run_ab.sh`).
- Data: `r273_poor_burn/frozen_*.txt`, `t6_*` (frames, logs, `result.txt`), `t6_*.frames.txt`, `t6_ab_summary.txt`.
  `t6_y300_both_noframes_killed` is a first attempt stopped because it wrote no frames.

