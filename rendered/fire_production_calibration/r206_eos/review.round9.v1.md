# r206 fresh review, round 9

Reviewed HEAD: `f2ba9994`; numerical source: `20607aea`.
Reviewers: `r206_round9_evidence`, `r206_round9_fidelity`, read-only.

Numerical/qualification/fidelity: no P1/P2.
Evidence: no P1; one P2, confirmed and fixed before round 10.
Different raw log hashes could represent one copied execution plus cosmetic
whitespace/comments, reducing the falsely reported process SD to zero.

Repair: generic repeat admission compares command-clock execution identities;
the fixed r206 campaign also pins its nine actual timing-log roots. Full
directory/sidecar copies with whitespace/comment-only differences are refused
by both guard and complete analyzer. Historical RED proof has assertion
failures and zero setup errors. No published artifact is modified by tests.

Actual data and measurement v3 are unchanged. Fresh post-fix review is required.
