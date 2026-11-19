# r210 review receipt

Reviewed implementation: `1d196178e5c3f8d40257412d5f1e513139444dde`.
Base: `17d31a8d`. Two independent, fresh reviewers per round.

Round 1 found three issues, all fixed:

- Missing Xcode CLI ARC invalidated the lifetime bound in that supported
  build. Main classified this as P1; reviewer had called it P2. Explicit ARC
  in both target entries for all four fire Metal files plus compile guards
  and 16 build-entry mutants close it.
- Resumed historical values could overwrite the derived case ID with an
  empty nested value. Restoration after checkpoint-value loading, a
  synthetic refused-resume regression and the real no-drain refusal seal
  close it.
- The small fixture did not prove the staging lifetime repair, and the
  diagnostic could return zero on refusal. A strict eight-step acceptance
  exit and an executed one-line no-local-pool mutant close it.

Round 2 reviewers:

- `/root/r210_round2_lifetime`: no P1/P2. Independently counted the packet
  layout, checked ARC scope and lifetime, production refusal before owner
  construction, overflow/shape handling and platform/API siblings.
- `/root/r210_round2_provenance`: no P1/P2. Verified qualification hashes,
  eight-step/observation records, strict refusal, post-resume case restoration,
  production selector-only RED, and limits on claims.

Reviewers were read-only and did not execute GPU jobs. Main executed the
qualified owner/full-suite/strict-replay/mutant gates. Xcode runtime lifetime
is supported by successful ARC builds and source reasoning, not a separate
Xcode observer replay. Device allocation is per-owner Metal delta, not process
RSS or host trace-vector storage. No r78 migration or focusing verdict is
authorized by this receipt.
