# r234 — exact shared bisection-prefix evaluation

After r233, hot accepted-step wall is 4.047979 s and device time 3.606640 s.
The host residual is 0.441340 s, so eliminating all of it cannot meet the
one-second target. EOS remains the largest device family (about 2.14 s before
the final host-only repair). The r229 hot histogram records 24–25 bisections
for interior cells; r230 already removes identical R2 input inversions.

## Bounded experiment

The bit-lattice bisection tree depends only on Tmin/Tmax and integer midpoint
arithmetic, not the cell mixture. Its first ten levels have 1023 nodes. At
each node, seven compensated species enthalpies can be evaluated once on
device, rather than repeating their logarithm/polynomial evaluation for every
cell that traverses that same node. Recombine those exact four-word enthalpies
with each cell's densities in the original species order. Do not interpolate,
change a midpoint, skip a predicate, choose a new root, or alter the final
two-bin consumer. Beyond the cached prefix use the current evaluator.

Ten levels are a performance/working-set experiment, not a physical constant:
seven EOSDD values plus one status record per node require 130,944 bytes.
Together with the existing fourteen endpoint values, the table is 131,168
bytes. It remains a private per-EOS object produced from the existing sealed
thermo and parameters; no new public authority or CPU substitution.

Invalid or class-ambiguous precomputed nodes must not refuse a cell that never
visits them, and unused nodes must not add branch-obligation bits. Record
per-node validity/class information, then propagate it only on actual use.
Keep the direct evaluator independent; qualification must compare every used
four-word energy result, validity and class outcome. Tests must cover wrong
node indexing, altered enthalpy, invalid unused nodes and visited refusals.
Update existing working-set arithmetic for the actual extra table, not a new
certificate framework. If preserving these semantics becomes intrusive, stop
this candidate and return to another arithmetic strategy.

Baseline is the three r233 repaired hot-input processes, executable SHA256
`d80a0adbb13a1246b89d0d5be4c5ad17f9b7f813175baea6f9b68f7e3ec4df32`.
Repeat three processes, native owner gate and eight-step canonical payload
comparison before a cost decision. A table that is slower or fails identity
is rejected; no widening or shortened empirical run. Any promoted changed
kernel starts production from zero. No speedup is yet measured or claimed.

Rejected for this immediate experiment: a Newton/warm-start search changes
the sequence of ambiguous class predicates and requires a separate contract
argument. The exact-prefix route preserves the current search trajectory.

## Candidate implementation (not yet promoted)

The existing endpoint allocation/dispatch also produces the 1023 prefix nodes.
Each node stores seven enthalpies plus validity, selected-segment bitmap and
the exact midpoint word. Segment selection can omit its global obligation
write during table production; the consumer publishes only the visited node's
bitmap. It recombines species in their original order and, in qualification,
compares all four resulting words, validity and segment bitmap with the
independent direct evaluator. A missing table or exhausted prefix retains the
old evaluation. Final rounding and the two-bin consumer are unchanged.

The table grows from 224 to 131168 requested bytes, or from one to nine
16-KiB allocation quanta. Four retained owner sets therefore add exactly
524288 certified bytes. The native census, non-Metal census and generated
fp64/trace copies are updated; the observer-only delta is unchanged. The
initial aggregate owner gate correctly caught the historical absolute census
expectation before that test was updated with this allocation-derived delta.
This is not an enlarged numerical allowance. The final native gate must pass
as a whole; individual passing fields do not supersede its exit code.

`tools/test_fire_r234_eos_prefix.py` extracts the actual table builder and
consumer into ASan/UBSan C++. An independent breadth-first midpoint tree checks
all 1023 nodes in three synthetic mixtures against the direct arithmetic.
It tests altered enthalpy/class metadata, wrong node identity, unused-invalid
versus visited-invalid behavior, and uncached fallback. A three-segment record
ensures unused interior segment bits cannot hide behind the endpoint bitmap.
Both production and qualification modes run; corruption detection beyond
private-buffer ownership is explicitly qualification-only. Synthetic record
endpoints are strictly outside the case endpoints: CPU extraction preserves
subnormal record bounds that native Metal flushes, so exact record-boundary
inputs are not used as an assertion of cross-runtime admissibility.

Before promotion: committed-source native owner gate, three hot-input timing
processes, eight canonical payload comparisons, clean build gates, and fresh
independent review. No production window or migration uses this candidate.

## Committed-source measurement (promotion pending build/review gates)

Implementation `51c7445f`; measured executable SHA256
`7e2eb0e9440277bf6e55111efc91c5941ff1b82112da6e5c32d24f9eb6a08b73`,
preserved as `r234_eos_prefix/FireSequenceTest_7e2eb0e9` under calibration output.
Three separate eight-step processes use the same r208 step-1300 input as r233
(checkpoint SHA256
`2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2`).
These are isolated qualification steps, not a resumed or shortened production
window. The diagnostic's expected successful exit is 93; the owner gate and
canonical comparison exit 0. No simulation/build/render ran concurrently with
the final measurements.

