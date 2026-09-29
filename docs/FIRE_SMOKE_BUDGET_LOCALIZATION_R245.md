# r245 — budget localization after the matched domain experiment

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
