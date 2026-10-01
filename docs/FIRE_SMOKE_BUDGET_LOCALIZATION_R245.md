# r245 — budget localization after the matched domain experiment

## Current: full budgets reviewed; top spatial exchange localization next

The full run completed exit 0 at **2026-10-01 05:00:32 UTC
(September 30, 10:00 PM PDT)**. Native output reports 15,315 accepted steps,
25.156864278920693 s and 235 frames, with zero hard-bound or physical-projection
retries. Independent frame/history and full-mode budget audits passed. All 15,315
accepted ledgers and 235 frame capsules were verified; this is byte/arithmetic
evidence, not empirical physical qualification.

Sequential full-window frame/history seal verification followed by the full-mode
budget audit launched under external `r245_full_verification.v1`. Its
`current_stage.json`, stage logs, stage completion receipts and final
`completion.json` distinguish active verification from a passing result.
The frame verifier retains the r244 checks with only run/case/receipt/root and
report paths adapted; its exact source and adaptation receipt are external.
No fixture mode is used for the budget audit. Budget interpretation and fresh
milestone reviews remain outstanding. Both stages completed exit 0 by 2026-10-01 05:22:16 UTC. Do not duplicate
them. Small reports are retained under `docs/data/fire_r245/`.

Fresh independent arithmetic/provenance and capture/lifecycle reviews of
`38be9615` returned zero P1/P2. Round 2 attempts failed with account usage-limit
errors and are not claimed as completed reviews; fresh round 3 supplied both
verdicts. The exact tested binary was frozen without rebuilding or relabeling
its embedded precommit/dirty build identity; the runtime manifest pins both
that identity and all reviewed source hashes.

The full unchanged doubled-domain budget run launched from zero at
**2026-09-30 03:24:04 UTC (September 29, 8:24 PM PDT)**. Initial process PID
48127 ended successfully; host memory admission remained below its ceiling.
No shortened window or checkpoint continuation is used. Target remains
25.156864278900095 s; discard 10.531864278900095 s, statistics 14.625 s and
235 original frame boundaries. Independent evidence verification and physical interpretation remain
outstanding. Host exclusivity now applies to the sequential verification job.

External roots: `r245_runtime.v1/FireSequenceTest` (SHA-256
`6f8ad4340d68b4bd657ba3603b970dfff2a14c3e901461305d340dc9447e9e0b`),
`r245_tier6_lateral_budget.v1`, `r245_tier6_lateral_budget.v1.log`, and
`r245_tier6_budget_launch.v1/{launch,pid,completion}.json`, all under
`/Volumes/SSD/RISE/fire_production_calibration/`. An absent completion receipt
means it has not been recorded as complete. Small launch hashes are in
`docs/data/fire_r245/full_budget_launch.json`.

After successful full-window completion, independently audit all frame/history
and budget seals, run `r245_budget_audit.py RUN/budgets --completion RECEIPT
--output REPORT` without fixture mode, then localize the signed budgets.
No boundary/solver revision, soot work or tier-10 run precedes that evidence.
Earlier sections below retain their chronological engineering status.

## Full-window signed budgets — descriptive localization

`tools/fire/r245_budget_localization.py` reads the audited final ledgers and
all 235 column publications, checks their seals, and retains source/provenance
hashes. `scalar_final_ledgers.csv` preserves every field in all three scopes;
`vertical_momentum_summary.csv` reports the mean/RMS/std of the four distinct
source-centred **vertical faces** at each height. Horizontal shared faces are
not summed. These are unweighted frame-sample statistics of instantaneous
accepted-step rates, **not** a time-integrated column momentum budget or an
antialias/stationarity certificate. Opposite columns can cancel in their mean;
RMS is calculated after the four-face average, not a mean of individual RMSs.

Across the unchanged 14.625 s statistics window, total-species mass enters
through the lateral sides by 8.211831 kg, through the top by 3.028479 kg and
through the fuel footprint by 0.009647 kg. Lateral outflow is 0.007858 kg;
top outflow is 11.231603 kg. Thus the top supplies **26.943% of open-boundary
mass inflow** (fuel excluded). Net inventory increases 0.010450 kg. The
reported mass closure residual is −4.665e−5 kg; explicit interstep inventory
change is zero. No tolerance is changed and arithmetic verification does not
claim a new physical conservation certificate. Ambient O2 inflow is 1.912679 kg
laterally and 0.705395 kg at the top; this establishes substantial actual supply,
but does not prove local oxygen sufficiency or rule out local entrainment limits.

