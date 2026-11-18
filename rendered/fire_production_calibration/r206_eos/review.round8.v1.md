# r206 fresh review, round 8

Reviewed HEAD: `82b7c894`; numerical source: `20607aea`.
Independent reviewers: `r206_round8_evidence`, `r206_round8_fidelity`.
No P1; two P2 families, confirmed and fixed before round 9.

1. EOS evidence admitted contradictory detailed counters and incomplete domain
   coverage when a caller recomputed the declared log hash. Fixed by pinning
   the trusted r206 execution supplement's exact byte SHA, plus explicit
   domain/population/runtime/core-verdict and all atomic-refusal checks.
   This fixed-campaign verifier does not issue authority for arbitrary future
   executions. Both helper and full analyzer refuse rewritten declarations.
2. Off-mode CLI still used an ASCII-space substring and falsely reported silence
   for tab-separated profile output. Fixed with the same token identity as
   the reader; actual CLI REDs cover whitespace variants before publication.

Historical-consumer proof records assertion failures with zero setup errors.
Genuine evidence remains complete and unchanged: numerical source, raw run,
cost values and v3 measurement SHA are preserved. Nine r206 CPU tests and
eleven r205 regressions pass. Fresh post-fix review is required.
