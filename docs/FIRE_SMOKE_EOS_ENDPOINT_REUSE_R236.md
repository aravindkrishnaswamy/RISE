# r236 — exact intra-cell endpoint aggregate reuse

Status: implemented and measured candidate, not yet promoted; review fixes
are undergoing verification. r234 remains the accepted source and executable.
r235's obligation batching was rejected
in `30d01730`: 3.3115 versus 3.3105 s/step, no meaningful saving. Do not retain
its changed kernel or stack another experiment on its executable.

## Derivation

In `evaluate_resident_eos_candidate`, admissibility already computes three
four-word quantities for this exact cell and input: lower endpoint energy,
upper endpoint energy, and the absolute endpoint-term energy scale. All are
accumulated in species order. After successful admissibility, `eos_temperature`
recomputes the same three quantities before deciding endpoint versus interior.
Its energy loops use the same terms and species order; its scale starts at
`abs(sensible)` and uses the identical nested absolute-term addition recurrence.

Reuse those three **unmodified** aggregates within this one device invocation.
Return them optionally from admissibility only on success, and accept them
optionally in temperature inversion. The production caller passes the local
result from its own preceding admissibility call; independent qualification
continues using the default, uncached path. No persistent cache, identity
surface, host value, buffer, or additional dispatch is needed. All affine,
mass, positivity, endpoint, class and rounding tests stay in their original
order. The scale must not be pre-normalized or replaced with the admissibility
threshold: temperature inversion still performs its original scale-order
decision and multiplication by feasibility.

This removes duplicated arithmetic, not an acceptance test. The surviving
admissibility evaluation still validates all source inputs. With the sealed
endpoint table, the eliminated reads have no additional branch publication;
the independent no-table path must remain available unchanged for diagnostic
callers. Exact-input cached temperature hits need no new inversion work.

The r234 uncached diagnostic has 93.10% lower-endpoint returns (SHA and scope
in r235). This motivates examining common endpoint overhead rather than only
deepening the interior prefix. It does not predict a speedup: carrying twelve
extra local words may increase register pressure, and the compiler may already
eliminate part of the redundancy. Measure the whole owner.

## Qualification and budget

Baseline: r234's three final repaired timing processes, wall 3310.496078 ms
and device 2871.500076 ms per hot accepted step. Use its preserved executable
`r234_eos_prefix/FireSequenceTest_fab251f2`, not the generic test binary left
by rejected r235. Same r208 step-1300 input, eight exact-time accepted steps,
three separate timing processes. No production migration or shortened window.

Before native timing, extract actual aggregate production and consumption into
the CPU sanitizer harness. Compare each of the twelve words to the uncached
formulation across endpoint/interior mixtures, signed species values and
inadmissibility. Compare temperature and alternate-bin outputs and obligation
bits; inject a corrupted aggregate and demonstrate qualification refusal.
Admissibility failure must never reach inversion with uninitialized aggregates.
Audit the iteration diagnostic's source extraction and other scalar callers
for the extra optional argument; keep their default independent behavior.

Native aggregate comparisons must be qualification-only and use the unmodified
recomputation, not merely compare final temperatures where different energies
could round to the same result. Run the existing owner/refusal gate, then
eight canonical payload comparisons at exact times against r234, excluding
only build identity. Reject on any numerical difference or no significant
whole-owner saving. Only a passing candidate earns clean make/Deployment/Opto
and fresh independent reviewers before promotion. No bounds are widened.

Production remains from zero after any kernel change. Host serialization,
32-GiB RSS ceiling, 16-GiB disk reserve and manifests-only git policy continue.
This experiment adds no infrastructure beyond the minimum extraction test and
qualification-only comparator needed to gate the arithmetic reuse.

## Staged candidate (not promoted)

Admissibility optionally returns the three raw aggregates only after its final
affine check. The live caller passes that local result to inversion after a
successful admission; the reference/default inversion retains the original
recomputation in `eos_uncached_endpoint_aggregates`. Qualification compares all
twelve words before consuming the cached aggregates. No threshold or predicate
is removed. The iteration histogram's parameter-injection marker is updated
for the optional argument; its default invocation still recomputes independently.

CPU ASan/UBSan extraction passes in both modes: 30 admitted and 10 rejected
synthetic cases, 10 endpoint and 20 interior inversions, identical aggregate
words, temperature/alternate outputs and final obligation masks. Rejected
admission leaves poisoned output untouched. In qualification, each of twelve
individually perturbed aggregate words refuses. Existing prefix/source-copy
tests pass. Native owner, timing and canonical payload gates remain pending;
these results are not a production performance or full-domain claim.

## Committed candidate measurement (promotion pending)

Source `9fd928d4`, executable SHA256
`d9397b6fddc682819724e2a02ef3c26f1d692eff8ef7b753e2e5e8c62ca83655`,
preserved as `r236_endpoint_reuse/FireSequenceTest_d9397b6f`. Three separate
eight-step hot-input processes complete with expected diagnostic exit 93.
The native owner gate and canonical snapshot comparison exit 0. All eight
equal-time persistent payloads match r234, excluding only producer build
identity, with zero per-cell/per-face bit mismatches. No migration is claimed.

| Metric, ms/accepted step | r234 | r236 |
|---|---:|---:|
| Outer wall mean | 3310.496078 | 3274.233087 |
| SD of three wall process means | 2.485824 | 0.648614 |
| Device mean | 2871.500076 | 2839.182922 |
| SD of three device process means | 3.161858 | 1.696100 |
| Nearest-rank p95 over 24 steps, wall | 3611.129334 | 3570.269750 |
| Nearest-rank p95 over 24 steps, device | 3168.211791 | 3131.429461 |