| Metric | r233 repaired baseline | r234 committed candidate |
|---|---:|---:|
| Outer wall mean, ms/accepted step | 4047.979321 | 3310.047988 |
| SD of three process wall means, ms | 10.411663 | 9.330382 |
| Device mean, ms/accepted step | 3606.639763 | 2870.422426 |
| SD of three process device means, ms | 8.692895 | 9.470402 |
| Wall minus device mean, ms | 441.339558 | 439.625562 |
| Nearest-rank p95 of 24 steps, wall ms | 4476.067458 | 3623.642750 |
| Nearest-rank p95 of 24 steps, device ms | 4033.716415 | 3184.146877 |

Measured wall reduction is **18.23%**, not a one-second step or a full-window
projection. Producer counter intervals attribute EOS evaluation
2218.299219 → 1434.012202 ms/step and table construction
5.614330 → 50.041009 ms/step. The extra table work is included, not omitted
from the saving. Target assembly remains 774.238438 → 779.364425 ms/step.
These kernel intervals are diagnostic attribution, not asserted disjoint
components of whole-command time.

All eight canonical persistent payloads compare equal at exact end times,
excluding only producer build identity. The artifact records each cell/face
field independently, plus the complete canonical payload and ledger/history
mutation checks; this is not just a maximum-velocity comparison. The native
owner gate passes, including unchanged local enclosure and refusal checks.
Prefix extraction tests pass in both modes with 626 invalid interior nodes
handled identically to direct evaluation. Existing EOS-basis, exact-input-cache
and target-basis extraction regressions also pass without changing arithmetic
expectations. The synthetic native r230 cache probe passes after its private
table fixture allocation/dispatch is updated.

Maximum sampled RSS across the three processes is 1902116864 / 1900806144 /
1902936064 bytes, with zero compressed bytes. These short samples are not a
full-window memory claim; retain the run RSS ceiling. The tier-8 owner working
set certificate increases 5616254976 → 5616779264 bytes, exactly the derived
524288-byte increment. The observer-only increment and production refusal of
observer controls remain unchanged.

Artifacts under `rendered/fire_production_calibration/r234_eos_prefix/`:

| Artifact | SHA256 |
|---|---|
| committed_owner_gate.log | 0b141ac0a04db4038ca49118a3270ab2aaab7db6ac1e66df5fe122e323a70f3a |
| committed_numeric_comparison.v1 | 7b7da65a722438bbeb1615b81b8c2266125d3b031708e270fcfb74d0142d716d |
| committed_1.log | 07474d69c23f54767fa0008cbb75e5dd020a3fcc89d691ea2e43a46a8c6540d3 |
| committed_2.log | 6bcf8488b5ab0aaf6c55bc8b883be3bf32aedd9dc363943f6ff299dc315efce2 |
| committed_3.log | 877e6603d383b8554daea1f8fd20d5df2b738daf26a8221923b3e7aa1031e1be |
| prefix_cpu.v4.log | 1105a6aa80ea6ff9ac053d16d5e045c3c29219cb13c4eb0b045d95363ccf70b5 |

Earlier `candidate_*` timings are exploratory, not the promotion dataset.
`owner_gate.log` and `owner_gate.v2.log` fail the stale absolute working-set
expectation; v2 was launched before the test relink completed and is excluded.
`owner_gate.v3.log` passes, but the final gate is the committed-source artifact
above. Historical logs are retained, not rewritten. Clean make/Deployment/Opto
and fresh review remain required before promotion.

## Build gates completed; independent review pending

Clean make, Deployment and Opto builds pass. Make has no compiler warnings;
GUI logs contain only the known missing local OIDN search path and AppIntents
metadata notices. The GUI builds are compilation/LTO gates, not executed
Opto solver qualification. The measured executable above is preserved before
cleaning and remains the authority for the timing and payload comparison.

Additional artifacts in the same directory:

| Artifact | SHA256 |
|---|---|
| clean_make_build.log | e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29 |
| clean_gui_deployment.log | 0f7dfcd8d26fc95877aa96e5f665f915395c50096ae17c28c0dc0f5c5a787c0e |
| clean_gui_opto.log | 8703196324c8ef49aaee51ecaed7b814ec4a66e6b5ecaa7918519864d05c6f22 |
| native_cache.log | b264b6df8ddc53ff54fb6c51deed1e69a2331b646555777e8b00dacc0c79c957 |
| eos_basis_cpu.log | 814790bf0f8d92c7357777432ab91558d011891f4367f45e78c568b0f27b81bd |
| reuse_cpu.log | 0079369c7aa86711d240d7e07c9a391159279bbd4d573b0f43e917e55a7d5f0b |
| target_basis_cpu.log | c0eeb886a910cd0358822981643efff75be4e472ced560a952dccc85e0072ac5 |

