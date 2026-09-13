# r235 — bounded experiment: batch EOS segment-obligation writes

Final status: rejected for no measurable wall saving. Candidate source and
its extraction test remain recoverable at `20ccb193`; production source and
the pre-existing test are restored exactly to r234. No acceptance changed.

## Evidence and scope

r234's final three-process baseline is 3.310496 s wall / 2.871500 s device
per hot accepted step. EOS evaluation is about 1.435 s, with another 0.050 s
for its table. Target assembly is about 0.779 s. See the r234 promotion receipt
for the executable, exact-time payload comparison and timing artifacts.

The retained diagnostic `r234_eos_prefix/repaired_iteration_probe.log`, SHA256
`e86a4d97f64b0a4f75ae2031ed3e93bcb6a5a15f802a461aaa0797cd58823abb`,
contains 156 extra uncached diagnostic dispatches: 78,727,896 cell evaluations,
73,294,494 lower-endpoint returns (93.10%), 1,927,818 with 24 bisections and
3,505,584 with 25. These counts are not production cache-hit frequencies,
momentum settling, or timing attribution. In particular, deeper prefix tables
do not remove the endpoint/admissibility work on the majority of these inputs.

Source inspection identifies another repeated operation in the remaining
direct energy evaluator: each successful species segment selection performs
a relaxed atomic OR to the same dispatch obligation word. Seven species can
therefore issue seven atomics per energy evaluation. The prefix consumer
already combines its selected-segment mask before one OR. Atomic contention
is a hypothesis, not a measured explanation of the entire EOS cost.

## Single-variable candidate

Restrict the first experiment to `eos_energy_dd`. Preserve every segment
predicate, species order, arithmetic operation and propagated enclosure.
Call its existing segment selector without publishing, accumulate the selected
segment bits in a thread-local integer, and OR their union once before leaving
the evaluator. Derive each bit from the successful selector's returned offset;
do not re-run or approximate the predicate. No changes to direct reference
evaluation, target assembly, table depth, root search or endpoint handling.

OR is associative and idempotent. This transformation preserves the final
dispatch bitmap only if no consumer uses intermediate publication as an
ordering signal. Audit those consumers before implementation. The word is
diagnostic obligation state, not an arithmetic input or lineage semaphore.
Retain bits from successful species even when another species is invalid;
null obligation pointers must remain permitted. Never suppress a refusal.
If any reader depends on intermediate visibility, reject this candidate.

## Gates and stopping rule

The independent direct evaluator remains unchanged. Extract the actual
candidate into the existing CPU sanitizer harness and compare all four energy
words, validity and final obligation bits with it. Cover different per-species
segments, ambiguous/invalid species after successful selections, null pointers
and pre-populated unrelated bits. Count writes in the extraction harness to
show at most one candidate publication; an omitted selected bit must fail.

Then run the native owner gate and eight exact-time canonical payload checks
against r234. Measure three separate eight-step hot-input processes with the
same command/profile settings and preserved checkpoint as r234. Preserve raw
logs outside git and bind them in this entry. Reject on any correctness
difference or no meaningful whole-owner saving; do not accumulate more changes
to rescue a failed timing result. Only a passing measured candidate proceeds
to clean build gates and fresh independent review before promotion.

No working-set increase, certificate framework, authority surface or migration
is proposed. Any changed production kernel starts a future window from zero.
Host serialization, 32-GiB RSS ceiling and 16-GiB disk reserve remain in force.
At planning, no heavy process is active and approximately 30 GiB is available.

## Candidate and CPU gate (not promoted)

The candidate changes only `eos_energy_dd` publication: a local selected mask
is ORed once after the species loop, including on an invalid mixture. The
independent direct evaluator is unchanged. An audit of Metal atomic readers
found failure/histogram readers, not an intermediate obligation reader; host
EOS obligation readback is a terminal blit after the dispatch, followed by
command completion. No bitmap acts as a progress semaphore.

The sanitizer extraction test covers 128 cases, including 64 invalid cases
and 68 mixed-segment masks, all four arithmetic words and existing unrelated
bits. Null pointers issue no writes; non-null calls issue at most one. An
omitted-segment mutant fails the final mask comparison. Existing EOS-basis
and prefix tests pass, including invalid/unvisited prefix obligations in both
production and qualification extraction modes. The old EOS-basis mask mutant
now targets this evaluator's actual publication mask rather than the unchanged
reference selector. No expected arithmetic result or bound changed.

Native gates and timing remain pending. These CPU results are not a production
owner qualification or a speedup claim. r234 remains promoted.

## Measured rejection

The focused build and aggregate native owner gate exit 0. Three separate
eight-step hot-input processes all reached step 1308 at exact time
2.1211945469258353 with `solver_accepted=1` and `migration_authority=false`.
The comparison below uses their outer trajectory `wall_ms` / `device_ms`,
not the nested inclusive owner intervals. SD is across three process means.

| Metric, ms/step | r234 baseline | r235 candidate |
|---|---:|---:|
| Wall mean | 3310.496078 | 3311.520616 |
| Wall SD | 2.485824 | 2.331615 |
| Device mean | 2871.500076 | 2876.741122 |
| Device SD | 3.161858 | 2.901032 |

The +1.025 ms wall difference is within process variation; there is no saving
to promote. The narrower atomic-write hypothesis did not yield a useful
whole-owner improvement. Reject rather than combine unrelated optimizations.
The planned extra canonical snapshot comparison, clean full builds and fresh
promotion review are not run: a cost-rejected candidate is never promoted,
regardless of its passing native owner gate. No bit-equivalence claim for an
unperformed eight-step snapshot comparison is made.

Measured executable SHA256:
`90d12c8fef6862f49aef0eaa7c44acdc00b2bbb25154a7af3699b19b084e6d10`,
preserved outside git as `r235_eos_obligations/FireSequenceTest_90d12c8f`.
Artifacts below are under `rendered/fire_production_calibration/`:

| r235_eos_obligations artifact | SHA256 |
|---|---|
| focused_build.log | 026a3f559d1b68a93ed6b099ef87aeda676591fb3534ffc19e07324e2e410b37 |
| owner_gate.log | 550cb7b64c6e0e63184f25b6bd75c88c83cf66b3c00775e3cdcd50e15b4f3db4 |
| timing_1.log | 1d0241dbac11c3a9bd87889de70afe8a987f4e4ea09c8256b16f37da2824e1f3 |
| timing_2.log | 913c34e88c8c82d5488563730e4942dd782e88b7d40b942406cc89e9858b7d59 |
| timing_3.log | 1024744f2779385131a5018d6c49a266761c3d862bb8d44c2df8b9e5c58f2b50 |
| timing_1/budgets/maximum_velocity_trajectory.csv | f1ff222c210c9983ebf17a2f25a044565153af5b3e7083e0ad039f81f8563acd |
| timing_2/budgets/maximum_velocity_trajectory.csv | 1e1990c6188cd80e1aff0e93e83105a500655a8c66ef41d288f938a235e9c1e3 |
| timing_3/budgets/maximum_velocity_trajectory.csv | d34641f8fe53b33c33aed50d4d3d22743aedd938fee7624bb089f0dde8bde905 |

All payloads and historical logs remain on disk. No simulation is active.
The generic `bin/tests/FireSequenceTest` is still the rejected candidate until
rebuilt; use the preserved r234 executable for any authoritative r234 run.