Sensible enthalpy has 211699.601 J frozen source increment, 215190.933 J top
outflow and −3491.258 J inventory change, with −0.002241 J closure residual.
Source components are 264166.417 J chemical, 52466.816 J radiated, zero pilot
energy, and 0.005280438 kg reacted fuel. Pilot input remains absent after discard.
The small positive top sensible-enthalpy inflow (0.077850 J) accompanies a much
larger incoming ambient mass; enthalpy uses the solver's sensible reference,
so its near-zero inflow must not be called zero ambient mass or energy.

At vertical faces z=0.204/0.245 m, sampled four-face RMS stress rates are
0.758/0.784 kg m−2 s−2, buoyancy 8.033/7.635, advection 11.100/11.307,
and combined projection 9.934/13.804. The momentum source rate is below
5e−7 in RMS at these heights. This localizes the sampled large terms to
advection and combined projection alongside buoyancy; it does **not** prove
that projection is defective or that smaller stress cannot affect stability.
A pressure/restoration split is unavailable and is not inferred.

### Decision at the reviewed evidence boundary

1. The doubled domain still has the same native 1.569578 Hz centerline peak;
   expected puffing remains unsettled. No production parameter is amended.
2. Global lateral starvation is not established: measured lateral mass supply
   is substantial. Top exchange is materially involved, making top-boundary
   location/sensitivity a defensible next isolation candidate after review.
   This is a hypothesis, not proof of recirculation or a chosen boundary law.
3. Reject pressure-tolerance relaxation, SGS tuning, endpoint pressure splitting,
   and a soot/tier-10 launch as unsupported. Do not use sampled term RMS to
   declare a solver defect or integrate frame rates across unsampled steps.
4. Fresh reviews passed (see below). Next map top incoming/outgoing flux by
   fixed source-centred radius and time using the retained operands before
   selecting a numbered boundary/domain sensitivity design. Preserve §3.6/§3.7,
   source/pilot, full windows, tiers, physical gates and golden checkpoint.

### Budget milestone review round 1 and correction

At `9d301217`, physical interpretation review returned zero P1/P2. Arithmetic
review reproduced the real final ledgers, every retained column hash and the
numerical claims, with no P1, but confirmed one P2: the localizer checked current
seals without binding consumed bytes to the earlier audit. A copied, resealed
final source record could be substituted under the old proof and mislabeled
verified. The original evidence was untouched and its numbers were correct.

The full audit now records its final-step SHA-256 and each verified column
SHA-256. The localizer requires those exact bytes before consuming either.
A fresh full audit passed exit 0 at 2026-10-01 05:58:31 UTC in external
`r245_full_verification.v2`; the prior
frame/history report is retained unchanged with its original provenance.
The unchanged-input GREEN and resealed final-source/column, foreign-case
and stale-unpinned-audit RED controls passed (`controls.v2.json`) under
`r245_localization_review_fix.v1`. The first control harness failed because it
restored an original record but retained its mutated sidecar; this was corrected
by restoring both. Its failed receipt/log remain external and are not relabeled.
The v3 localization consumes the fresh input-pinned audit. Two fresh post-fix reviewers of `39c32a1c` returned zero P1/P2.
The pin reviewer independently repeated all five controls; the evidence reviewer
reproduced all235 column hashes and sampled moments. See
`budget_review_round2.json`. This closes the descriptive budget milestone. No native solver or run payload changed, and no physical
bound or gate was relaxed. The original v1 reports remain historical evidence.

### Top radial/time diagnostic launch

At `552ccf6b`, `r245_top_exchange_gate.py` passed a hand-calculated staggered
plane/sign/limited-flux/counterdiffusion/Z-exclusion/sensible-reference/endpoint
control and missing-layout REDs. The full retained-frame mapper launched at
2026-10-01 06:03:49 UTC, initial PID89608, under external
`r245_top_exchange_launch.v1/{pid,completion}.json` and `mapping.log`, with
outputs in `r245_top_exchange.v1`. It rechecks capsule SHA/Merkle roots against
the fresh full-audit roots, uses pinned column clocks, and samples five fixed
source-centred radius bins. Endpoint top-cell temperature/face velocity are
labeled separately from the actual limited Heun scalar flux. No temporal
integration or boundary-class reconstruction is claimed. Completion, output
interpretation and fresh independent reviews remain pending. Only this one
heavy postprocessor may run; do not duplicate it.

