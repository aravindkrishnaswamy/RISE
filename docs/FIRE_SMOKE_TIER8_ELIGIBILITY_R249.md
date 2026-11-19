# r249 — offline extinction and ignition-graph hypothesis

| Variant | Mean eligible CH4 (g) | Final eligible CH4 (g) |
|---|---:|---:|
| (a) Current gate | 0.036034497 | 0.036207613 |
| (b) Stoichiometric-parcel CFT; retain 600 K | 0.077114689 | 0.074006366 |
| (c) Parcel CFT; allow connected cold vertices | 0.577397442 | 0.589330667 |
| (d) Parcel CFT; no connectivity restriction | 0.577401615 | 0.589332821 |

Mean positive methane inventory is **0.582954983 g**. The means are trapezoidal
time averages of 156 retained r241 states from 10.543308698034 to
25.091040127256 s (99.47% of the statistics window), not newly simulated
states or measured consumption. Final positive inventory is approximately
0.5963 g. All masses and heights use binary32 solver geometry.

**Decision: select (c) for a design revision.** Variant (b) recovers only
**7.51%** of methane ineligible under (a). Variant (c) recovers **98.98%** of
that stranded inventory and makes **99.05%** of positive methane eligible.
Removing connectivity in (d) adds just **0.000004173 g** to mean eligibility.
This is strong evidence that the whole-cell CFT trial plus the 600 K graph
barrier strand the retained fuel. It is not evidence of ≥95% consumption in
an evolving flow. No mixing-time change is selected by this offline result.

## Height profile

Time-averaged eligible CH4 mass in each height band, in grams. Assign cells
by their center height. The [106-layer profile](data/fire_r249/eligibility_height_profile.csv)
retains the original solver height resolution.

| Height (m) | (a) Current | (b) Parcel | (c) Connected cold | (d) Unrestricted |
|---|---:|---:|---:|---:|
| 0–0.250 | 0.034743898 | 0.058556836 | 0.122511019 | 0.122511107 |
| 0.25–0.500 | 0.001273416 | 0.012295946 | 0.038425505 | 0.038425603 |
| 0.5–1.000 | 0.000017183 | 0.006228567 | 0.075774853 | 0.075775001 |
| 1–2.000 | 0.000000000 | 0.000033340 | 0.143904349 | 0.143905012 |
| 2–3.243 | 0.000000000 | 0.000000000 | 0.196781717 | 0.196784893 |

Current eligibility sits almost entirely below 0.5 m. Correcting the parcel
trial extends warm eligibility through the lower plume, but (b) still admits
almost no methane above 1 m and none above 2 m. Variant (c) admits the cold
methane throughout the plume, including the upper fuel that r249 measured
leaving through the top.

## What was tested

The read-only native probe loads each original checkpoint with
`LoadMethaneRunCheckpoint`. Variant (a) calls the unchanged
`BuildIgnitionEligibility` and preserves the existing post-pilot floor-holder
exception. Its start/middle/final totals reproduce the original r249 probe
after the explicit solver-volume conversion.

Variant (b) substitutes the parcel CFT predicate in the graph and the
floor-holder exception. It retains T > 600 K for graph vertices and the
existing pilot-mask / T > 810.4 K seed tests. Variant (c) floods face-connected,
parcel-passing cells of any temperature from the warm seeded components of
(b). It does not create cold seeds. The floor-holder exception is preserved
but does not seed this graph expansion. Variant (d) counts every
parcel-passing cell without a temperature or connectivity restriction.

The hypothetical parcel contains all CH4 and `phi` times all nonfuel species,
where `phi = min(1, s_O2*q_CH4/q_O2)`. Nitrogen and pre-existing products are
scaled together; retaining all product dilution would be a different test.
The native probe evaluates species sensible enthalpies before and after
complete stoichiometrically limited reaction at the existing 1700 K CFT,
using the current record's reaction vector and LHV. No constant heat capacity,
changed reaction rate, new ignition flag, or adjusted physical threshold is
introduced. Parcel arithmetic is normalized by fuel mass to avoid cancelling
almost the entire cell energy in very lean cells.

The FDS 6.11.1 Technical Reference Guide's `EXTINCTION 2` criterion excludes
excess air and proportional existing products, while retaining excess fuel
as a diluent. This verifies the proposed parcel interpretation. The hypothesis
should say **sufficiently lean** cell means fail the current whole-cell trial;
not every lean mixture fails. FDS supplies the extinction trial, not RISE's
600 K vertex waiver or its seeded graph.
[Guide: “Extinction Based on Both Fuel and Oxygen,” tagged source](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Combustion_Chapter.tex#L485-L501).

## Checks, limits and next action

- All 156 native loads completed. Signed inventories reproduce the r249
  retained-state table within 1e-15 kg; the separate positive/negative read
  reproduces each signed total to the same reduction precision. Every state's
  per-height sums reproduce its eligible totals. Eligible masses are ordered
  (a) ≤ (b) ≤ (c) ≤ (d).
- Native controls verify an empty seed set admits no graph component and an
  isolated remote cold pocket is excluded. A stoichiometric parcel and a lean
  parcel pass; an oxygen-starved rich parcel fails. The temporary native probe
  compiled and linked with an empty warning/error log.
- Accepted binary32 states contain small signed negative CH4 densities. Mean
  signed inventory is 0.582876579 g; mean negative inventory is −0.000078404 g.
  Eligibility counts positive fuel only, so recovery fractions use positive
  inventory. No accepted state or signed fuel-balance inventory was clamped.
- Tiny positive methane tails connect almost the entire plume to a seeded
  component, explaining why (c) nearly equals (d). A disconnected cold pocket
  stays excluded, but these measurements do not validate flame propagation
  speed or behavior of a distant pocket joined by a dilute cold bridge.
- The old flow was generated with (a). Enabling reaction in its retained
  fields does not predict the changed temperature, mixing, radiation or
  residence time after the source evolves. Actual burn remains limited by
  tau_mix, oxygen and existing source energy/expansion constraints.

The [specific design revision for (c)](FIRE_SMOKE_EXTINCTION_PARCEL_R249.md)
keeps seeds and connectivity and uses one shared parcel predicate at every
extinction site. Implementation and the tier-8 from-zero run remain the next
physics deliverable. This task's explicit offline/no-solver-change/no-new-run
scope is preserved. The eventual 33 kW acceptance remains consumption ≥95%
of statistics-window supply, near-zero top methane export and a closing fuel
balance; eligibility does not certify it.

Evidence:

- [156-state eligible masses](data/fire_r249/eligibility_mass_samples.csv)
- [Mean height profile](data/fire_r249/eligibility_height_profile.csv)
- [Definitions, numbers, source citation and SHA-256 manifest](data/fire_r249/eligibility_comparison.json)
- [Native probe source](../tools/fire/r249_eligibility_probe.cpp)
- Full per-state height profiles, the measured native source/binary, input
  list, build recipe/log and reducer remain in
  `/Volumes/SSD/RISE/fire_production_calibration/r249_tier8_fuel_balance/`.
  The repository probe differs from the measured source only in its portable
  relative include path. The original r249 artifacts and their hashes remain
  untouched.

The earlier report, data and status update were committed by explicit
pathspec in `767fbb2b7` before this offline test.

One review round used independent thermochemistry/FDS and graph/data reviewers.
Both found no material issues; the latter independently reproduced all mean
masses, layer sums, baseline checks, artifact hashes and original commit scope.
