# r228 — requalify target temperature reuse on the current owner

The r215 target-basis implementation remains on `codex/r214-target-basis`
(numerical change `a3dec05a`), not the completed r226 run's owner. Its
historical 1.196x wall improvement is not a current-build measurement.
Reapply only the target helper change, not that branch's historical payloads
or unrelated source changes. Preserve r217 exact division by five: the old
candidate's `0.2f` multiplication must not be reintroduced. Reuse each
temperature's basis within `target_tangent_at_temperature`, including the
separately evaluated adjacent-bin path; do not alter the current two-path
hull, downstream lineage clauses, or r226 projection recovery.

First establish three sequential eight-step hot baseline measurements with
the existing `--fire-production-owner-eos-refusal` diagnostic and retained
step-1300 checkpoint from r208 (SHA
`2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2`).
This is isolated qualification, not a production continuation or migration.
The historical diagnostic's positive solver-completion line, not its exit
code alone, determines completion. Profile owner/producer kernels; preserve
per-process evidence. No overlapping heavy jobs.

After integration, require the current r201 owner/target gates, separate
four-word per-species comparisons, and equal-input persistent numerical-byte
comparison excluding only the explicitly changed producer build identity.
Measure three matched hot processes with the same profile settings. Report
whole-step device/wall distribution and inclusive target timing without
adding overlapping intervals. No promotion before gates and fresh review.
The 1 s/step and subsequent 300–500 ms budgets remain unmet. Fixed-k remains
unselected: frozen provisional-momentum deltas are not convergence evidence.

## Qualification result (candidate; review pending)

The implementation hoists only pure temperature primitives across seven
species. Qualification mode compares enthalpy and cp separately, including
hi/lo/tail/bound words; production does not compute the direct witnesses.
Both adjacent-temperature paths still compute their own basis. No buffer,
working-set, policy, target hull, or iteration-count change is made.

Eight saved equal-time steps 1301–1308 pass the existing persistent-byte
comparison: zero scalar, temperature, momentum and velocity bit mismatches;
the entire canonical persistent payload matches after excluding only
producerBuildId. This is a numerical comparison, NOT migration authority.
The reference executable is preserved r226 (SHA
`6acc6871c80d4eb7744403039cf7db5271a4b603509d7fafd11f3cb8c3da864d`);
the candidate SHA is
`b9742c395bc1049bda1f4077843aa74c6a33d6435adc50c7e6b39c3113ad7190`.
The timing baseline was the r227 executable
`81d5acb91d29aed89d7eecdea3a96b4a6c03073727a25b673e60b1eec8109832`,
hashed before being overwritten by the candidate build, not frozen. r227
changed preview output patterns, not solver arithmetic. Do not conflate the
preserved numerical reference with the timing binary.

Three sequential processes per configuration, eight identical-input hot
steps each, with RISE_FIRE_OWNER_PROFILE=1 and
RISE_FIRE_PRODUCER_KERNEL_PROFILE=1 (no snapshot output during timing):

| Quantity (ms/accepted step) | Baseline | Candidate |
| --- | ---: | ---: |
| Launcher wall mean ± sample SD of three process means | 8542.611 ± 7.366 | 7169.966 ± 12.535 |
| Launcher wall p95 | 9155.555 | 7655.947 |
| OwnerRun wall mean | 8466.716 | 7093.950 |
| Device summed time mean | 6649.284 | 5275.810 |
| Device summed time p95 | 7144.476 | 5643.258 |

p95 uses sorted index floor(.95*(N-1)), N=24. The wall reduction is
16.07% (1.1914x), not the 1 s target. Profile scopes overlap: OwnerRun and
producer kernels are attribution views, not additional step costs. These
are hot diagnostic steps, not a full-window or uninstrumented timing claim.
Kernel-scope device sums per accepted step attribute the win to target
evaluation: 2227.196 → 849.323 ms. EOS is unchanged within these samples
(3801.372 → 3804.702 ms) and remains the largest cost; advective-pair
evaluation is 159.120 → 159.305 ms. These sums are included in device time.
All six runs report solver_accepted=1 and checkpoint_unchanged=1; historical
exit 93 on this diagnostic is not interpreted as a production success code.

Focused changed-source build and existing resident-target/owner gate pass.
No full clean GUI/Opto rebuild is claimed. Extracted CPU helper tests cover
1792 synthetic species/temperature pairs (1701 accepted, 91 refused), with
four-word identity and branch/status equality. Wrong power and inexact
division mutants fail. The omitted-bound comparator mutant fails on a
synthetic changed bound; this is NOT a live-device consumer mutation test
or exhaustive thermochemical-domain coverage.

Self-audit: preserve division by five; do not share across adjacent-bin
temperatures; compare bound words as well as values; keep witnesses out of
production; distinguish nested timers and the two reference binaries.
Existing direct enthalpy/cp helpers remain the qualification witnesses.
Fixed-k and additional cost work remain unpromoted.

Evidence bytes remain outside git under
rendered/fire_production_calibration/r228_target_cost/. SHA256 manifests:

| Artifact | SHA256 |
| --- | --- |
| numeric_comparison.v1 | 1dfff310763e61fb26448f69c4e1b5d64805481892bb43499e5b507234fa6e5c |
| candidate_owner_gate.log | f565951a2a5612e7a61abc09551cac7f7424e3a8d50fcf3fa2fedb8468a073e5 |
| candidate_build.log | 7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51 |
| cpu_helpers.v2.log | 0ded1ac79bbe455d92317977b2c207525fd246093e5fc180d54adc0adc14c19f |
| baseline_1.log | 00915a0a37a419f1fe37f7df8d169e3e6a545e90fe60f8f9e1f1e8a1bd6ed590 |
| baseline_2.log | fd7cd873279c770c49e1c7c72773cf0582b7597aee5567ce7b8e9b81a1b5a49f |
| baseline_3.log | 9d1317a9707a3bb3a3af745d1b8a79b9478609fa71365e98058d48440b14935b |
| candidate_1.log | 5d1573cd59eec3d268f18f42474ff0758f09f4b38797332205f29cc4b86aa218 |
| candidate_2.log | 626d55cdbf4221d2c5d1092b69a35cd1bedbc0543b0c3b4edad1e9ef6699a60a |
| candidate_3.log | 140a86949c9ec8ac0c68f9354264faf499816265d0c65d3fb351e67d1af371dc |
