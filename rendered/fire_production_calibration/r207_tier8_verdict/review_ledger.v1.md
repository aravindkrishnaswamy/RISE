# r207 crossing prerequisite — review record

The numerical executable compiled from `357c93369ff90c451f5c2171a59e18396c557f59`
is frozen as `FireSequenceTest_r207_sealed_v3`, SHA-256
`aebfd7915a1e8235ce6e921deb66c78b057cafb4188d6c098b7eee813b64580f`.
The full test suite completed with exit 0 before launch; see the exact log
and qualification hashes in `launch_receipt.v1.json`. No replay verdict is
claimed by this qualification record.

## Round one

- Numerical/lineage and evidence axes: zero P1, two P2 findings.
- Fixed: the older Python qualification helper ignored the eight new
  crossing REDs. The r207 checker now requires them and both positive
  rerun/writer verdicts; the historical six-RED helper is unchanged.
- Fixed: terminal EOS demand was mislabelled as executed restoration.
  Diagnostic capture now persists the last target increment actually
  consumed by each projection. The projection is labelled combined, and
  no separately measured tail impulse or realized drained volume is claimed.
  Two regression witnesses distinguish consumed requests from terminal demand.

## Fresh round two on 978a5930

- `r207_round2_lineage`: zero P1/P2 after checking the immutable protocol.
  Initially reported early 60 m/s / missing-reference observation as P2;
  retracted because r194 explicitly requires that case to stop without a
  verdict. No exception is introduced. Pending raw diagnostics do not confer
  a shortened-prefix verdict.
- Confirmed production trace-disabled arithmetic and allocation unchanged;
  the diagnostic rerun does not publish, its terminal result and authorities
  must match the accepted step, actual consumed targets are captured, and
  crossing aliases are transitively SHA-bound.
- `r207_round2_evidence`: zero P1, two P2 and one P3 in the offline checker.
  Jointly edited/rehashed trace and CSV could hide missing faces/invalid
  column geometry; replaced positive target IDs could be accepted; device
  timing and staging metadata were incompletely checked. These are checker
  issues, not changes to the executed device trajectory. The exact executed
  campaign record is being pinned and the demonstrated mutants added before
  final qualification sign-off. No arbitrary rehashed record is an execution
  authority.

The synthetic device fixture's consumed tail fields are zero. Its helper
REDs prove the estimator/consumed-request semantic distinction, not a nonzero
burning-tail device experiment. The live replay is the requested physical
measurement and remains untouched while CPU-only checker repairs proceed.

## Fresh round three on 500dd40e — closed

`500dd40e7a8678144cabcc02d54898697b9f89a1` pins the exact-byte executed
v3 raw-log SHA as the qualification authority, rather than issuing new
execution authority to a caller's rehashed record. It additionally validates
complete column geometry/trace blocks and event device/staging metadata.
All sixteen CPU test groups pass. Four rehashed exploit tests independently
fail against `978a5930` because that older verifier accepts the mutants, with
no setup errors. The numerical executable remains unchanged.

- `r207_round3_evidence`, mutation integrity and transitive bindings:
  **zero P1/P2**, independently re-ran all sixteen tests and the four old-code
  REDs; actual v3 fixture passes and synthetic scope is explicit.
- `r207_round3_contract`, execution provenance and cost/claim boundaries:
  **zero P1/P2**, independently verified the immutable log, executable,
  qualification, full-suite and protocol bindings, exact-byte loading and all
  sixteen tests. No arbitrary future self-rehashed execution is admitted.

Round-two checker findings are fixed and freshly reviewed. The retracted
missing-reference finding remains a sanctioned no-verdict limit of r194.
This closes the crossing qualification prerequisite, not the running physics
experiment. New r207 Xcode clean Deployment/Opto builds have not been run;
the launched standalone Metal executable has the forced clean make build,
independent r190 owner gate, publication gate and full-suite evidence above.
