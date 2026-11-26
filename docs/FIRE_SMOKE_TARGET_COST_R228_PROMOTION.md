# r228 — promotion receipt

Promote source/test commit `97d3a34b` after the fresh three-axis review
recorded in `FIRE_SMOKE_TARGET_COST_R228_REVIEW.md` and the subsequent
serialized clean make, GUI Deployment and GUI Opto builds, all exit zero.
No source changed after review. Make emitted no warnings. Each GUI build
emitted only the documented missing-checkout OIDN search-path warning and
AppIntents metadata notice; no additional compiler/linker warning was found.
This is clean Opto build coverage, not a claim of running the simulation
through the GUI executable. Numerical qualification is the measured Metal
test executable and current owner gate recorded in the main r228 entry.

The measured executable is now preserved outside git as
`rendered/fire_production_calibration/r228_target_cost/FireSequenceTest_b9742c39`,
SHA256 `b9742c395bc1049bda1f4077843aa74c6a33d6435adc50c7e6b39c3113ad7190`.
Future kernel-changing production runs start from zero. No migration or
checkpoint-transition machinery is introduced.

Accepted cost result: three eight-step hot processes each, wall mean
8.542611 → 7.169966 s/step (16.07% reduction), device mean
6.649284 → 5.275810 s/step. Corresponding p95 device/wall:
7.144476/9.155555 → 5.643258/7.655947 s. These diagnostic-window observations
do not establish a full-window speedup or meet the ≤1 s production target.
EOS remains 3.804702 device s/step; changing-momentum capture and fixed-k
qualification remain next, with no k inferred from frozen-input deltas.

Build logs remain outside git in the same r228 directory:

| Artifact | SHA256 |
| --- | --- |
| clean_make_build.log | b3e25565f79a1a5474e3f6af6c9afb7a394b2403a85e30fed9f5785908d629a1 |
| clean_gui_deployment.log | 2ea4aa4a042a7f79b2078e452b832a96f3a7cdcd3c9f6858564ca490674bded9 |
| clean_gui_opto.log | 16cf305d25486957510b5b5869779b8af92f66f81fc12109011c2d84d05bd6f3 |
