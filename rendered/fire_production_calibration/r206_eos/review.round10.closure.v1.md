# r206 independent review closure

Reviewed commit: `06b8ac6c2664eb6c74951e14ba9e6fef5d410366`.
Numerical implementation commit: `20607aea6da099f0083bcdbcbd6f62b3ac5149a2`.
This attestation adds review metadata only; it does not change reviewed code,
measurements, tolerances, expectations, or historical evidence.

## Fresh round ten

Two independent, read-only reviewers returned **zero P1 and zero P2**:

- `r206_round10_fidelity`: numerical fidelity, private device lineage,
  residency/accounting, independent r190 comparison, executed EOS qualification,
  and claim scope. Confidence 0.90. Confirmed four-word endpoint retention,
  unchanged arithmetic/root search/tolerances, atomic refusal/publication,
  all 15 named REDs, and unchanged numerical sources since `20607aea`.
- `r206_round10_evidence`: evidence and test integrity. Independently ran all
  21 CPU regressions (10 r206, 11 r205), reproduced both measurement artifacts
  byte-for-byte, verified executable/qualification/timing bindings, and
  reproduced the historical copied-run false greens as assertion failures
  with zero setup errors. The historical complete-analyzer path was separately
  exercised, not inferred from the earlier repeat-guard assertion.

Neither reviewer edited source, rebuilt, or executed GPU work. Earlier review
rounds were not clean: their findings and fixes remain in the adjacent round
ledgers. The round-four setup-error proof remains historical and is superseded
by round-five assertion-only evidence. This clean round closes the r206
implementation-review loop, not the outstanding production milestone ladder.

## Exact bindings

- Executable: `7bbc7e924b1f35ceff03cc41f610652cb86477133f0bcf5b754c2e36ac5b5eb0`.
- Parent qualification: `78e4c9f7ee441052d934de30fe6dda058640392da4733565b8db050f88718cb5`.
- Final-source EOS execution supplement: `b627b6292f8626e0ec1ce974f9a6c38fabc5c140f982a6c57a72bf96f82fb798`.
- EOS execution log: `8c4588b5d843deb73240853b41eddac3d4e55a9be1900191b37cd8b1ca62ecf9`.
- Warm v2 measurement: `27212e469259999198337bb61b24bf8876c5398c24b82e924ddd208ea381e96d`.
- Qualified v3 measurement: `f889b9148415927d9ef7ac1304c819f6d4177d5a0b39da032e9484cd03774569`.

The qualified analyzer pins nine independent executed timing-log roots and
the EOS execution authority. Historical qualified v2 remains preserved but
is superseded by v3 for complete standalone EOS qualification.

## Accepted result and remaining scope

Warm-starting did not improve cost: baseline 594.713733 ms versus warm
596.281520 ms per EOS evaluation call. Exact endpoint reuse instead reduces
that kernel to 64.58951654 ms/call, a 9.207589-fold improvement. Its average
14 calls per step cost 904.2532316 ms/step. Table construction is separately
instrumented (about 0.2935 ms/call); it is not included in the evaluation-kernel
figure and is included in whole-owner timing.

Whole-owner p95 is 3243.8116672 ms device / 4736.904042 ms wall. These are
three-step cold-prefix measurements, not a hot-front or production-window
verdict. The 300–500 ms whole-owner target is unmet. Target assembly, residual
kernels, from-zero tier-eight focusing replay, convergence export, fixed-k
selection, and the tier-ten queue remain pending. Host hashing remains deferred.
