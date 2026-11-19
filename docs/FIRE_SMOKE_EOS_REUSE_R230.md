# r230 — measure exact-input EOS reuse, not an assumed warm-start win

The complete eight-step hot histogram from r229 reproduces the current
inversion with zero refusals and zero published-temperature bit mismatches.
This extra diagnostic dispatch is not included in production timing claims.

| Stage | Producer calls | Cell evaluations | Lower endpoint | Previous T bit-identical | Bisections |
| --- | ---: | ---: | ---: | ---: | ---: |
| R0 | 54 | 27,251,964 | 25,372,340 | 25,372,407 | 46,323,780 |
| R1 | 44 | 22,205,304 | 20,672,108 | 20,674,934 | 37,785,116 |
| R2 | 58 | 29,270,628 | 27,250,046 | 29,270,628 | 49,798,336 |

Every interior evaluation takes 24 or 25 bisections. R2 repeats identical
temperatures at all cells, including 2,020,582 interior evaluations. This
does not establish that a previous temperature alone is sufficient authority
to skip inversion: it only names where to inspect exact input reuse.
The log, retained outside git at
`rendered/fire_production_calibration/r229_momentum_settling/eos_hot_histogram.v1.log`,
has SHA256 `e1db57d57eb618e7c58b45553ad6cc7521f34c7f7cbcac31a1f00fabd1df5983`.
The command ended with solver_accepted=1, checkpoint_unchanged=1,
migration_authority=false; its historical exit 93 is explicitly diagnostic.

Investigate a private, exact-input temperature-result reuse on R2 first.
Reuse is admissible only from an already sealed device-produced EOS result,
with the full conservative input and numerical EOS parameters/thermochemistry
unchanged. Preserve both temperature lanes (the r225 adjacent-bin result),
all admissibility checks and current candidate-parent publication. A merely
equal nominal temperature, nonzero identity or matching pointer is not a
proof of identical numerical inputs. No CPU substitution or new production
authority surface is authorized by this plan. If this cannot be expressed
as reuse inside the existing qualified kernel/authority chain, do not add
provenance machinery to force it; prefer another arithmetic cost change.

Rejected as an immediate claim: a guessed previous-root shortcut can change
which ambiguous predicate the search encounters. It needs its own contract
qualification and is not bit-identity merely because both roots are close.
Also, common-temperature energy primitives are already reused in
`eos_energy_dd`; repeating that r213 change cannot create another saving.

After r229 serialized clean builds, establish three hot baseline processes
with the preserved current executable, then implement one candidate only.
Gate on the current owner, per-cell temperature/alternate-lane equality and
stale-input/cache mutation REDs, unchanged residency, and eight equal-time
persistent payloads (excluding build identity only; not migration). Measure
three candidate processes before promotion. No tolerance widening, no fixed-k
claim, no full-window speed claim from this isolated qualification. Any
kernel-changing production run starts from zero.

## Candidate integration, unpromoted

The first implementation reuses only R1's sealed temperature pair during
R2. R1 remains alive throughout R2. The existing private EOS object carries
non-owning references to that parent/candidate; no buffer or authority type
is added. Encoder validation requires the immediate candidate capability,
original command, physical/transport/candidate publications, same EOS thermo,
private device buffers and exact extents. A device-side comparison then
checks every parameter byte and all nine conservative words at the cell.
A mismatch recomputes the original inversion. A hit preserves both published
lanes, translating an identical second lane back to the inverter's zero
sentinel until the existing output normalization. Admissibility, represented
pressure/deviation and publication still run on the current candidate.
Qualification mode recomputes the original inversion and compares both lanes.

No owner counts or production buffers change. The existing physicality and
downstream lineage clauses are not suppressed. The cache is not admitted by
a nonzero identity alone. The changed kernel requires from-zero production;
the historical input is used solely for qualification.

Three baseline eight-step processes completed before code changes:
device means 5270.173, 5295.348, 5250.146 ms; wall means 7217.173,
7183.411, 7131.772 ms. These are process means, not p95 values. Logs under
`rendered/fire_production_calibration/r230_eos_reuse/`:

| Artifact | SHA256 |
| --- | --- |
| baseline_1.log | 97506bcf9b992052fc21121071b017a9c198c3245eea0a923b53958dbd963494 |
| baseline_2.log | 0bfa4cc673b44a3ec6af76eca395cf2fd087e86e59f91e5fe7e80b54dd99ebd7 |
| baseline_3.log | a4b732995ecf23ae708afdcfe9f40a92168155aeb4f92ad5d9917254b4239cb9 |

Focused candidate build passes without warnings. Three CPU extracted-selector
tests pass under ASan/UBSan, including every conservative word, parameter
byte, signed zero and null inputs, and mutants omitting energy or parameter
comparison. These do not prove native cache lineage or temperature values.
The first candidate owner gate passed. Its eight-step comparison passed with
zero bit mismatches in each scalar, temperature, momentum and velocity field;
the complete persistent payload also matches with only `producerBuildId`
excluded. This is qualification, not migration. Artifact
`numeric_comparison.v1` SHA256
`a00035fcb0e2faa36f228855ac20e4d8aefa2662160256bf342991d1a5e3f35e`.

Native synthetic cache REDs pass: identical inputs preserve both lanes;
one-ULP corruption of either cached lane refuses in qualification mode;
an unsealed cache refuses; changed energy or parameters recompute. The fixture
uses extracted production kernels but shared synthetic buffers, so it does
not claim production authority or prove host parent authentication. Artifact
`native_cache_red.v1.log` SHA256
`b264b6df8ddc53ff54fb6c51deed1e69a2331b646555777e8b00dacc0c79c957`.
The explicit zero-cache-identity refusal was added after the first timing and
snapshot runs. A final focused rebuild passed without warnings; its owner
gate and eight-step comparison are being repeated. Earlier snapshots and
timings are not attributed to this final binary.

