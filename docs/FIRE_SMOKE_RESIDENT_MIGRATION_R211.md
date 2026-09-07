# r211 — resident r78 equivalence, certificate v2

Owner-authorized extension of r78, not a numerical change. Qualification and
fresh independent review are required before the continuation is launched.

## Claim and derivation

The preserved r208 resident binary does not emit the oracle-only v1 runner's
Tmax/EOS/VDB trace. It does emit the full accepted device-payload Merkle root
and exact binary64 accepted step times. Certificate schema v2 therefore claims
equality of full v2 input and publication roots at each equal accepted time,
for N >= 8 consecutive accepted steps from the identical checkpoint. It also
requires exact dt and kernel-set identity. No inferred Tmax or fabricated
frame evidence substitutes for a missing historical observation. This is a
finite continuation-equivalence certificate, not a proof for all future time.

The CBOR envelope/encoding remains RISE-CBOR64-v1; the certificate record kind
and schema are explicitly v2. The existing r78 certificate-v1 parser, evidence
requirements and historical bytes remain unchanged. v2 is admitted only for
the resident projected-Heun owner, not for the CPU oracle or old remap path.

## Executed provenance

`tools/fire_resident_resume_v2.py` launches both binaries sequentially from
separate copies of the retained checkpoint. Its historical adapter pins the
r208 executable `70eca63d…7db9` to build `a3ef3563…e1f5`, as recorded in the
original r208 from-zero identity. The new binary reports its current build and
executable hashes itself. Executable and checkpoint hashes are checked before
and after execution. Output directories must be fresh; inherited fire mutation
or diagnostic environment variables are refused. The old diagnostic's exit 93
is not relabelled success: the adapter also requires its explicit solver-success
terminal record and exactly eight actual accepted CSV rows. The new strict
certificate diagnostic must exit zero.

The certificate retains both traces separately. Its loader checks every row
against the SHA-bound raw CSV, all exact times, digest version, input/output
roots and executable files. The raw log must bind the same checkpoint,
beginning time and starting step and report successful unchanged-checkpoint
termination. Modern trace logs also self-report the executing build/binary;
the sole historical exception is the SHA-pinned r208 adapter above.
Top-level build/executable identities must equal
the trace identities; the two traces must agree step for step. Publication is
through the executed issuer, not a CLI that accepts hand-authored trace arrays.
As in r78 v1, this is a repository evidence certificate, not a cryptographic
signature by an independent attestation service. The executed issuer and sealed
evidence are the provenance authority; a self-hash is not a signature.

`bridge.v2.json` records both executed commands, statuses, binary hashes,
checkpoint SHA and certificate SHA. It bridges the current r208 lineage to v2
without rewriting historical evidence. The certificate's absolute evidence and
executable paths are dependencies: historical verification requires those
byte-identical files to remain available at the recorded paths. Relocation is
not silently accepted.

## REDs and scope

The actual C++ loader is exercised with synthetic, explicitly non-production
fixtures: every step's root mutation; a final-root mutation with re-sealed raw
CSV; fewer than eight steps; one-bit time mutation; changed binary, forged
top-level and trace binary/build identities; missing arrays and wrong versions.
Existing v1 loader/migration REDs remain in the full FireSequence suite.
No tolerance or solver arithmetic changes.

The sealed continuation entry accepts only checkpoint
`2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2`, step 1300,
under an exact-current-binary v2 certificate. It makes a new working copy,
keeps periodic checkpoints, and writes a continuation identity rather than
reissuing a from-zero identity. It retains the original 3.0-second *case
identity* while extending the independently controlled observation horizon to
3.5 s or the first 15 m/s crossing. This does not shorten a statistics window
or authorize empirical rows. At a crossing, the existing full source/budget/
convergence bundle is mandatory. The monitored policy, production working-set
certificate and qualification observer certificate are unchanged.

Baseline: 63.96 m/s at 2.699 s; aligned advection 11,692. The oracle comparison
is the r193 exact-equal-time 159.01-class criterion. Raw column advection at a
different time is not that comparison. Completion of a stable prefix alone
must not be labelled a fully passing operator/readmission verdict.

## Cost and remaining work

r210's retained-state measurements were roughly 17–19 device seconds and
19–21 wall seconds per step. They are not the earlier cold 3.24 s/step number.
The approximately 846 remaining CFL steps to 3.5 s would cost roughly 4.5–5
wall hours at that rate, excluding crossing observations and future iteration
growth. The run stops earlier if a crossing occurs. No production-speed or
window claim is made by this migration work.

## r211a — executed first-checkpoint handoff repair

The first certified continuation admitted step 1300 and accepted five steps
through 2.1162557490170002 s, then refused its first periodic checkpoint.
The log reports checkpoint authorization, timeline, observation and state
matching all true; publication refused an incomplete prior payload/seal pair.
The initial cursor had been copied with the generic raw-file helper, which
omits the v2 sidecar. The failure was not a physics or commuting refusal.

The resident import now validates the original published pair and stages its
exact raw checkpoint bytes through `PublishPreparedPayload`, creating a complete
pair at the fresh mutable cursor. The source bytes/sidecar are not edited and
no state is reserialized. First replacement uses the unchanged publication
gate. The named `RESIDENT_CURSOR_IMPORT_RED` proves bit-identical import,
successful subsequent replacement, original bytes/seal unchanged, and refusal
of unsealed/foreign-case inputs. The old bare-copy mutant reproduces the exact
executed refusal on first replacement. A test requiring this marker is RED
against the prior executable, not merely inferred from code inspection.

The first failed directory, certificate and launch receipt remain historical
evidence. The repaired executable requires a fresh executed N=8 v2 certificate;
the next run starts from the original step-1300 checkpoint in a fresh directory,
not the five-step in-memory prefix that was never checkpoint-published.

Sibling audit: the remaining generic durable-copy callers export immutable
VDB frames or a sequence manifest. None initializes a mutable resident
checkpoint cursor or later passes the copied path through mutable payload
replacement. The generic helper is now labelled with that limitation; the
downstream pair-validation and replacement gate remain unchanged.
