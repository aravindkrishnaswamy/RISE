# r249 — parcel extinction trial with one-cell flame-edge eligibility

Status: **variant (e) is implemented for the tier-8 from-zero test**.
The run acceptance measurements are pending.
The retained-state measurements are in
[FIRE_SMOKE_TIER8_ELIGIBILITY_R249.md](FIRE_SMOKE_TIER8_ELIGIBILITY_R249.md).
The one-cell follow-up supersedes the earlier choice of (c): (e) retains
57.72% of (c) eligible mass below 1 m, and 99.91% of (e) lies below 1 m.
Its mean eligible mass is 0.136754 g; (b) and (c) are 0.077115 and 0.577397 g.
[Offline edge measurements](FIRE_SMOKE_TIER8_EDGE_R249.md).
It does not claim the from-zero ≥95% consumption acceptance result.

## Reference and parcel definition

The FDS 6.11.1 Technical Reference Guide, “Extinction Based on Both Fuel and
Oxygen” (`EXTINCTION 2`), uses
`phi = min(1, s_air * Z_F / Z_A)`. It retains all fuel and `phi` times air and
pre-existing products in its enthalpy test. Thus excess fuel remains a diluent;
excess air and proportional products are excluded. The test suppresses
combustion when the resulting product enthalpy at CFT exceeds the reactant
enthalpy, including chemical energy.
[Tagged Guide source, labels `eq:dza` and `eq:extinction`](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Combustion_Chapter.tex#L485-L501).

For RISE's pure methane fuel and individually transported species, use
`phi = min(1, s_O2 * q_CH4 / q_O2)`, retain `q_CH4` and `phi*q_k` for every
nonfuel constituent. This includes nitrogen and existing products; it does
not discard all diluents. With `xi = min(q_CH4, q_O2/s_O2)`, compare

```
sum(q_parcel[k] * h_s[k](T)) + xi * LHV
    >= sum((q_parcel[k] + xi * reaction_delta[k]) * h_s[k](T_CFT)).
```

Equality passes. This is the primitive-species form of the cited parcel
criterion, using RISE's existing thermochemistry and atom-balanced reaction.

## Proposed eligibility semantics

1. Build warm vertices from accepted beginning-of-step state: positive CH4,
   positive O2, T > 600 K, and passing parcel CFT. Keep the existing pilot-mask
   or T > 810.4 K seed tests. Resolve the face-connected components that
   contain these seeds, exactly as before apart from the CFT trial.
2. Freeze that original warm vertex map. Add a cold vertex only when it has
   positive CH4 and O2, passes parcel CFT, and shares a face with an original
   warm parcel-passing vertex. Traverse this augmented graph from the unchanged
   warm seeds. A waived cold cell cannot grant another waiver; a cold chain
   therefore cannot connect the flame to an arbitrarily distant dilute pocket.
   No prior-step flags or new cold seeds are used.
3. Keep the established source-contact floor-holder exception. Replace its
   CFT predicate by the same parcel predicate. As in the measured offline
   variant, this exception enables those floor cells but does not create
   additional graph seeds for step 2.
4. Use the existing exponential mixing-time extent only after eligibility is
   decided. The parcel is hypothetical and defines an extinction decision;
   it is not an extra mass, heat, or inlet source. Apply actual consumption
   to the full cell, once, using its real available reactants, existing
   temperature/expansion limits, reaction products and radiation accounting.

Changing the extinction trial alone is (b); unrestricted cold connectivity
is (c), and removing connectivity entirely is (d). None is the chosen revision.
FDS supplies the parcel extinction criterion; extending RISE's cold vertices
is a separate proposed modeling decision supported by the offline comparison,
not a claim that FDS uses RISE's ignition graph.

A remote **disconnected** cold fuel pocket remains ineligible. Connectivity
can, however, bridge long distances through very dilute methane tails. The
near equality of (c) and (d) prompted the tighter one-cell restriction. The graph is
memoryless and independent of timestep, but that alone does not establish a
correct propagation model. An isolated-pocket control must stay excluded;
a connected cold chain must have its intended instantaneous-eligibility
meaning documented rather than acquire a one-cell-per-step front.

## Implementation scope and acceptance

The implementation uses one shared parcel predicate for the graph and the floor-holder trial.
The sites to keep consistent are `BuildIgnitionEligibility` and
`ProductionEstablishedFlameHolderEligible` in the capstone launcher, plus
`FireProductionSource.cpp`'s canonical source-contact exception. Audit other
callers of `TrialAdiabaticTemperatureK` before changing that helper globally:
its name promises a whole-cell adiabatic temperature, so a distinct parcel
predicate is clearer than silently changing unrelated thermodynamic queries.
All three extinction sites now use the same parcel predicate. The whole-cell
adiabatic-temperature query remains unchanged.

Evaluate the enthalpy inequality directly. In the dilute limit, form the
parcel normalized by fuel mass: `phi/q_CH4 = s_O2/q_O2` on the lean branch.
This avoids subtracting nearly the whole cell's sensible energy or dividing
a tiny difference by a tiny fuel mass. Retain the actual species enthalpies;
a constant-cp substitution or an epsilon-shifted CFT is unnecessary.

After focused chemistry/graph verification, the empirical
check is tier 8 **from zero**, unchanged 33 kW fuel input. Required outputs:
statistics-window consumption ≥95% of supplied fuel, top methane export near
zero, and the inlet/consumption/inventory/export balance closing. In addition,
most time-averaged heat must lie below 1.5 m,
with mean flame height near the 0.646 m Heskestad prediction. Use the published
FDS proxy: height integrating 99% of nominal 33 kW input through the averaged
plane heat profile; report it unavailable if that input fraction is not reached.
Report a realized-heat quantile separately, without using it to hide missing heat.
[Flame-height method, p. 7](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=907227).
Eligibility of retained fuel is not a surrogate for those measurements. If dynamic
consumption remains low despite the new eligibility, measure the tier-8
mixing-time distribution and its realized extent before tuning it. Tier 10
and McCaffrey follow the tier-8 fuel-balance result.
