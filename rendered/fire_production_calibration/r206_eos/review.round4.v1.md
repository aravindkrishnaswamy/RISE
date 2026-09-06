# r206 fresh review, round 4

Reviewed HEAD: `2780f93a`; numerical source: `20607aea`.
Reviewer: `r206_round4_evidence`; CPU-only, no edits/builds/GPU jobs.
Verdict: no P1; two P2 findings, confirmed and fixed before round 5.

| Finding | Concrete failure | Repair / RED |
|---|---|---|
| Legacy cost CLI bypass | Failed completion or duplicate outcome hash still generates report | Centralized completion gate called by CLI and all prefix consumers; actual CLI mutants must refuse before output |
| Instrumentation verdict presence | Passing record plus failed FP64/convergence record is admitted | Exactly one successful record per verdict tag; duplicate/conflicting records tested against helper |

Shared metadata parsing additionally rejects malformed and duplicate identity
keys. Six r206 CPU tests, eleven r205 regressions, producer/profile and cost
self-tests pass. Historical-consumer REDs are retained beside this ledger.
The reviewer independently reproduced both measurement SHAs and current/baseline
executable hashes and confirmed unchanged numerical source and honest scope.
Fresh post-fix review is required on the committed repair.
