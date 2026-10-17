# r223 — measured migration across a qualified EOS kernel transition

**Disposition: proposal withdrawn after measurement. No migration certificate
was issued. The transition verifier and launcher changes were removed; r78's
strict historical verifier remains unchanged.** The pre-implementation proposal
below is retained as the rejected-candidate record, not live authority.

## Amendment before launcher implementation

r222 repairs a previously unresolved EOS interior order without changing any
previously accepted arithmetic path. Its frozen strict-calibration build passed
the captured-cell and boundary mutants, independent energy enclosures, complete
Metal/fp64 kernel sweep with the reviewed r190 owner, and full CPU suite. Two
fresh read-only reviews returned zero P1/P2. Numerical qualification record:
`docs/evidence/fire/r222_qualified_repair.v1.json`, SHA
`cd56726d058cafda58c3c87a0e1d4639f1d94d6501bdab35e0b9c0da0ad91e2f`.
Whole-project clean shipping-build checks are still pending, not claimed here.

The old r78 v2 implementation additionally required equal kernel hashes across
both traces. That is appropriate for launcher-only changes but cannot express
this qualified formulation transition. The equivalence function is exact
accepted time and full input/publication payload equality; implementation
identities must be authenticated individually, not made equal by declaration.

Preserve v2's old form and verifier unchanged for historical certificates.
An optional, explicit `qualified_kernel_transition` record admits **only** the
following measured numerical lineage, and binds the qualification record's
actual bytes as well as its SHA:

- old: `e938373ec6cd9e0217c04242ad96cd4d46055236e422bd7ad0ea34e164d0a8b2`;
- new: `e246730c19b96cbb3e1db7abe072fa271541efe1594a990f4189e71bc6d0e8ea`.

Each trace must contain its own pinned kernel at every step. Without this
transition record, different kernels still refuse. No caller-authored new
kernel, qualification record, or changed root becomes authority by re-hashing
it. Both executables, source checkpoint, raw transcripts, exact times and all
payload roots remain bound. At launch, a direct native eight-step execution
must reproduce the new trace, including its kernel identity, before any onward
production run. Occupied/preauthored output cannot skip execution. An altered
root, time, executable, kernel, qualification record, or N < 8 refuses.

## Anchor and production continuation

Step 1952 has only three historical successful following steps, so it cannot
alone support N >= 8. Use the nearest earlier retained checkpoint, **1945**,
time 3.0205594031140208 s, SHA
`a47d7cd11260749e67488ae4b50552a091acd8cb43263450b328d1ca0a11839b`.
Compare new steps 1946–1953 against the SHA-pinned r221 continuation transcript.
That old transcript begins at step 827, not zero; its offset, initial time,
extent through 1955, native-launch receipt, producing executable/build, and
terminal log are authenticated explicitly. Do not manufacture an old replay.

After a passing certificate and mandatory fresh native overlap, production
replays from 1945 through the retained 1952 state and continues toward 3.5 s
or the first 15 m/s crossing. This reversible earlier anchor implements the
authorized resume under r220's operational judgment; it does not shorten the
horizon. Checkpoints 1945/1952 and original binaries remain untouched. Source
packets, monitored policy, conservation, outlier bound, RSS ceiling/graceful
stop, host serialization, and crossing observer remain unchanged. This entry
does not claim that the eight-step comparison has run or passed yet.

## Measurement falsified the proposed bridge

The frozen probe from source `0761fca3` accepted all eight steps 1946–1953 and
left checkpoint 1945 unchanged. Exact times and step sizes matched, but **all
eight input roots and all eight publication roots differed**. Evidence:
`docs/evidence/fire/r223_refused_migration.v1.json` binds both transcripts and
the frozen probe executable. No claim of equal full numerical fields follows
from the matching velocity/manifold summary columns alone.

The cause is in the payload's function, not a hash defect: its input descriptor
contains the qualified kernel-set SHA; terminal publication includes 22
authority identities plus the input digest. The repaired kernel therefore
changes full payload identity even along unchanged accepted arithmetic paths.
The proposed root-equality migration cannot express that transition. Dropping
these identity bytes from comparison or calling unlike roots equal would
break the stated provenance claim. Neither is done.

Operational decision under the expanded charter: retain the qualified r222
repair, withdraw this unused bridge, and use from-zero production. Prioritize
the tier-8 full window rather than another long tier-10 prefix before the
deliverable hedge. The tier-10 3.5 s record remains pending; survival through
3.037 s is not relabelled as that horizon. The historical checkpoint, golden
checkpoint, original run evidence, and frozen executables remain untouched.