The current canonical vertical-extent derivation also changes flow-through and
pilot clocks. Any later top-only experimental selection must explicitly retain
original source/pilot bytes and clocks and full original discard/statistics
windows; blindly doubling nz is not an authorized matched experiment.

### Top spatial map result (fresh review passed)

The mapper completed exit0 at 2026-10-01 06:05:45 UTC. It verified the actual
native/flux capsule SHA and Merkle roots against the full audit, and pinned
column clocks, across all235 frames. The 1175-row CSV is a complete235×5
frame/ring product. Provenance and summary are retained in
`docs/data/fire_r245/top_exchange_{by_radius.csv,provenance.json,summary.json}`.

Of the **sampled** top mass inflow, 95.516% lies at radius≥0.60 m; 97.569% of
sampled top mass outflow lies within radius<0.60 m. These are fractions of
unweighted235-frame mean rates, not of the integrated statistics ledger.
Inflow-weighted endpoint top-cell temperatures in the outer rings are
300.027 K (0.6–1.2 m), 300.000015 K (1.2–1.8 m), and300 K farther out.
Endpoint quantities are weighted by sampled incoming limited mass rates;
they are not R0/R1 donor temperatures or pressure classifications.

This pattern is consistent with peripheral ambient inflow around a central
hot outflow. It provides no affirmative evidence of substantial hot material
being copied inward at the top, and does not identify a boundary-law defect.
It also does not prove that top placement cannot affect the near-base response.
No frequency/causal/antialias verdict is obtained from these frame-rate maps.
The next candidate remains a top-location sensitivity preserving the existing
pressure-open law, rather than a change to that law based on gross top inflow.
Two fresh reviewers of `75e23630` returned zeroP1/P2. Arithmetic review
independently reproduced all ring summaries, proof hashes and capsule-based
extraction at frames0,117,234 (max absolute difference5.69e−14). See
`top_exchange_review.json`. The numbered top-only sensitivity design is
[the r246 draft](FIRE_SMOKE_TOP_LOCATION_SENSITIVITY_R246.md); its own
post-fix design review and engineering gates remain pending.

## Status and decision

The complete r244 baseline/wide comparison does not establish expected puffing.
Keep the production domain multipliers, source/pilot, solver, physical gates and
full-window policy unchanged. Do not start soot or tier 10. Localize the response
before selecting a boundary or transport remedy.

The first step uses existing data only. `tools/fire/r245_station_localization.py`
extracts all 8,916 statistics samples at all 80 heights from each **pinned** final
checkpoint. It locates the unique complete native probe-time vector and reads
subsequent vectors in `WriteSolverFrameValues` order. The native centerline
signal, accepted timestep history, height ordering and every station timestamp
are checked. This is a SHA-pinned block extractor, not a general checkpoint
loader or a simulation continuation. Summing station reaction times cell volume
reproduces the exported centerline heat release with maximum absolute difference
1.14e-12 W (baseline) / 1.36e-12 W (wide), from summation order.

The full station arrays and figure are external:
`/Volumes/SSD/RISE/fire_production_calibration/r245_station_localization.v1/`.
Small provenance and 480-row station summary are under `docs/data/fire_r245/`.
All samples span 10.531864278949797–25.156864278920693 s. No window is shortened.
The 8192-sample affine-detrend/Hann analysis is a processing sensitivity, not a
replacement for native r244 spectra, an antialias certificate or a physical gate.

## Preliminary observation, not causation

At the three heights with greatest temperature sample standard deviation,
baseline peaks are near 1.778 Hz; wide peaks are near 1.572 Hz. These heights
are 0.184–0.265 m. The wide reaction signal also carries that frequency near
0.184–0.224 m. Thus the shifted centerline peak has a near-base temperature and
reaction signature; it cannot be dismissed solely as a change in rendering.
However the centerline samples fixed central 2×2 columns, so lateral motion can
also change these observables.

The strongest vertical-velocity fluctuations near 0.306–0.428 m peak around
1.778 Hz in baseline and 0.342 Hz in wide. This separation makes signed momentum
and entrainment budgets relevant, but does not identify their cause. Per-height
normalization can emphasize tiny signals: use the retained sample mean/std
columns when interpreting the heatmap. First/second frequency bins contain only
about one/two cycles and do not establish stationary recirculation.

## Accepted collection contract

