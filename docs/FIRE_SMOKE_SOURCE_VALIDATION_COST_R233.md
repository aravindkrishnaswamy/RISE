# r233 — attribute and remove repeated frozen-source scans

The promoted r232 hot step costs 5.417490 s wall / 3.532581 s device.
The 1.884909 s difference is not assumed to be hashing. A read-only 20-second
native process sample of the preserved r232 diagnostic identifies repeated
`EncodeResidentFrozenSourceAuthority → FireProductionFrozenSourcePacketSeal::Matches`
and its content/radiation hash scans inside producer construction. Collapsed
top-of-stack counts include 191 in `HashSourceDoubleValues` and 173 in
`HashFluxValues`. These are samples, not exclusive wall percentages; the
capture includes startup and GPU waiting. No speedup is claimed.

`rendered/fire_production_calibration/r233_host_profile/cpu_sample.txt`
SHA256 `eee7b376b68a9290530c49c90bf7cca42ff682280cd3596a7d4327cbdc367e20`.
The eight-step diagnostic completed successfully under the unchanged binary,
without migration or production output. It is not a timing baseline because
the profiler perturbs execution. Use r232's three unprofiled-by-sampler timing
processes for the next candidate comparison.

## Bounded next design

Source packet vectors are private and exposed only as const references, but
the owner borrows its request: a const reference alone does not make the
caller's object immutable. Do not simply remove repeated validation on that
assumption. Evaluate taking one private const source snapshot for the owner,
validating it once before uploading the source, and using that same snapshot
for every producer. The encoder's metadata, private-buffer and immediate-parent
checks remain. Standalone qualification callers must retain full validation.

At publication, verify the caller's source still matches the admitted source
identity and its own full seal; callback mutation must still refuse atomically.
No cached boolean supplied by a caller, new authority type or weakened identity
check is admissible. Prefer factoring the existing private encoder call sites
over extending public API. Account for snapshot copying and bounded RSS in
the measured cost. If the lifetime/issuance boundary cannot make this exact,
retain validation and pursue EOS instead.

Before promotion: native owner and callback/forged-source REDs; eight equal-time
payload matches; three hot timings; working-set/RSS accounting and fresh review.
No solver arithmetic or kernel edit is intended. This plan does not authorize
kernel-transition migration, physical gate changes or new production evidence
machinery. The one-second target remains unmet.

## Candidate measurement, unpromoted

The implementation owns one const source snapshot, validates it in Prepare,
and supplies that same value to every private producer call. At publication it
fully validates the caller's original and compares packet identity. Standalone
qualification revalidates immediately before entering the private encoder;
all per-candidate metadata/private-buffer/parent checks remain there. No public
API, skip-validation flag or new authority type was introduced.

The measured executable SHA256 is
`26cbdfdda6eca6028556b3b80502b85ad85d693dd7f4bc285d36aeb8a49bfc30`,
preserved as `r233_source_validation/FireSequenceTest_26cbdfdd` under calibration
output. Three eight-step hot-input processes measure outer wall
**3934.467 +/- 1.732 ms**, versus r232 **5417.490 +/- 14.320 ms**.
Device mean is **3536.265 +/- 3.001 ms**, versus **3532.581 +/- 14.901 ms**.
Thus the approximately **27.37%** wall saving is host work, not faster solver
arithmetic. Candidate 24-step nearest-rank p95 is **4326.309 ms wall /
3926.971 ms device**. SDs are across three process means; no window claim.

All eight canonical payloads match, excluding only producer build identity;
per-field bit mismatches are zero. Native owner gates pass. Additional
copy-lifetime and unsealed-source REDs are being run; the existing device
callback-mutation RED is not presented as host callback injection (the native
owner exposes no host callback).

One source snapshot adds 108 bytes/cell of vector payload: 54,503,928 bytes
at this 504,666-cell fixture, plus object/string overhead. It is one bounded
owner-lifetime copy, not per-iteration retained state, and adds no device
buffers. Max sampled RSS per process was 1,900,675,072 / 1,914,798,080 /
1,903,017,984 bytes. These short captures do not establish full-window memory
behavior; the production RSS monitor remains mandatory.

Artifacts under `rendered/fire_production_calibration/r233_source_validation/`:

| Artifact | SHA256 |
| --- | --- |
| owner_gate.v1.log | ea6a359b79ae8e292197d403109044cb86fcf9cba27e41c60790cc5cc45ad480 |
| numeric_comparison.v1 | 7a523170b2b1baab36cc67dcf13d97dc9029fb300e9eadfc20d2ed47f906eff9 |
| candidate_1.log | 5f5865d8c2471e7691e2c108eebf31f90f7aefc8d49f0c01ec98c55a0bd3737c |
| candidate_2.log | 29ae680933a6304a40e039f881f55f7604b7b6ddb1e72d8191798ee10cfcb346 |
| candidate_3.log | 5e6ebd0fd1cc1d54de8358734705feb96a74d65e9b9c7e508d244ee645668e50 |

Clean build/configuration gates and independent review remain pending.

