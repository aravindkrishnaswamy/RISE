# r224 — full-window launch after the r222 repair

## Operational amendment, before implementation

r223 did not produce a migration certificate: all eight full payload roots
changed with the repaired kernel's authenticated identities. Historical roots
and the strict r78 verifier remain untouched. The reversible route is a new
from-zero tier-8 window using the qualified r222 numerical owner. Tier-10's
3.037 s survival is not relabelled as a completed 3.5 s record.

The temporal entry point must only run its complete derived window: discard
5 flow-through times, then at least 40 expected puffing periods, rounded up
to the pinned 0.0625 s frame cadence. Remove its diagnostic target override
and unauthenticated resume option; these environment variables now refuse
before creating output. The existing diagnostic CLIs remain separate.
Use the existing mutation-environment admission check as well. No solver
arithmetic, kernels, timestep rule, physical gates or tolerances change.

Enable the existing per-step diagnostic directory for velocity, accepted
input/output roots, source lineage, and RSS/footprint records. The existing
RSS ceiling and next-step reserve remain authoritative. An RSS stop seals
the partial run and returns named exit 97; it must not publish a completion
summary. A complete-window summary requires all diagnostic histories to have
the full accepted-step count, not the shortest history. A launch receipt
binds build, executable, case, full target, cadence, and host serialization.

Retain immutable checkpoints at accepted boundaries at least 900 wall seconds
after the previous publication, instead of 300. This changes only recovery
granularity: a roughly threefold cadence reduction, not an exact storage
speedup (publication cost and step overrun also enter). No old payload is deleted. Current free disk
is about 48 GiB. Frames remain streamed during statistics, not retained in
host memory; finalization also copies them into the sequence's frames directory,
so budget both copies. Monitor disk alongside RSS; never silently omit frames
or statistics. Native disk-triggered checkpoint/exit is **not implemented**.
Operational storage protection is reversible suspension of the verified worker
with SIGSTOP if free space reaches 16 GiB or projected growth threatens that
reserve. Preserve retained checkpoints and all in-memory state; resume only
after capacity is secured. This is a pause, not an exit, completion claim, or
r78 migration. The conservative disk reserve is an operational choice, not a
physical tolerance. Native RSS protection remains checkpoint + seal + exit 97.

Qualification: native preflight REDs reject shortened-target, resume and
mutation environments before any output; complete-history admission rejects
each missing channel. Re-run the CPU suite, existing owner gate, and fresh
launcher/provenance review before launch. Record exact executable and gate
hashes. No build, exhaustive qualification, oracle regeneration or render may
overlap the sealed window. Timing is measured on this run; no cold-prefix
cost is presented as the hot-window cost.

Tier 8 is a preview and a 6-to-8 spectrum comparison, never a substitute for
r57's tier-10 empirical rows. Publish centerline and display-lit-area spectra
with their measurement definitions. A persistent dominant slow mode triggers
the r54 domain/open-boundary audit before claiming the puffing physics.

## First review: publication repairs (before implementation)

Two independent reviewers found no numerical-owner change, but identified
publication defects: the temporal manifest's fixture defaults rebuilt a
one-second/16-fps case instead of the actual full-window/1-fps case; and a
missing trailing VDB bundle could pass relative-cadence checks and publish a
shortened movie. Fix the manifest's authored duration and case frame rate
explicitly and require embedded case identity equal to the actual solver.
Require the full derived snapshot count, the producer's streamed count, and
absolute coverage on the existing cadence bound before completion publication.
Pin this production CLI's cadence to 0.0625 s; diagnostic CLIs remain separate.
Add native CPU REDs for the foreign-case defaults, truncated/shifted frames,
and incomplete resource-stop publication. These are provenance/publication
repairs, not numerical or physical-gate amendments.

The native manifest RED exposed the additional schema constraint at
`FireSequence.cpp`: `delta_t_frame == 1 / case.outputFramesPerS`. Therefore
the complete-window case itself must author **16 fps**, not the launcher's
historical 1-fps placeholder. Regenerate that case identity for both the
zero-state launch probe and the actual worker, and pass the identical full
duration/16-fps recipe to the manifest. `caseFramesPerS` is consumed only by
case construction (plus diagnostic scope guards); it is not a force, source
or integration coefficient. The actual snapshot cadence stays 0.0625 s.
The 1-fps/full-duration manifest must refuse the existing schema gate;
the 1-second/16-fps fixture default must fail the case-identity comparison.

## Second review: admission sibling and carbon units (before implementation)

The final pass found a missed diagnostic environment option:
`RISE_FIRE_REFUSAL_CLAUSES` must refuse at temporal preflight, before directories
are created, just as its downstream scope guard already refuses it. Add both
present-empty and `1` native REDs. The diagnostic CLI remains unchanged.

It also found a latent render-publication unit defect: the scalar/temporal/
checkpoint extraction paths all carry carbon in **kg/m³**, while the canonical
sequence channel is **g/m³**. Convert once at the common VDB writer boundary
using the exact SI factor 1000; keep solver states and checkpoint fields in
their existing units. Test a synthetic nonzero-carbon cell through extraction,
VDB write and readback. PhysicalV1's zero soot yield masks this in the current
from-zero methane run; it does not make the reusable writer correct. Historical
payloads are not rewritten. This changes the render export, not solver
arithmetic, physical gates, or the resident kernel set.

## Qualification and release

Numerical owner/kernel remain r222's qualified implementation. The launch
source is `fec97e3a93c1b9edbd742016b9d42fc8b12af857`, clean build identity
`05bc1ed1635e977d3202e01b9ae81595ccf4cd8942065ce282ad758b94e1345d`.
Frozen executable SHA:
`d847978f8ec0eac9a08d05485aa9d0c26c0b1b83aea85ee53acc298da65b01a2`.
Its native launcher/publication REDs, complete CPU suite, and Metal/fp64 sweep
including the r190 owner all pass. Clean make and Xcode Deployment/Opto pass;
the only Xcode warnings are the documented missing OIDN checkout product and
AppIntents metadata notice. Subsequent changes were in the standalone test
launcher/docs, not the shared library or Xcode project.

Three independent two-reviewer rounds: first fixed frame completeness and
manifest identity; second fixed diagnostic preflight and carbon export units;
third (`r224_release_identity_review`, `r224_release_test_review`) returned
zero P1/P2 on the final code/tests. The storage-monitor dependency is explicitly
operational above, not represented as a native graceful-exit capability.
`docs/evidence/fire/r224_qualified_launcher.v1.json` binds the gate files.
No completed-window, spectrum, movie, or formal filtered-field verdict is
claimed by this qualification. Historical payloads and golden SHA
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`
remain unchanged.
