# r236 promotion receipt

Promote the exact intra-cell endpoint aggregate reuse implementation
`9fd928d4`, with test/documentation repairs `b7b0b7d9`. Two fresh post-fix
reviewers report zero P1/P2 on current source/tests and numerical evidence.
Two review rounds: no P1s; two P2s (historical status wording and partial
poison-output coverage) fixed. Production source is unchanged by those repairs.

Wall mean improves **3.310496 → 3.274233 s/step**, **1.10%**, over three
eight-step hot-input processes. Device mean is **2.839183 s**; wall/device
p95 is **3.570270 / 3.131429 s**. This is a modest saving, not the one-second
target or a full-window projection. EOS plus table remains about **1.451 s**,
target assembly **0.779 s**, and wall minus device **0.435 s**.

All eight exact-time canonical payloads match r234, excluding only producer
build identity; per-cell/per-face bit mismatches are zero. Native owner/refusal
and source-generation diagnostic gates pass, as do CPU sanitizer/RED tests
and clean make/Deployment/Opto builds. Opto is a build gate, not an executed
solver qualification. The [r-entry](FIRE_SMOKE_EOS_ENDPOINT_REUSE_R236.md)
binds evidence SHAs, derivation and coverage limitations.

Measured executable SHA256:
`d9397b6fddc682819724e2a02ef3c26f1d692eff8ef7b753e2e5e8c62ca83655`,
preserved as
`rendered/fire_production_calibration/r236_endpoint_reuse/FireSequenceTest_d9397b6f`.
Use this preserved binary for its measured identity, not a different local build.

No numerical bound, physics gate, manifold policy or device allocation changed.
RSS samples remain about 1.91 GB with zero compression; production retains its
32-GiB ceiling. No migration authority: subsequent production starts from zero.
No heavy job is active. Tier-8 spectrum/preview are delivered; tier-10 and
spectral outputs, formal filtered-field report and branch hygiene remain queued.
