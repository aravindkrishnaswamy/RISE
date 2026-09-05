# r205 review repair ledger

Implementation source: `3a732a00fda8beea33570efa7fade8b9166c1f89`.
Final executed build/owner gate: `qualification.final.v1.json`.
Final complete suite: `full.fixture.final.attested.v1.log` (exit 0).
Eleven CPU tests pass; several tests contain multiple named mutants.

| Round | P1 | P2 | Confirmed issues and disposition |
|---|---:|---:|---|
| 1 | 4 | 4 | Immutable publication bytes, mutable policy overreach, orphan/empty sidecars, stale build cache; bridge/isolation REDs and cost accounting gaps. Fixed at bfca0246. |
| 2 | 1 | 4 | Inherited make flags; mutable advancement/real-loader REDs, pending publications, v1 relabelled as v2. Fixed at b97f46c9. |
| 3 | 1 | 1 | MAKEFILES dry-run escape; restored bridge framing assertion. Fixed at 7dc6bbf5. |
| 4 | 1 | 1 | Compiler diagnostic flags preserve stale objects; successful exit without actual owner verdict. Fixed at ab5e7d78. |
| 5 | 1 | 0 | Ignored local build configuration. Fixed at 60753669; explicit Makefile and no cached dependency recipes also RED-proven. |
| 6 | 1 | 0 | Vendored and wildcard-selected compilation sources absent from admission. Fixed at 904c57b8, with ignored-source sibling at bf4879ad. |
| 7 | 1 | 0 | Build directory include-search root omitted. Fixed at 01985f35; Git execution-context sibling fixed at 3a732a00. |
| 8 | 1 | 0 | CPU RED depends on campaign log not yet committed. Closure is publication of the exact existing logs in this evidence commit, followed by a clean Git-export test. |

Total: 11 confirmed P1 and 10 P2 findings repaired. The frame-fsync concern
was rejected after tracing the already-present durable sync, not counted as a
confirmed defect. Fidelity/cost reviewers in rounds 5 through 8 independently
reproduced the historical bridge and cost analyses and found no P1/P2.

This ledger is not a final zero-P1 verdict. The next fresh round reviews the
committed evidence package and its clean-export test; its verdict is published
separately so this immutable inventory need not be rewritten.

No numerical expectation, tolerance, owner arithmetic, or golden checkpoint was
changed by these review repairs. No EOS speedup, affordable owner, fixed-k,
onset/replay, full window, or new movie is claimed.