Three first-candidate eight-step profiles measured OwnerRun wall
5726.232 +/- 6.833 ms versus baseline 7084.959 +/- 26.147 ms (mean and
sample standard deviation of three process means). Device sums were
3916.910 +/- 8.707 versus 5271.889 +/- 22.650 ms. Nearest-rank p95 over
24 steps: OwnerRun wall 6289.436 versus 7589.117 ms; device 4355.928 versus
5650.391 ms. OwnerRun scope excludes the surrounding launcher; it is not
the outer step-wall metric recorded above. These are isolated hot-input
qualification timings, not a full-window projection or a promotion.

| Candidate timing artifact | SHA256 |
| --- | --- |
| candidate_1.log | 9b5729318ee6848022de857a9a47fb5119756cfaffa42fdf2ee6daf1b6f8902c |
| candidate_2.log | acbec68e174cbb8ea99284b79be435a96af31f12b6c122ac299d48158e59aca0 |
| candidate_3.log | 38a0aac5ae4665bc1e867e4a6285723c1fa01287e45b47df1b90d6234cab7a02 |

The final-guard executable SHA256 is
`b1f031d88c6aa2413cfbdf9ff95ea46888f6db25a499d6a75c1b461e52d8a263`.
Its owner gate passes (`final_owner_gate.v1.log`, SHA256
`5a65588742cb75fff28c7ea39287f2a866e9ce30d1dd44b2f79e46b2f799e7c1`).
Its eight-step comparison also passes, with zero per-field bit mismatches and
exact canonical payload equality excluding only the build identity
(`final_numeric_comparison.v1`, SHA256
`e72f4edd7bc8a4b46b73914ef1ecc0bc759d52a5c7098d6ffbfa812441d64d1c`).
Three final-binary eight-step profiles completed: OwnerRun wall
5751.455 +/- 13.494 ms, device sum 3939.362 +/- 15.041 ms; nearest-rank
24-step p95 6321.828 ms wall / 4384.927 ms device. Baseline scope and sampling
are the same as above. Final timing log SHA256 values:

The outer accepted-step wall metric (trajectory CSV, including the owner
call's surrounding work) is 5827.083 +/- 13.737 ms versus baseline
7177.452 +/- 43.012 ms, p95 6392.050 versus 7660.015 ms. This is an
18.81% reduction on this hot eight-step qualification, still well above the
one-second target. No full-window speed is inferred.

| Artifact | SHA256 |
| --- | --- |
| final_candidate_1.log | 3d9305efc257f12f231caf94fee71197b90608e285117ba74270dea723c8d659 |
| final_candidate_2.log | 8939077678598a0145d6343c44cc00ab1a45ea9900cfb34400f0aa3ff5d8edc7 |
| final_candidate_3.log | 67a8b6ff909dbba5aa940f59d7e13ee09d13d3161de308006e588f3090e12cb0 |

The extracted host encoder guard passes native-buffer ASan/UBSan tests for
the linked parent, unsealed/forged capability, foreign command/thermo/candidate/
physical/transport parent, CPU-shared surface, short temperature buffer and
incomplete pair. This branch-level synthetic fixture is explicitly not a
capability-issuance proof; the live owner gate covers the assembled owner.
`parent_guard.v1.log` SHA256
`eb1fdd2fea1d8f21e8d3d77e21befce9ba6cf34f7c879fb88e4a74384c22b1d5`.

Serialized clean make, GUI Deployment and GUI Opto builds pass. Make emits
no warnings; GUI logs contain only the documented missing-checkout OIDN path
and AppIntents notices. These GUI builds are not executed Opto solver tests.

| Build artifact | SHA256 |
| --- | --- |
| clean_make_build.log | e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29 |
| clean_gui_deployment.log | 73b9233d19c2420a30738da44a8964d7cced6629cc44b9399ba042c582f9c9cc |
| clean_gui_opto.log | 376b923d2d27c166488b2bb18869f698c9e559c85c4109043470715a7cfa905c |

Self-audit axes: stale or forged parent admission; R1 lifetime through R2;
exact-input coverage and alternate-lane normalization; qualification versus
production-mode arithmetic; measurement scope and fixture overclaims. Native
owner, paired hot snapshots, kernel mutations and extracted guard tests cover
these from distinct directions; independent review remains the promotion gate.
Latest promoted wall cost is 7.17 s; no fixed-k shortcut is claimed.

## Independent review, round 1

Two fresh reviewers inspected `a9da7ca2`: lineage/lifetime/correctness found
no P1/P2; test integrity/cost evidence found no P1 and one confirmed P2.
The native parameter-mismatch fixture originally retained correct cached
temperatures, so its output did not distinguish reuse from recomputation.
Fixed by simultaneously poisoning a cached temperature lane. Correct fallback
still produces 300/300; the native parameter-comparison-bypass mutant now
fails that case with bit 7, exit 96. No production code or measured executable
changed in this repair.

| Review-fix artifact | SHA256 |
| --- | --- |
| native_cache_red.v2.log | b264b6df8ddc53ff54fb6c51deed1e69a2331b646555777e8b00dacc0c79c957 |
| native_parameter_mutant.v1.log | 743c9e6a67990e28c95c682380795302f1c5085f7df4abc2338b52115846d1a3 |

Remaining coverage limitations, not asserted as proved: the synthetic native
fixture does not positively exercise a distinct adjacent-bin pair in both
compilation modes, nor does the extracted host guard exercise capability
issuance by the assembled owner. Both lanes are checked in the qualification
kernel and the actual hot-owner payload comparison. A fresh post-fix review
is required before promotion.
