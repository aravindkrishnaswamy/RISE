# r229 — measure the changing momentum before choosing k

The r228 cost step is promoted, but 7.17 s/step is not production speed.
Integrate only the diagnostic source/test portions of the staged r214
changing-momentum exporter (`e61418ec`), preserving all later EOS,
projection-recovery, memory and target-basis changes. Do not import historical
artifact deletions or obsolete certificates from that branch.

The observable is resident `Stage::nextMomentum` in R0/R1, not the frozen
provisional projection input. R1's supporting advection must use R1's own
shared alpha. Bootstrap and R2 absence mean unavailable, never zero settling.
Only observer packet accounting grows; production buffers and arithmetic
remain unchanged. This is diagnostic instrumentation needed to select and
gate the already-authorized fixed-k variant, not a new production authority.

Run current CPU exporter/mutation tests, generated-mirror checks and resident
owner/residency gates. Capture real hot-state per-iteration changes with
the smallest existing nominated-observation path, serialized with builds.
Actual settling nominates k; only per-quantity full-owner comparisons under
the r201 contract may qualify it. No tolerance changes, pooled ratios or
zero frozen-input deltas substitute. No k or speedup is claimed in advance.

Historical checkpoints used as isolated qualification inputs remain sealed;
this is not production migration. Future kernel-changing production runs
start from zero. No kernel-transition certificate will be built. Retain raw
diagnostics outside git, commit SHA records only, and cap live observer memory
with the existing observer-mode certificate and RSS monitor.

## Current integration (not yet qualified)

Applied the historical diagnostic source hunks only, regenerated the fp64
and roundoff-trace mirrors from current source, and retained the current
production working-set value (5,616,254,976 bytes at tier 8), not r214's old
5,584,011,264. Observer packet arithmetic alone changes from 36 to 38 face
planes. No historical payload deletion was applied. Current Metal kernel
text and target-basis arithmetic are unchanged by this diagnostic port.

Seven CPU exporter/mutant tests pass under their ASan/UBSan harness. Sixteen
historical v1 crossing tests pass using the preserved r203 worktree's actual
`r207_tier8_verdict/crossing_gate.v3` artifact. Their initial default-path run
failed because payload files are absent in this worktree; no fixture or
expectation was changed, and the failed log remains retained. Both generated
mirror `--check` commands pass. The focused linked FireSequenceTest build
passes without warnings. The live owner gate and native v2 export fixture
both pass. The latter re-derives production-versus-observer terminal/stage
identity, stale next-momentum and R1-alpha advection binding REDs, and
production-scope refusal. Its small synthetic inputs are fixture-only; this
is not a real hot-state settling measurement or a new-field all-domain error
certificate. Real capture, independent review and any fixed-k decision remain
pending. No historical measurement qualifies those surfaces by assertion.

Logs under `rendered/fire_production_calibration/r229_momentum_settling/`
remain outside git:

| Artifact | SHA256 |
| --- | --- |
| cpu_mutants.v1.log | 635f0a917891ec636a2fcef026a3c6628b08c9d7702bd8f26b9d9cc7e426215c |
| historical_v1.v2.log | 89850277853bb5af5e954f2999429c474781f96652d3c084f63016fa08c842e8 |
| build.v1.log | 5ba7ecc71fd4b5aaa972a34c3c89be8ac8b6b7e72c75e8b464802b60fe2c9f67 |
| owner_gate.v1.log | 9efb773d173a9a14669870f0400234a4e64437ae7d36794429b8396226507303 |
| convergence_fixture.v1.log | 096b61afa6fdf181a4fdee3e64b03209ed9c63e214e1e2f3e3e2476b52257be5 |
| convergence_fixture.v1 (v2 content) | 663063b969e98e3a23afb0d51e153ba147296a1a54d2777528ef51a077cc20a9 |
| convergence_fixture.v1.csv | 9fd72182968045a55f36709b8647256cd91941b2cc431c9578823159ed52584e |

Qualification executable SHA256:
`f2d13fa9a0f5d64a75a6359b11b743c21349df5f6623463c7f2f91480cd5e338`.
Self-audit: sampling the frozen projection input would falsely imply settling;
using R0 alpha for R1 would misidentify its producer; omitted packet fields
would violate allocation accounting; absent R2 momentum must not read as
zero. These are the explicit mutation/identity and review axes. Full clean
platform rebuilds have not yet been repeated for this diagnostic change.

## Hot-state observation and first review

The existing isolated certificate-diagnostic command accepted eight steps
1301–1308 from the SHA-pinned r208 step-1300 input, with input checkpoint
unchanged and migration_authority=false. At the nominated column (38,42),
the actual R0 next-momentum maximum successive changes over iterations 2–5
are 0.0006185174, 0.0004903972, 0.0004917979 and 0 kg m⁻² s⁻¹.
R1 iterations 2–3 give 0.0003036857 and 0. R0 velocity changes remain
0.05175430, 0.04633445, 0.04636818 and 0 m/s; R1 gives 0.05072397 then 0.
R2 has no next-momentum producer; velocity changes at iterations 4–5 are
2.5868416e-5 and 4.7683716e-7 m/s. This does NOT support the expected global
k=2–3 shortcut. It nominates (5,3,5) at this state only, the same counts the
full owner already used, hence no measured iteration-cost saving. Local
error/branch-contract qualification and broader states still decide any
fixed-k variant. No convergence bound is inferred from these observed deltas.

The observer rerun had terminal bit identity and cost 8347.502 ms on this
single hot step. Host RSS stayed below 5.04 GB in the eight recorded steps;
compressed bytes stayed zero. These are diagnostic costs, not production
throughput measurements. Raw capture log SHA256:
`fa9954748578eb251b8603c2c6eb01d79a656677dc54d0f853bfd2b376897177`.
Transcript SHA256:
`72d5dfefcc2625f109f06bd452e3e183e03dabcb1ef4cb44579a8668abb0f2da`;
CSV SHA256:
`8430f149b8c6f3751e07ca456e63742cb10a34c9ca58242f1fb32d0111150630`.
Both are under `hot_capture.v1/budgets/` with basename
`reference_composition_candidate_fixed_column.raw.csv.convergence.v2`.

Two fresh reviewers inspected `af0f787b`: numerical/residency review found
no P1/P2; test review found one P2, accepted and fixed: consecutive terminal
records could manufacture a zero-delta observation. The shared serializer
now rejects consecutive terminals, while a positive fixture retains failed
terminal → next Picard → terminal ordering. Valid historical v1 bytes are
unchanged; only malformed traces lose admission. The new guard's removal
mutant fails before any binding check. Eight CPU tests now pass; log
`cpu_mutants.v2.log` SHA256
`3e914c6524ed733d6bdd9c03b4d3bb7fc351a3b0a3c92246414ccbf18010237a`.
Post-fix linked build and native convergence/owner gate pass; native log
`convergence_fixture.v2.log` SHA256
`cb09cd1fe8bdd086e1e40b146afdf22c1c24c26ee687aae856c02574ef808129`.
Fresh post-fix review remains pending; not final sign-off.