The measured wall saving is 36.263 ms, **1.10%**, not a major cost breakthrough.
EOS attribution is 1400.682 ms/step, table construction 50.093 ms and target
assembly 779.279 ms. These are diagnostic counter intervals, not claimed
exclusive partitions. Wall minus device is 435.050 ms. The one-second and
design targets remain unmet; no full-window projection is made here.

Sampled RSS maxima are 1914585088 / 1901215744 / 1913733120 bytes with no
compression. No device allocation was added; the existing owner working-set
certificate is unchanged. The local temporary's possible register cost is
included in measured device time. Heavy jobs were serialized.

Artifacts below are relative to
`rendered/fire_production_calibration/r236_endpoint_reuse/`:

| Artifact | SHA256 |
|---|---|
| focused_build.log | 7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51 |
| owner_gate.log | da60d4eaf4373f6fe53f1a1207440d7a9bd24011ac2efae72953391517b3d996 |
| timing_1.log | bded44d8e65684590f08c37df4ab07416ecd109a953318884e6454877acf179a |
| timing_2.log | 6665670d89a6ccdf371119ab091a9092449b5372fafc1f28718223a632fc1529 |
| timing_3.log | d05f50dc5b8e8ae7a74c772605a009c4438a412a054745971028113929e5c1e3 |
| snapshots.log | 14c04f6d22f2c1d3697ef056c4ea842be685f1c13d8061c88892788440124b8b |
| numeric_comparison.v1 | 918fb7c73b125efd9da884163e03a7f56b8108af65d5c7d7d98e3d897836b734 |
| timing_1/budgets/maximum_velocity_trajectory.csv | 15bb44e4f703cf2223d680327117f3fd01bd2cf219274ccb394419e6da0549c3 |
| timing_2/budgets/maximum_velocity_trajectory.csv | 6be82abd6cce4cc551a2fd8170d68568a57ddc3ca92a09c2590369de3da1abc0 |
| timing_3/budgets/maximum_velocity_trajectory.csv | 9568cfc9e775d37eaf411e23dad659ee4849ef78a883fe11e11d21efa4ba8ba7 |

The changed diagnostic source-generation path is being exercised natively;
clean full build gates and fresh independent review still precede promotion.
r234 remains the production authority until they pass.

## Gates complete; independent review pending

The native iteration diagnostic completes eight steps with 156 extra
dispatches / 78,727,896 cell evaluations, zero refusals and zero temperature
bit mismatches. This verifies the changed source-injection marker and remains
excluded from timings; it does not qualify a reduced Picard iteration count.
CPU endpoint, prefix, exact-input reuse, EOS-basis and target-basis regressions
pass. Clean make, GUI Deployment and GUI Opto builds succeed. Make has no
warnings; GUI logs contain only the documented missing local OIDN path and
AppIntents metadata notices. GUI build success is not an executed Opto solver
qualification. The preserved measured executable remains the timing authority.

| Additional artifact | SHA256 |
|---|---|
| iteration_probe.log | 7cc6043bbc054b285bf065932dbf0f308414fd9228a549562e5ced80bc4fc4df |
| endpoint_cpu.log | 872d27b3ad41a666809f0005c9491e33e951f4143c7e0c268306c7c95da0544d |
| prefix_cpu.log | 660c99baeb46876e4dde55e42746d6411442147e92a156c8c19ca2e1cc19e33e |
| reuse_cpu.log | e194ef21d51b541ea4c630b33fa9af272bf6e4004eb7536675266e5c130e2403 |
| basis_cpu.log | 0cfed229014cbaedf14283f23edf7223eb9d3f52332a13dbc7b43ec5714d08df |
| target_cpu.log | 6bed2496692356b5433df8f74f467acee64e1a89a080c2429382096d1870612d |
| clean_make_build.log | e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29 |
| clean_gui_deployment.log | 0c53ff079c5f24698d9837112af380674056d760debaff475b795b365f052b5b |
| clean_gui_opto.log | b709bb37f0757e64e8cfe8b64e07d6debbcd84cec81231c73122f5f7dd3bb0c6 |

Review self-audit: identical species accumulation and raw scale semantics;
refused inputs never publishing initialized-looking aggregates; private local
candidate provenance; independent twelve-word comparator and mutation checks;
diagnostic source-copy/default-call siblings; exact timing scope and hashes.
No heavy job is active. Promotion awaits fresh source/test and evidence review.

## First review and test-only repairs

Fresh source/test review finds no P1/P2. Evidence review independently
reproduces all timing, RSS, attribution and SHA claims, with two P2s: the
opening status still described the historical plan, and rejected-output
coverage checked only one poisoned word while prose implied the entire
aggregate. Update current status and compare the complete poisoned aggregate.
A nonleading-word mutation RED confirms the stronger comparison fails.

These changes touch only this entry and the extraction test; production source
and the measured executable remain unchanged. Historical logs are retained;
the strengthened CPU test is recorded separately. Fresh post-fix review is
required before promotion. The synthetic fixture has zero affine rows and no
nonzero alternate-temperature case; the broader native owner battery supplies
separate coverage, not a claim that these narrow CPU cases exhaust the domain.

Strengthened CPU test: three tests pass under ASan/UBSan, including the
nonleading rejected-output mutation. Artifact `endpoint_cpu_review_fix.log`
SHA256 `c9550ca4fcc08ff941d97686fc72f4c0fc5f00c64f5faa391af8f544fbb43176`.
An exact source diff against `9fd928d4` confirms no production-code change.
