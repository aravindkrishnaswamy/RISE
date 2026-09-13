# r232 — target-basis sibling of the measured EOS scheduling win

r231 reduced EOS cost through exact reuse of T^3/3 and T^5/5. The target
enthalpy helper still computes the same temperature-only divisions once per
species. Its enclosing target kernel costs 849.521 ms per hot accepted step.
This is the bounded next experiment: extend the existing thread-local target
basis with the two complete EOSDD results, computed once separately for each
temperature path. Reuse them in enthalpy without changing heat-capacity
arithmetic, species sums, segment predicates or two-path hull construction.
There is no new device buffer, public surface, table or tolerance.

The original direct enthalpy/heat-capacity comparators remain unchanged.
Division by five stays division. T^5 need not remain stored in the basis once
its divided value is produced, since no heat-capacity consumer uses T^5.
Keep T^3 for heat capacity. This limits extra live register state; whether the
compiler improves scheduling is a measurement, not a premise.

Baseline: r231's three eight-step hot-input processes (outer wall 5490.994 ms,
device 3605.724 ms), preserved executable SHA256
`510fc680514757dd5d8be3f60ef920fe34fb3dd5a681217497fd5731ec1916ea`.
Repeat the same input, three timings, native owner gate and eight-step payload
comparison. Reject no-win or incorrect candidates. A successful candidate
still needs clean configuration builds and fresh review. No full-window
projection, fixed-k shortcut or kernel-transition migration; production starts
from zero with an accepted changed kernel. The one-second budget remains unmet.

## Candidate results, unpromoted

Executable SHA256
`f1c4b061e2dff70a0a52c071a9aec4f40a67985f73f8aa2c23876fde6ed1958a`
passes the native owner gate and eight-step canonical payload comparison,
excluding only producer build identity; explicit per-field bit mismatches are
zero. The independent direct target arithmetic is unchanged.

Three eight-step process means: outer wall **5417.490 +/- 14.320 ms**,
device **3532.581 +/- 14.901 ms**. Baseline r231 is **5490.994 +/- 8.354 ms**
wall / **3605.724 +/- 7.139 ms** device. Standard deviations are across three
process means. Candidate nearest-rank p95 over 24 steps is **5933.399 ms**
wall / **3926.539 ms** device. The modest **1.34%** outer-wall saving is
larger than repeat variability, not a major advance toward the budget.

Attribution supports the change: target kernel **849.521 → 779.474 ms**,
EOS **2144.314 → 2141.248 ms**, advective pair **154.948 → 154.629 ms**.
These are isolated hot-input results; neither a full-window prediction nor
a fixed-k saving is claimed. Native regression and serialized clean builds
are running before independent review.

Artifacts under `rendered/fire_production_calibration/r232_target_divisions/`:

| Artifact | SHA256 |
| --- | --- |
| owner_gate.v1.log | 087549c606a1eb187a53fa4dd8192a3dd09710e6b389eb45d287a6321f89361e |
| numeric_comparison.v1 | 398ef8e43a61e4b5c86ba15c24acee71a7de691812271c19bdb4b68d851b79cd |
| candidate_1.log | 879d8a0af2d65886a9b2ad9c7a8bd3057884293da6f4fe0310e80d4d193c8126 |
| candidate_2.log | 88b18f36acd94633d0dab4c84db1b57cf6e574c3184fee4829dc8e7f5f0c51de |
| candidate_3.log | 4ec6103d62168659c0eef732d3bcc7a7461baf183a19ebc4965764f4d2c49d27 |

Native cache regression and serialized clean make/GUI Deployment/GUI Opto
builds pass. No make warnings; GUI logs contain only the documented OIDN
missing-checkout path and AppIntents notices. GUI compilation does not claim
executed Opto solver qualification. Fresh review remains pending.

| Build/regression artifact | SHA256 |
| --- | --- |
| native_cache_red.v1.log | b264b6df8ddc53ff54fb6c51deed1e69a2331b646555777e8b00dacc0c79c957 |
| clean_make_build.log | 01d153dd56358e77f9a185948afffb569c5bbd3649ea0e27f9f383aae6113d3c |
| clean_gui_deployment.log | 3d851e40d473a1a15ef69f2a8550d6abbc52ce79396837726a50847448b3ef40 |
| clean_gui_opto.log | d24bce3833dbb221e7c5d15b1f7488e387379c4fc587ccbde1d65ed3ca389492 |

Self-audit: exact divide-by-five operands; both adjacent-temperature paths
build their own basis; T^3 remains available to CP; independent direct
comparators and existing reciprocal mutant remain intact; timing scope and
small magnitude of this gain are explicit. No production buffers are added.

## Review repair, pending fresh review

Round 1: cost/evidence reviewer found no P1/P2; arithmetic reviewer found
one confirmed P1 in the omitted target-helper regression run. Its two mutation
anchors still named the removed `basis.t5`. Updated anchors exercise the new
expression; all four target tests now pass, including wrong-power, rounded
fifth and bound-word mutants. The 1792 synthetic cases include 91 refusals.
Log `target_helper_review.v1.log` SHA256
`6a45f08775ceb8296fbc472b98b616d695dc71e6c805953deaac81d3574c3988`.

The sibling EOS extraction suite exposed a separate stale reference: its
immutable r213 input `9f1b80fb...` contains the retired rounded `0.2f`
formulation. Comparing current exact division to that known defective formula
is not a valid scheduling invariant. Re-pin this current regression to reviewed
pre-r231 `a9da7ca2`, which retains corrected division inside the species loop.
No expectation, tolerance or production arithmetic changes; old artifacts and
the failing transcript remain preserved. The test explicitly disclaims
re-certifying historical r213 evidence. Both repairs are test-only, so the
measured candidate binary and its numerical evidence are unchanged.
