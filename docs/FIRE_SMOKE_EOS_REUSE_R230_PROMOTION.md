# r230 promotion receipt

Promote the exact-input R1-to-R2 EOS reuse implementation at `a9da7ca2`,
with the test-only repair at `326f069b`. Two fresh post-fix reviewers on
`326f069bbe6408d7897021843a311f85cf83b54f` independently report zero P1/P2
across lineage/lifetime/numerics and test integrity/cost/evidence fidelity.
Round 1 found one P2 false-green test; round 2 confirms its mutation-proven
repair. No production numerical defect was found or tolerance changed.

The measured executable remains SHA256
`b1f031d88c6aa2413cfbdf9ff95ea46888f6db25a499d6a75c1b461e52d8a263`,
preserved outside git at
`rendered/fire_production_calibration/r230_eos_reuse/FireSequenceTest_b1f031d8`.
All gate/build/timing artifact SHAs and test limitations are recorded in
[the r230 entry](FIRE_SMOKE_EOS_REUSE_R230.md).

Three eight-step hot-input processes per variant measure outer accepted-step
wall **7.177452 → 5.827083 s**, an **18.81% reduction**. Device means are
**5.271889 → 3.939362 s**. Outer-wall p95 is **7.660015 → 6.392050 s**;
device p95 is **5.650391 → 4.384927 s**. These are qualification results,
not full-window performance. The one-second target is not met.

The eight equal-time persistent payloads match bit-for-bit excluding only
producer build identity. Owner gates, native mutation tests and serialized
clean make/Deployment/Opto builds pass. GUI builds are not executed Opto
solver qualification. No fixed-k shortcut is promoted. Production runs with
this changed kernel start from zero; historical checkpoints remain evidence,
not migration anchors. No production run is currently active.

The next cost work starts from the remaining measured producer costs; do not
infer a saving from unqualified iteration truncation. The full tier-8 spectrum
and temporal preview have already been delivered; tier-10 window and final
spectral deliverables remain queued.