The final test-only additions pass in executable SHA256
`831cf1b399179ae6741efe1cc45c80547ba93998061aff9dee4e39f05c635071`.
`OWNER_SOURCE_SNAPSHOT_RED` witnesses independently owned vector storage and
a still-valid const copy after replacing the original object. The unsealed
source owner call refuses without publication/staging. This does not isolate
the Prepare check from other ingress checks, and is not a claimed mid-run
host callback injection. Existing device callback-mutation REDs still pass.
`final_owner_gate.v1.log` SHA256
`ee6b9549edc65e1c1f143f50275bfbab9620f972ed5d2987924645f2ac157990`.
Timing and eight-step artifacts above remain attributed to the earlier measured
executable, not this test-only relink.

All 11 raw Metal source literals are unchanged from r232; concatenated SHA256
`549e34ba3ae624f85b610dcd03d3f91f3f166bf69a31a53db47ea82d11bddf51`.
This is a source-literal comparison, not an assertion that differently linked
host executables have identical hashes. Clean configuration builds are running.

Clean make, GUI Deployment and GUI Opto now pass. Make has no warnings; GUI
has only documented missing-checkout OIDN and AppIntents notices. This is not
executed Opto solver qualification.

| Build artifact | SHA256 |
| --- | --- |
| clean_make_build.log | 41f3c8fce9d32b2b9b64f6089a4cbd96fcda83bedf1cc715c2311cea434fca16 |
| clean_gui_deployment.log | 4414ca2f4f287e5f30e457bda93021b923dcbcef40e8c4d9f9db28cbd25e8926 |
| clean_gui_opto.log | 0786f45ed7e86902f97a37e2490db5f2c5719176af7d3b2a0c9f93f5d15b87d9 |

Self-audit before review: borrowed-request mutation versus owned snapshot;
all private encoder callers preserving validation; final identity and atomic
publication; snapshot lifetime/RSS; timing scope and test-only relink identity.
Fresh independent review remains required before promotion.

## Round 1 findings — not promoted

Cost/test review returned no P1/P2. Lineage review identified two confirmed
P1 admission gaps: checking only source nx before uploading a full shape-sized
target could read beyond a smaller sealed source; independently supplied EOS
dose/beginning-state inputs were not bound to the canonical packet in the
owner (the standalone comparator already enforced this).

Repair moves the existing full beginning-state/shape validation before any
owner source upload, and also checks case, methane record, timestep, attempt,
target extent and byte-identical dose against the seal. No new tolerance or
authority is introduced. Source-copy scheduling remains, but earlier timing
numbers are now pre-repair evidence, not a final repaired cost claim.
Added owner REDs cover modified finite dose and authentic sources from a
different beginning state or case. Native build/gate is running; further
shape-specific coverage, repeated measurements and fresh review are pending.

## Repaired qualification

The repaired executable SHA256 is
`d80a0adbb13a1246b89d0d5be4c5ad17f9b7f813175baea6f9b68f7e3ec4df32`,
preserved outside git as `FireSequenceTest_d80a0adb`. Native owner gates pass,
including modified dose, authentic wrong-beginning and wrong-case sources.
Direct admission-helper REDs reject differing ny, nz and cell width; their
scope is explicitly the helper, not a claimed full-owner smaller-source replay.

The repaired eight-step canonical comparison passes with zero per-field bit
mismatches. Three repaired timing processes measure outer wall
**4047.979 +/- 10.412 ms**, device **3606.640 +/- 8.693 ms**; nearest-rank
24-step p95 **4476.067 ms wall / 4033.716 ms device**. Against r232 outer
5417.490 ms, this is **25.28%** lower. The observed device mean is 74.059 ms
higher than that baseline despite unchanged kernel source; do not call device
time identical. The wall-minus-device difference falls from 1884.909 to
441.340 ms. These are separate matched-input process batches, not an
interleaved clock-controlled experiment or a full-window claim.

Sampled maximum RSS per repaired process: 1,903,624,192 / 1,902,542,848 /
1,899,479,040 bytes. Earlier pre-repair timings are not the promotion result.

| Repaired artifact | SHA256 |
| --- | --- |
| repaired_owner_gate.v1.log | 8507b32a631c23fee9cfb385caf1fee52787ad15df9fe0edb9548fadbdfc6bbd |
| repaired_numeric_comparison.v1 | 1a58bc1432f6a43d3544527ce6f8be186e2b228362a93b85a2ab5b9e916ccef3 |
| repaired_1.log | 38187c6912608190811ce60256e17310a33baaf41e92b7f1b918697f6d52f05d |
| repaired_2.log | 6fe7b10c15663f23941d5a1f605b0caa2d20b1d8c468d6991bf8f4b15fcbb933 |
| repaired_3.log | 6cb0bb7959c1f7148bef5fb019547ea5b22a2d325002d7987144ccb01dc7e68e |

Repaired clean builds and fresh post-fix review remain pending.

Repaired clean make/GUI Deployment/GUI Opto builds now pass, with only the
documented GUI OIDN-path/AppIntents notices and no make warnings. This remains
compilation coverage, not executed Opto solver qualification.

| Repaired build artifact | SHA256 |
| --- | --- |
| repaired_clean_make_build.log | 991f46f9f1aeaf53d891c0ef92773e0641134fb2ee23f12a76fe39e20fe280df |
| repaired_clean_gui_deployment.log | e5772b04dd98d69d0eb1182ec69233f4cb68480e4f3037104a04beee43914e04 |
| repaired_clean_gui_opto.log | 1fb91ea589b20b6a25c69a80abccfa8b628512e843459280149f067640c7ff30 |

The two confirmed findings are implemented and regression-gated; independent
post-fix review is the remaining promotion gate.