Review self-audit: midpoint indexing/search trajectory; delayed invalid-node
and segment obligations; all-word arithmetic identity versus the independent
evaluator; allocation/dispatch/census siblings; and timing/evidence scope.
No production process is active. The one-second target and tier-10/spectral
deliverables remain unclaimed.

## Review round 1

Both fresh reviewers report zero P1s in the prefix implementation. The
arithmetic reviewer also enumerated 2096127 tree-node/domain combinations
(widths 1–2049) without an indexing mismatch. The evidence reviewer reproduced
the statistics and artifact hashes.

One surrounding P2 is confirmed: the optional EOS iteration diagnostic copied
through the following `eos_exact_cached_input` helper while renaming only
`eos_temperature`, duplicating that helper in the generated Metal source.
Narrow the copy to end before the helper. A source-generation regression checks
the actual extraction delimiter and demonstrates that the old delimiter
includes a second helper. This diagnostic-only repair changes none of the raw
Metal literals (concatenated SHA256 before/after
`ecfb46fa1354ca3e95450f85e94bbdead03dbe5ee440cc530514c5d57dfed163`).
Native diagnostic-enabled validation and fresh post-fix review are pending;
the earlier measured executable remains explicitly identified above.

## Post-repair native qualification and repeated measurement

Repair commit `1e07d533`; executable SHA256
`fab251f25acbd4f6bacf1250685eece634648aea9cfcd5b65616b899d166447f`,
preserved as `r234_eos_prefix/FireSequenceTest_fab251f2`. This is the final
measured candidate; the preceding measurements remain historical evidence.

The complete owner gate and eight-step canonical comparison pass again.
Three new eight-step processes measure **3310.496078 ms wall / 2871.500076 ms
device** per step; sample SDs of process means are **2.485824 / 3.161858 ms**.
Nearest-rank p95 over 24 steps is **3611.129334 ms wall / 3168.211791 ms device**.
Wall minus device is **438.996002 ms**. Against the same repaired r233 baseline,
the measured wall reduction is **18.22%**. Per-step producer intervals are
1434.974933 ms EOS evaluation, 50.035799 ms table construction, and 779.322331 ms
target assembly. No one-second or full-window claim follows from these samples.
Max sampled RSS is 1901608960 / 1891024896 / 1904197632 bytes, compression zero.

The repaired optional diagnostic completes eight accepted steps and 156 extra
histogram dispatches, totaling 78727896 cell evaluations: zero refusals and
zero published-temperature bit mismatches. These are uncached diagnostic
inversions, not production cache-hit timing or momentum settling evidence.
Their extra cost is excluded from the timing dataset. The three-test CPU
prefix/extraction suite also passes under ASan/UBSan. No fixed-k change is made.

Final artifacts in the same output directory:

| Artifact | SHA256 |
|---|---|
| repaired_owner_gate.log | e50c6b4bf1ff35dbf4f96d5e96dafa9838d7d6a461742085fcdd966562cfba7d |
| repaired_numeric_comparison.v1 | 6fbb1d6b6fc201ce382a61ea7f2da346636417be21b7f96f2587cd7d2354ee4f |
| repaired_1.log | 2ebab036c6434039561cb841ff5489c39ff053de2d9bbe8e671129c7853df6d3 |
| repaired_2.log | da17a97eb18e226c16109de9d08279ca5bcfff15e1a9619afb62ced90bfe7958 |
| repaired_3.log | 72740c6df477c0cfb064e97c931fd1a4a0138a476802921ac0f93a9e23f12646 |
| repaired_iteration_probe.log | e86a4d97f64b0a4f75ae2031ed3e93bcb6a5a15f802a461aaa0797cd58823abb |
| prefix_cpu.v5.log | 5921e633461a13f12a04ae641d81718ed15076d392fda838976367a16ada1988 |

Repaired clean builds and fresh post-fix review are the remaining promotion
gates. Neither this diagnostic nor the payload comparison authorizes migration.

The repaired clean make/Deployment/Opto builds now pass, with no compiler
warnings beyond the same known GUI notices. Their hashes are respectively
`e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29`
(`repaired_clean_make_build.log`),
`aa1c883fb94cad95408022e4eccb514bcdcab1d406df52182ffcaf186f48a4b9`
(`repaired_clean_gui_deployment.log`), and
`8bc6ac26e385d655651d04903b6c8b73f1c8f48c235320c28d644db6a45676ff`
(`repaired_clean_gui_opto.log`). Fresh review on this post-fix state remains.
