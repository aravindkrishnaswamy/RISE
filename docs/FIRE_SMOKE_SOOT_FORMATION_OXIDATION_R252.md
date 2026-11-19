# r252 — methane soot formation and oxidation revision (draft)

2026-10-19. **Proposed for owner review; no implementation and no simulation.**
This revises the methane interpretation in FIRE_SMOKE_DESIGN.md §3.4 and the
methane fuel record. The [source evidence](FIRE_SMOKE_METHANE_SOOT_DATA_R252.md)
separates in-flame soot inventory from escaping smoke. The operational record
currently enforces a gross yield of zero using evidence about net smoke;
that prevents the measured in-flame inventory from existing.

## Closure and conserved quantities

Start with the existing single transported carbon-soot inventory and the
existing primary-formation/oxidation source packet. Do not add a precursor
species or an acetylene mechanism that the solver cannot resolve. The first
model to fit is a single nonnegative methane gross formation coefficient
`y_form` (kg carbon / kg methane reacted):

    formed_carbon = y_form * accepted_reacted_methane

Use atom-balanced primary products that reserve that carbon from gaseous
products. The existing source packet adds `grossCarbonFormed` separately
after applying `PrimaryReactionDelta`; preserve that convention so formed
carbon is added exactly once, rather than also inserting it in that delta. Regenerate effective oxygen demand and primary reaction enthalpy
from the existing gas and aerosol thermochemistry. For the simple carbon/CO₂
split, the changes relative to complete combustion are:

    primary_O2_per_fuel = complete_O2_per_fuel - (32/12) * y_form
    primary_heat_per_fuel = methane_LHV - carbon_oxidation_heat * y_form

Actual implementation must use the record's molecular weights and formation
enthalpies, not these rounded explanatory ratios. Bound `y_form` by available
fuel carbon and require positive primary heat release. Both the CFT trial and
the source must use their consistent balanced chemistry after the record
changes. Do not keep full primary LHV while retaining unoxidized carbon.

Retain the existing transported-soot oxidation candidate:

    oxidation_candidate = min(carbon, oxygen / s_carbon)
                          * (1 - exp(-dt / tau_mix))

when temperature exceeds the existing record's oxidation threshold; otherwise
it is zero. Keep the common oxygen allocator shared with primary combustion:

    demand = s_primary * fuel_candidate + s_carbon * oxidation_candidate
    theta = min(1, available_oxygen / demand)     # theta=1 if demand=0

Both accepted extents use the same theta and the accepted packet's subsequent
extent limiter. Burnout makes CO₂ and releases the reserved carbon oxidation
heat. Formation and oxidation must conserve C/H/O, total mass, sensible plus
chemical energy and positivity. Existing §3.4 mixing-limited oxidation is a
model assumption, not a measured methane kinetic law. Keep it fixed for the
first formation fit so formation and oxidation are not simultaneously tuned
to one scalar observable.

Do not add a cold-cell soot eraser, force soot to zero above a chosen height,
set a desired ppm in the renderer, or multiply fuel burning merely to make a
yellow image. If the existing constant-gross-yield model cannot fit growth and
burnout together, report that identifiable model failure before adding a
richness/temperature formation function. A spatial formation function would
need its own evidence and review, not a hidden per-scene knob.

## Calibration to local in-flame data

The primary reference is NIST's **0.37 m, 34.5 kW methane** fire, not an escaping
yield. In a later authorized calibration run, reproduce that source geometry,
flow, ambient conditions and centerline sampling heights. Fit **only y_form**
to the published mean **Y_soot(z)** at 0.01–0.45 m, including the **0.0008 kg/kg
peak at 0.14 m** and the subsequent decay. Account for measurement averaging
and sampling; probe gravimetry is not an instantaneous pointwise optical
measurement. Use the available uncertainty and publication-rounding interval;
zero printed uncertainty must never give infinite fitting weight.

A concrete initial fit functional is weighted squared profile error in Y_soot,
with each residual scaled by the larger of the published uncertainty and
0.00005 kg/kg publication half-width. This half-width is a numerical weighting
choice based on reporting resolution, not a claimed experimental standard
deviation. Retain unweighted profile residuals too. No fitted numerical
coefficient is asserted here: it cannot be recovered from a local soot peak
without residence time, transported inventory and the fixed oxidation model.

Use 0.60 and 0.75 m zero-rounded measurements as burnout consistency checks,
with their finite reporting interval. Independently check the smaller Xin–Gore
flame against the direct volume-fraction profile (roughly 0.37 ppm mean at
H/D=2 and declining thereafter), recognizing its >93% commercial methane,
4.2 kW size and 30% measurement uncertainty. Its instantaneous 5 ppm map scale
is not an ensemble target. Do not silently rescale the smaller flame into
the larger one or fit its much higher instantaneous streaks to a coarse mean.

After that one fit, predict the existing **0.30 m/33 kW** case with unchanged
coefficients. Compare native exported soot mass and volume fractions, axial
peak location and burnout against the cited near-scale evidence, disclosing
geometry and averaging differences. Predict a second resolution unchanged;
if an apparent formation coefficient varies materially with resolution,
report the closure's applicability limit instead of freezing a universal
methane preset. No simulation is authorized by this draft itself.

## Escaping smoke and radiation

Let the oxidation equation, transported oxygen and temperature determine the
escaping smoke. Measure integrated carbon formation, carbon oxidation,
inventory change and boundary export with the existing accepted-stage ledger.
Near-zero escaping methane smoke is an independent outcome, not a zero gross
source. The NIST upper rounded local values do not define a numerical exhaust
yield limit. Preserve the prior net-smoke evidence as a separate constraint. The newly
verified NIST TN 2162r1 Table 4 also reports 34.5 kW methane soot below
detection in eight repeats; the evidence note preserves its footnote and unit
ambiguity. NIST calorimetry record methane_gas_burner22 separately lists
0.00044 kg/kg in the uncertainty column, which must not be treated as the
detection limit. Do not relabel net-smoke zero as a measured formation rate. If burnout fails while local formation fits, inspect the fixed oxidation
model before adding knobs or suppressing outflow soot.

**Keep χ_r=0.20, budget-partitioned.** Apply the existing radiation budget to
accepted gas plus soot oxidation heat release. Soot changes the emission
spectrum/spatial allocation and its balance with gas emission within that
budget; it does not add an independent Planck cooling term on top of 20%.
Soot heat reserved at formation is released on oxidation, so temporal heat
release can change even though complete burnout recovers the same fuel LHV.
Escaping carbon retains its chemical energy in the balance. Preserve the
accepted methane consumption, realized HRR and flame-height checks when
this model is eventually exercised; do not assert solver dynamics are
arithmetically unchanged simply because the radiative fraction stays fixed.

## Minimal implementation boundary after review

Replace methane's enforced-zero gross-yield record and its provenance with a
calibrated-model value and honest fit uncertainty; retain the existing optics
record and reference its density. Update the atom-balanced primary products,
effective oxygen and heat coefficients together with the methane record
validation that currently demands complete combustion and zero carbon.
Use the existing source formation, oxidation, allocator, carbon transport,
render channels and radiation budget. Add only focused conservation/burnout
regressions required by that change. No new qualification framework, gate
suite, generic fitting service or runtime calibration API is proposed.

Open issues for review are the lack of an exact pure-methane 33 kW f_v profile,
commercial-fuel composition in the LII reference, probe versus optical
averaging, whether the existing constant formation coefficient fits the whole
profile, and whether the existing oxidation temperature cutoff is adequate
for cooled escaping parcels. Those limits remain visible rather than being
folded into a soot-opacity adjustment. **Stop before implementation.**
