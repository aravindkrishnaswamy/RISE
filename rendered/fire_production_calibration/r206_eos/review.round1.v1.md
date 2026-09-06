# r206 fresh review, round 1

Reviewed HEAD: `07f33927`; numerical source: `20607aea`.
Independent reviewers ran sequentially because the agent service refused a
second simultaneous reviewer (`agent thread limit reached`). Neither edited
files or ran builds/GPU workloads.

## r206_round1_numerics

Verdict: no P1/P2 findings. Reviewed exact four-word endpoint reuse, density
product/sum ordering, cold lattice/midpoint search, sticky failure before
publication, private same-command cache lineage, allocation certificates,
sibling callers, actual 0x80 mutation refusal, and the separate binary64/r190
reference paths. The possible fewer-than-14-cells concern was ruled out by
the existing minimum 4x4x4 shape gate.

## r206_round1_evidence

Verdict: no P1; three P2 findings, confirmed and fixed before round 2.

| Finding | Concrete failure | Repair / RED |
|---|---|---|
| Missing counters become success | Delete bit-mismatch/refusal/overflow tokens; parser reports zero | Required exact record schemas; delete each mandatory verdict and reject duplicate counters |
| Repeated evidence counts as independent | Redirect repeats 2/3 to repeat 1; reports three samples and SD=0 | Distinct resolved paths and log SHA checks; copied-log and repeated-path REDs |
| Owner field coverage is not pinned | Rename T to NOT_T and set words=0; 41-name check still passes | Canonical names and geometry-derived extents, iteration-bound residual lengths; rename/drop/zero/short/long every field plus duplicate/malformed verdict REDs |

The reviewer independently reproduced the saved measurement JSONs and
confirmed hashes, units, histogram totals, process statistics, p95, cold-prefix
scope, dirty-tree warm-prototype disclosure, and the unmet whole-owner target.
The saved evidence was complete and distinct; repairs close false-green
acceptance paths rather than changing measurements or numerical tolerances.
