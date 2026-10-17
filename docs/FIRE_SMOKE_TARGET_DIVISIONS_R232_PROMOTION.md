# r232 promotion receipt

Promote source `879918a2` with test-only fixes `b29ff814`. Two fresh post-fix
reviewers return zero P1/P2. Round 1's P1 stale mutation anchors were repaired;
the sibling EOS reference now pins the reviewed corrected division, not the
retired rounded reciprocal. Both reviewers independently reran all seven CPU
tests, including deliberate arithmetic and obligation mutations. No numerical
expectation or tolerance changed. Historical artifacts remain unchanged.

The additional EOS helper transcript `eos_helper_review.v2.log`, under
`rendered/fire_production_calibration/r232_target_divisions/`, has SHA256
`7f6c417cbbaddda5349466cafb6c1a72f647b3e6397f1320eb88191f624cc462`.
Other evidence hashes are in [the r232 entry](FIRE_SMOKE_TARGET_DIVISIONS_R232.md).

Three eight-step hot-input processes measure outer wall
**5.490994 → 5.417490 s**, a modest **1.34%** reduction. Device mean is
**3.605724 → 3.532581 s**. The target kernel accounts for nearly all of this
saving (**0.849521 → 0.779474 device s**). This is not full-window performance;
the one-second budget remains unmet and the next strategy must address larger
remaining costs rather than extrapolating small arithmetic wins.

Native owner, eight-step payload bit-identity, regression and clean build
gates pass. Measured executable SHA256 remains
`f1c4b061e2dff70a0a52c071a9aec4f40a67985f73f8aa2c23876fde6ed1958a`.
No fixed-k shortcut, kernel-transition migration or executed Opto solver
claim. Production starts from zero on this changed kernel. No run is active.
