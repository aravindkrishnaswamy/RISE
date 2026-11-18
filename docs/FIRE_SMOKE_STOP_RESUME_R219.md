# r219 — memory-pressure stop, host exclusivity, and resume-entry audit

Status: kill cause confirmed; restart not launched. The retained checkpoint
and frozen executable are unchanged. The owner's authorization for a plain
same-binary resume is recorded, but that executable has no admissible
step-835 projected-owner continuation entry point.

## Cause: kernel memory-pressure kill, not inferred from exit 137

The requested unified-log search was performed, followed by the bounded
2026-09-08 22:25–22:42 PDT interval. The decisive kernel entry is:

```
2026-09-08 22:34:25.846 Df kernel[0:22b] [com.apple.xnu:memorystatus] memorystatus: killing largest compressed process FireSequenceTest_5cba6861 [91310] 142048 MB
```

This attributes termination to the kernel's memory-status subsystem. It does
not identify which application allocation accumulated, establish a leak's
location, or justify calling the reported 142048 MB an RSS measurement.
The original execution session returned 137. No numerical refusal, retry,
or crossing was published. The last accepted state is step 835 at
1.356785726849921 s, maximum velocity 8.585746765136719 m/s; peak recorded
velocity is 9.398045539855957 m/s at step 777. The 2.131 s examination point
was not reached; no onset or window authority is claimed.

The retained log ends at 22:33:20 PDT. Qualification file completion times:

| Activity | Completion, 2026-09-08 PDT |
|---|---|
| Clean make all | 21:11:06 |
| FireSequenceTest link | 21:12:24 |
| Deployment | 21:14:32 |
| Opto | 21:17:30 |
| Restored-fixture CPU suite / count gate | 21:18:32 |
| Resident owner/target qualification | 21:19:30 |

These activities overlapped the sealed run earlier, which is now prohibited,
but ended approximately 75 minutes before the kill. The evidence does not
support saying a concurrent Opto link caused this termination.

The trajectory's **tracked owner allocations**, not process RSS, were:

- Peak: 5,821,808,640 bytes, at accepted step 683.
- Last accepted step: 5,820,104,704 bytes.
- Same-scope owner working-set certificate: 10,770,268,160 bytes.

These counters do not account for the kernel's process-wide memory figure.
That accounting gap remains an explicit follow-up; host serialization alone
is not evidence that process memory is bounded.

## Binding host execution discipline

While a sealed simulation is live on this host, do not run full builds,
Opto/LTO links, exhaustive qualifications, or oracle regeneration. Serialize
those jobs after the sealed run's verdict. Light CPU-side filtered-contract
analysis is permitted. Before launch, check the process inventory; after
launch, the monitor checks it for conflicting heavy jobs and reports a
violation rather than killing unrelated processes. Record process-memory
observations separately from the device/owner allocation certificate.

The pending launch receipt is
`docs/evidence/fire/r219_resume_launch_receipt.v1.json`. It records
`launched=false`; it is not a fabricated continuation identity. Existing run
records, protocol bytes, checkpoint bytes, and historical seals remain intact.

## Same binary is verified, but a same-binary entry point is absent

The executable SHA-256 remains
`91978041bb53124e45324838a082d6fb64bb0f2a6da28c68ddc7e8c186faaefa`.
Checkpoint 835 remains
`4b5bfcbd02ec7c5f472352f739b1b39ee7157b871896efb8b2dc35e708bd8d64`.

The r214 sealed tier-ten entry calls `Tier10OnsetScopeAccepted`, which requires
`continuation == false`; it additionally rejects
`RISE_FIRE_ONSET_RESUME_CHECKPOINT`. The frozen binary's existing scope RED
passes and explicitly confirms the resume refusal.

The r216 continuation command instead requires its old checkpoint SHA
`65bb74aa8186a95acde57de593d54de664303c4d92ebaea374eea75e1f138c19`,
accepted step 792, the r216 kernel identity, and a v2 migration certificate.
Passing checkpoint 835 to that frozen entry returns 91 with
`RESIDENT_CONTINUATION_REFUSED`, before creating the requested output
directory. No simulation was launched by this admission check.

The temporal production entry also explicitly refuses resumed ported
transport (`production_transport_resume_requires_operator_lineage`). The
generic onset/momentum diagnostic uses a different diagnostic operator and
is not an equivalent escape hatch. None was substituted.

This is a control-plane capability gap, not a claim that an unchanged
operator mathematically requires migration. Adding that capability requires
a new executable, contrary to the current identical-binary constraint, and
therefore requires owner direction. No binary patching, forged checkpoint,
relaxed lineage gate, numerical change, branch rewrite, or resume occurred.

The local bounded system log SHA-256 is
`46eabadf70c127f83ccd3012f23a38242a647985435dd4f4d2c63595873a96dd`
at `rendered/fire_production_calibration/r219_stop_resume/kill_window.v1.log`.
Only this record and the small receipt belong in git; checkpoint and binary
payloads remain local. Monitor remains paused until a real qualified launch.
