# r220 — owner autorelease lifetime and host-memory admission

Status: repair and diagnostic gates in progress; no sealed campaign resume,
no migration certificate, and no onset verdict. Work is isolated on
`codex/r220-host-memory`; the r217 checkout and frozen executable are untouched.

## Attribution

r219 established a kernel memory-pressure kill, not a concurrent qualification
kill. The short frozen tier-eight prefix measures accepted-step RSS at
1,579,106,304; 1,702,739,968; and 1,834,958,848 bytes. These are process RSS,
not the tracked-owner device allocation counter or the kernel's compressed
process report.

`OBJC_DEBUG_MISSING_POOLS=YES` supplies the allocation-class witness: the frozen
three-step prefix emits 6,330 missing-pool reports, including 1,878 compute
contexts, 338 blit contexts, and 150 buffer reports. The two top-level
projected-Heun entry points did not establish an autorelease pool. ARC alone
does not retire autoreleased Metal objects when called from this C++ CLI.
Completed command/encoder resources could therefore outlive an entire step.
Both entries now drain their own pool after the owner and completed commands
have published CPU-owned results. No shader string, solver expression,
projection tolerance, or source-field byte construction changed.

The second review caught an exception-only lifetime defect: an autorelease
pool is not drained when an exception crosses its boundary, even if an outer
catch recovers ([Clang ARC specification](https://clang.llvm.org/docs/AutomaticReferenceCounting.html#autoreleasepool)).
Both owner entries now use one catch-inside-pool recovery function. Its fault
injection observes a live autoreleased sentinel, throws `std::bad_alloc`, and
requires the sentinel to be released before the boundary returns false. The
old-order mutant retains the sentinel until an enclosing fixture pool drains,
so the test witnesses the actual compiler/runtime distinction, not only source
ordering. Failure while formatting the allocation error also returns through
the pool normally.

The sibling audit identified 20 standalone remap/comparator/diagnostic wrappers
with the same pattern. They now enclose their existing try/catch in an outer
pool; four allocation-error reporters also catch a secondary allocation failure.
These wrappers are not extra live projected-Heun callees: the owner directly
encodes resident stages. Context/capture pools reached by the live owner are
nested under its new recovery boundary. This is a lifetime-only sibling repair,
not a change to the qualified numerical kernel set.

The matched repaired prefix has identical input roots, publication roots,
kernel-set SHA and exact accepted times at all three steps. Missing-pool
reporting drops to 226 references to **one** `AGXG16CDevice` singleton; the
growing encoder/buffer classes disappear. This is not a claim that all
Objective-C missing-pool sites in the repository are repaired.

The executable identities, both protocol files, both full trajectories, and
the non-timing comparison are bound in
`docs/evidence/fire/r220_prefix_comparison.v1.json`; reproduce it with
`python3 tools/fire_host_memory_evidence.py rendered/fire_production_calibration/r220_host_memory`.
Only the four explicitly named timing columns are excluded. This closes the
first review's P2: RSS-file hashes alone did not bind the numerical comparison.

The 24-step repaired tier-eight prefix completes. After warm-up its RSS is
2,055,815,168 bytes at step 6 and 2,077,638,656 at step 24; footprint is
2,239,924,432 and 2,261,551,336 bytes respectively. This is bounded-prefix
evidence, not a claim that allocator residency is bit-constant or an empirical
fire verdict. No scalar histories, ledgers, checkpoint bytes, or source
records were dropped to obtain the reduction. Existing scalar histories
remain; full field/packet histories are not newly retained by this change.

## Host admission is separate from numerical acceptance

The launcher records RSS, physical footprint, and compressed bytes per accepted
step in `budgets/host_memory_trajectory.csv`, alongside the unchanged velocity
trajectory. Footprint participates in admission so compression cannot make
retained memory invisible to an RSS-only check. Unavailable measurements fail
closed.

Before stepping, `host_memory_admission.v1` and its v2 sidecar bind the case,
producer build/executable, beginning step/time, both byte ceilings, reserve,
stop rule/code, and the no-build/Opto/exhaustive-qualification/oracle-regeneration
host rule. The receipt distinguishes a synthetic or forced-stop fixture from
a production run. Host exclusivity is operator/launcher discipline, not a
claim of an OS-wide lock preventing another application from starting work.

The resource budget is `min(physical host RAM / 2, 32 GiB)`, reserving the other
half of this 64-GiB host for the OS and other processes. The early-stop reserve
is the observer-mode owner device certificate plus 2 GiB of host/checkpoint
scratch budget. At tier ten these are 34,359,738,368 and 13,449,641,984 bytes.
They are resource limits, not physical/numerical tolerances. The first 24-step
instrumentation trial used a more restrictive quarter-RAM budget; its artifact
records that experimental value and is not rewritten.

At an accepted boundary approaching the limit, the same checkpoint writer
publishes the accepted state, seals the run evidence, and the onset launcher
returns named code **97**, `host_memory_ceiling_checkpoint_sealed`. Publication
or sealing failures do not receive that success-of-checkpointing label.
Exact threshold, footprint-only threshold, unavailable measurement, and
invalid-budget REDs exercise the admission rule.

The synthetic-retention CLI is a diagnostic-only from-zero tier-eight fixture:
after warm-up it deliberately retains 16-MiB blocks and uses a small local
budget. It cannot be selected by a sealed replay or a resumed run. Its first
trial trips at step 5, writes a valid sealed final checkpoint, and exits 97.
Synthetic memory-budget values have no production authority.

This boundary guard is not a claim that arbitrary external memory pressure,
an OS termination, or an unaccounted within-step allocation can never kill a
process. The operator's within-step allocation certificate and the owner's
host-exclusivity rule remain necessary. Full builds, Opto/LTO links, exhaustive
qualification, and oracle regeneration remain serialized with sealed runs.

## Resume provenance, still not invented

The new `--fire-r220-resume-probe` admits only retained step 835, SHA
`4b5bfcbd02ec7c5f472352f739b1b39ee7157b871896efb8b2dc35e708bd8d64`,
under its original build and kernel-set identities. It executes exactly eight
diagnostic steps and cannot authorize migration or overwrite that checkpoint.

The eight-step diagnostic reaches step 843 at 1.3674342863960192 s. RSS is
2,031,763,456 bytes at step 836 and 1,928,003,584 at step 843; the latter
footprint is 2,109,933,944 bytes, compressed bytes zero. Its trajectory is
diagnostic-only and does not supply the missing old-executable continuation.

The first review found a P1 in forced resource checkpoint routing: an isolated
resume's input could have been selected as a mutable destination. No original
checkpoint was overwritten. All isolated probes now exclude their input from
periodic or forced publication; resource stops choose a new immutable
`host_memory_stop.checkpoint` in their diagnostic output directory. The
forced-stop RED runs on a disposable published copy of 835: it exits 97 after
step 836, verifies the new checkpoint and its full v2 sidecar, and checks both
input-file and input-sidecar SHA identities unchanged. This covers the sibling
EOS/certificate probe routing, not just the new entry point.
Its negative-test mutant deliberately restores the unsafe destination and
mutable publication only on a second disposable pair: the test returns 93
with `input_and_sidecar_unchanged=0` and `passed=0`. The mutant is not retained
in production source, and the original input and golden checkpoint remain
unchanged. No claim rests on a regression that merely passes after the fix.

There is no historical eight-step continuation **after** step 835: the old
process ended there, and its frozen binary has no admissible step-835 resume
entry. Neither a newly rebuilt old-arithmetic binary nor the new probe may be
mislabelled as execution by the old frozen executable. Consequently an r78 v2
certificate anchored at 835 is not yet issuable.

Retained step 826, SHA
`f18cac44b71b1fdac106d30d86bcc1efb955ed39ed4edc0a66be912a4130c337`,
has historical steps 827–835. The owner has been asked whether to certify that
earlier checkpoint and replay through 835. No earlier-anchor substitution,
certificate weakening, or production continuation has been performed.

## Evidence and remaining gates

`docs/evidence/fire/r220_host_memory.v1.json` binds the local diagnostic files.
Large logs, checkpoints and executables stay ignored under `rendered/`.
The normal CPU suite and resident target/r190 owner gate pass; warning-clean
native, Deployment and Opto builds pass (only documented OIDN/AppIntents noise).
The updated synthetic-retention RED exits 97 at step 5 with two retained
16-MiB blocks and a sealed checkpoint; their contents are observably hashed.
The standalone host-policy ASan/UBSan gate checks one million randomized
cases against an independent wide-addition predicate. This is launcher-policy
sanitizer coverage, not a claim of sanitizer coverage of Metal kernels.
The final v3 CPU and resident target/r190 owner gates pass after safe routing
is restored and the admission receipt added. Their exact tested source hashes
are recorded separately from producer build/executable identities; a later
commit does not retroactively change which binary ran. The subsequent v4 CPU
and owner gates pass after the exception-family repair; Deployment/Opto v2
clean builds pass with only the same documented warnings. The shared
allocation-failure RED passes both in the native binary and an exact-source
standalone ARC/ASan/UBSan fixture. The final three-step comparison is bound in
`docs/evidence/fire/r220_exception_prefix_comparison.v1.json`: all non-timing
fields still equal the frozen execution. The executed native binary is retained
under `executables/FireSequenceTest_52120712`, SHA
`5212071242aa6c1aff0625c12f0045441696137ad105114bddb949c2a5b60c0d`.
Fresh review and migration remain pending at authoring of this exception-fix
commit; the earlier review evidence is retained, not relabelled as this build.
Main's dirty Xcode project, the golden checkpoint, and the
deferred r217 branch rewrite are outside this edit.
