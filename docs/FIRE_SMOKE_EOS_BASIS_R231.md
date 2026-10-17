# r231 — remove repeated temperature-only divisions from EOS species work

The promoted r230 hot-input profile still spends 2479.214 ms/step in
`evaluate_resident_eos_candidate`, followed by 849.447 ms in target terms.
These are sums of per-kernel device intervals divided by 24 accepted steps
over the three final-candidate logs SHA-bound in the r230 entry. EOS remains
the first cost target. The outer baseline is 5827.083 ms/step; fixed-k has no
qualified saving and is not an alternative assumption.

The EOS energy evaluator shares powers of temperature across seven species,
but spells the compensated divisions T^3/3 and T^5/5 inside the species loop.
The operands are independent of species, density, segment and coefficients.
Compute those two complete EOSDD values once per energy evaluation and reuse
their four words. Preserve species sum order, coefficient products, segment
predicates and enclosure arithmetic. In particular, division by five remains
division, never multiplication by a rounded 0.2 literal. Leave the independent
direct evaluator unchanged for qualification comparison.

This is a small arithmetic scheduling experiment, not a new thermo table or
authority. The compiler might already eliminate the repeated work, or longer
register lifetimes might worsen occupancy. Neither speedup nor promotion is
assumed. Baseline is r230's three final eight-step hot-input processes, using
the preserved `FireSequenceTest_b1f031d8` executable. After one candidate,
repeat the same three-process protocol, owner gate and eight-step numerical
payload comparison; reject an ineffective or incorrect candidate. No bound
changes, iteration truncation, or new certification mechanism. Production
starts from zero after any accepted kernel change.

## Candidate measurement, not yet promoted

Candidate executable SHA256
`510fc680514757dd5d8be3f60ef920fe34fb3dd5a681217497fd5731ec1916ea`,
preserved as `r231_eos_basis/FireSequenceTest_510fc680` under the calibration
output directory, passes the native owner gate and eight-step persistent
payload comparison (only producer build identity excluded). No per-field
bit mismatch is observed. Independent direct EOS evaluation remains in the
qualification kernel; no reference arithmetic was edited.

Three eight-step process means give outer wall **5490.994 +/- 8.354 ms**
versus r230 **5827.083 +/- 13.737 ms**; standard deviations are across the
three process means. Device mean is **3605.724 +/- 7.139 ms** versus
**3939.362 +/- 15.041 ms**. Nearest-rank p95 across 24 steps is
**6049.525 ms wall / 4038.835 ms device**. This approximately 5.77% wall
improvement exceeds repeat variability but remains isolated hot-input
qualification, not a full-window projection.

Per-kernel attribution: EOS **2479.214 → 2144.314 ms/step**, target
**849.447 → 849.521**, advective pair **155.161 → 154.948**. The saving
is localized to the changed kernel family; target and advective work remain
essentially unchanged. No fixed-k saving is claimed.

Artifacts under `rendered/fire_production_calibration/r231_eos_basis/`:

| Artifact | SHA256 |
| --- | --- |
| owner_gate.v1.log | 3c6b61411a44add51608054eaffec47e55eb55ed5fddf59ee5b781cf7a4dce88 |
| numeric_comparison.v1 | 767ce3ef92d0a117b48f10260e92c5b0856c44d1fdfd10c7ac0ad120ee0f0be9 |
| candidate_1.log | f04fe070b8151b3637465c303053b833ac14effcf13fba0343e96a07270a3215 |
| candidate_2.log | 8daebbd0527c32470e069740719c722065014d445f47058ab1d8e8a247803711 |
| candidate_3.log | 828714c878e8ee47ac6cc585a22e0dfcbc89b65fc55d0b96395ece1bf1718bf8 |

Native cache regression and serialized clean make/GUI Deployment/GUI Opto
builds pass. Make has no warnings; GUI has only documented missing-checkout
OIDN and AppIntents notices. GUI builds are not executed Opto solver tests.

| Build/regression artifact | SHA256 |
| --- | --- |
| native_cache_red.v1.log | b264b6df8ddc53ff54fb6c51deed1e69a2331b646555777e8b00dacc0c79c957 |
| clean_make_build.log | e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29 |
| clean_gui_deployment.log | ea79d6100c4153f8842a32484c1877ca4b1cc981b5fe9e6231c9cad53007c3e5 |
| clean_gui_opto.log | 3e8d720de9be7114b5583524778a393623fb57984163215383230267072d9349 |

Fresh review is pending. Self-audit checks:
unchanged divide-by-five formulation; independent direct evaluator; exact
four-word values and operation ordering; register-lifetime performance risk;
qualification-versus-window scope. No production run is active.