The retained station histories and maximum-velocity trajectory do **not** contain
historical signed stress, buoyancy, advection, source or boundary mass/enthalpy
fluxes. Final checkpoint fields and visual VDB frames cannot reconstruct accepted
R0/R1/R2 operands through the full past window. Do not rename a derived estimate
as an accepted budget. The old fixed-checkpoint eight-step fuel diagnostic is
not authorized to consume these cases, and the legacy checkpoint-physics helper
rebuilds baseline dimensions; neither is a valid wide-domain budget tool as-is.

Implement observation-only diagnostics on the existing accepted production path:

1. Use the doubled-domain case unchanged, from zero through its complete discard
   and statistics window. Preserve the full original spectrum/preview cadence.
   Keep the RSS ceiling, dynamic reserve and accepted-boundary graceful exit.
2. At each of the existing 235 statistics frame boundaries, retain immutable
   accepted state and the actual stage operands needed for signed column budgets
   through the source-centred 2×2 columns over all 80 heights. Use the source's
   existing integer translation; do not choose a moving column after seeing the
   result. Report advection, stress, buoyancy, source, combined projection and
   closure. A pressure/restoration split requires recorded operands; do not infer
   it from endpoint pressure or relabel combined projection. Capture actual R0/R1
   stage rates and R2 combined projection. The legacy one-shot frozen-force
   decomposition around `FireSequenceTest.cpp:5054/5190` is not an accepted
   projected-Heun budget and must not be reused under that name.
3. Collect accepted Heun-consistent mass/species and sensible-enthalpy inflow and
   outflow by lateral side and top, with bottom source separate, plus cumulative
   inventory/reaction/radiation/flux closure. Use the actual accepted stage fluxes;
   endpoint velocity × density is not the integrated scalar flux. Bind each
   record to accepted step, time, case, producer and payload seals. Accumulate
   cumulative closure over **every accepted step**, including discard for a
   full-run claim; frame cadence controls publication, not budget integration.
   Maintain a separate statistics-only ledger with the existing clipped first
   statistics-step duration. Use the actual averaged, limited R0/R1 scalar
   fluxes, not a recomputed unlimited or frozen-force substitute.
4. Before the empirical run, prove observation on/off leaves accepted fields,
   timesteps and source/pilot bytes unchanged on focused controls, include REDs
   for rejected-attempt contamination, missing stage terms and sign/side swaps,
   and get fresh independent reviewers. Instrumentation must not migrate an old
   checkpoint into a changed producer or relax physical/derived bounds.
5. Only after full-window budgets exist may a numbered design revision choose a
   boundary variant or solver fix. The current design §3.6/§3.7 pressure-open
   contract must be preserved unless a supported revision explains the change.
   No speculative pressure tolerance or dissipation tuning is selected here.

This is a diagnostic collection plan, not a completed budget localization, a
new boundary law, or permission to skip the owner's remaining milestones.


## r245 instrumentation — engineering gates passed; fresh review pending

The observation switch captures existing terminal buffers after native owner
publication sealing. It does not add a solver kernel, change the Heun tableau,
recompute a stage, or enable the qualification replay path. The native publication
layout and root remain unchanged. A separate Merkle packet contains the input
root, native publication root, accepted R1 candidate identity, averaged low and
antidiffusive scalar fluxes, and eight actual R0/R1 momentum rate fields. R0
advection is the operand recomputed under accepted R1 alpha, as required by the
existing owner. An explicit additional-staging counter records the observer.

`--fire-r245-lateral-budget OUTPUT` selects the unchanged tier-6 doubled-domain
case and its complete original window. No short empirical entry is added. Every
accepted step writes a sealed scalar-budget record only after launcher acceptance
checks, with separate full-run and clipped statistics ledgers. Failed/retried
attempts cannot advance the ledger. The first statistics inventory is explicitly
labeled a clipped linear increment, not a solved interior state.

The nine transported rows retain their native ordering: Z tracer, CH4, O2, N2,
CO2, H2O, CO, carbon, sensible enthalpy. A tenth row reports total species mass,
excluding the tracer. Total-mass inflow/outflow uses the sign of the summed
species flux at each face, avoiding false entrainment from counterdiffusion.
Each side retains inflow and outflow separately; the bottom also separates the
burner footprint. Source chemistry, radiation, pilot and reacted-fuel integrals
are taken from the same frozen packet. Any inventory change between a native
accepted output and the next input is reported separately and included in full
closure; it is not silently attributed to transport or omitted.

At the existing 235 frame boundaries, retained capsules contain the exact native
publication, exact observation packet, and beginning Q/source/M operands, five binary64 frozen-source
arrays (gas/soot heat release, radiation, pilot energy and reacted fuel), plus
burner mask. The column CSV uses the source-centred fixed 2×2 columns, all 80
heights, and actual Heun rates. Combined projection is terminal minus provisional
momentum divided by dt; no pressure/restoration split is claimed. Shared faces
appear once per adjacent selected column and must not be double-counted in a
future spatial aggregate. Files bind case, producer, step, time and payload seals.

With C cells and F packed faces, the extra device packet is 72+104F bytes plus
its 32-byte root and the existing Merkle-tree allocation formula/rounding. Host
reserve additionally accounts for that packet, the 76C+36F+216-byte native
publication, and 112C+4F+nx*ny beginning/source/momentum/mask bytes. The existing
32-GiB ceiling, 2-GiB general scratch reserve and accepted-boundary graceful exit
are preserved. This is accounting for new observation storage, not a widened
numerical acceptance bound. Full frame operands live externally, not in git.

The standalone reducer has passed ASan/UBSan controls for side/sign mapping,
limited flux, counterdiffusion, bottom partition, missing operands, rejected
attempts, duplicate/gapped clocks and clipped ledgers. The initial mixed-state
native gate passed exact observation-on/off publication/token checks, independently
captured R1 flux/stage-rate equality, and CPU/GPU Merkle agreement. Final-source cold/launcher controls and clean make/Deployment/Opto builds now
pass. Fresh independent reviews remain required before launch. `tools/fire/r245_budget_audit.py` independently checks
retained capsules and ledgers; fixture results are expressly nonempirical.


Final-source native controls and the independent capsule/audit mutations are
recorded in `docs/data/fire_r245/instrumentation_gate.json`; all six focused
commands passed in external `r245_validation.v4`. Mixed and cold observation
packets match terminal qualification operands byte-for-byte. The launcher
normal/retry controls retain exact accepted fields, source/pilot totals and
selected timesteps with observation on/off. Clean make and final test builds
have no warnings; Deployment/Opto have only the documented checkout OIDN and
AppIntents warnings. Mirrors pass mechanical regeneration checks. A prior
harness directory collision at the cold test's nested mixed prerequisite was
corrected by distinct destinations; failed and subsequent logs are retained.

Self-audit before review: inspect (1) final acceptance placement and retry
contamination; (2) actual limited R1 operands, stage-rate signs and total-mass
counterdiffusion; (3) clipped versus full closure and interstep inventory;
(4) native/observer/source/frame bindings and full-window audit coverage;
(5) transient allocation lifetime and explicit host/device storage accounting.
The observer intentionally adds full-grid terminal transfers and durable
operands. No throughput qualification or production-default performance claim
is made. No instrumented empirical run has started at this review boundary.


### Review round 1 — independent evidence audit correction

The two reviewers of `9df0a5f1` found no P1. Numerical/design review found no
P2; lifecycle review confirmed one P2 in the Python audit: statistics inventory,
closure/interstep fields and source-component metadata could be corrupted and
resealed without refusal. The native observation/reducer arithmetic was not the
fault. The corrected audit verifies every ledger recurrence, the clipped first
statistics inventory, and closure for all three scopes. Selected frames now
retain the five canonical binary64 frozen-source arrays for independent source
component reduction. The exact extra 40C host bytes are included in reserve;
the fixed ceiling remains unchanged. These are observation bytes only.

The full-run audit additionally binds producer/executable to sealed
`window_launch.v1` and compares every accepted dt to sealed
`monitored_diagnostics.csv`, addressing the review's broader provenance concern.
New resealed REDs cover statistics inventory/closure/interstep, cumulative and
statistics source metadata, and consistently corrupted source metadata checked
against retained operands. A hand-calculated three-event positive fixture
exercises the half-step statistics clip and nonzero interstep inventory changes.
The old focused fixtures remain external and are not relabeled as the new layout.
Fresh review is required on this correction before the full run starts.

Post-fix evidence: `r245_validation.v5` contains all six passing native gates,
three independent positive audit controls and 13 targeted resealed REDs. The
additional history RED changes two dt values with the total endpoint unchanged;
the producer RED changes executable identity. The incremental test/Job identity
build is warning-free; shared library files are unchanged from the clean
make/Deployment/Opto builds. Small hashes are in `instrumentation_gate.json`.
