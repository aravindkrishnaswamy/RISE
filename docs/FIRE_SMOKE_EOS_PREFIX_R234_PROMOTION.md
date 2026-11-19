# r234 promotion receipt

Promote the exact EOS bisection-prefix implementation `51c7445f` with the
diagnostic extraction repair `1e07d533`. Two fresh post-fix reviewers on
`d2d977b5` report zero P1/P2 on source/lineage and tests/cost/evidence.
Review took two rounds: no P1s; one surrounding P2 (duplicate helper in the
optional histogram shader) was fixed and native-tested before promotion.

Final measured wall is **4.047979 → 3.310496 s/step**, **18.22% lower** over
three eight-step hot-input processes. Device mean is **2.871500 s**; p95 is
**3.611129 s wall / 3.168212 s device**. Table construction is included in
these costs. EOS evaluation plus table construction remains about **1.485 s**;
target assembly about **0.779 s**. The one-second and design targets remain
unmet. These are qualification measurements, not full-window projections.

Measured executable SHA256:
`fab251f25acbd4f6bacf1250685eece634648aea9cfcd5b65616b899d166447f`,
preserved as `rendered/fire_production_calibration/r234_eos_prefix/FireSequenceTest_fab251f2`.
The [r-entry](FIRE_SMOKE_EOS_PREFIX_R234.md) binds all logs and derivations.

All eight equal-time canonical payloads match r233, excluding only producer
build identity. Native owner/refusal checks, CPU sanitizer tests, the repaired
native iteration diagnostic, and clean make/Deployment/Opto builds pass.
The diagnostic evaluates 78727896 cells with zero refusals or temperature-bit
mismatches; it is excluded from timing. Sampled RSS stays near 1.90 GB without
compression. The existing run RSS monitor remains required.

No tolerance, physical gate or manifold policy changed. No migration is
authorized; the next production run starts from zero. No production process
is active. Tier-8 preview/spectrum are already delivered; tier-10 and spectral
deliverables, formal filtered-field report and branch hygiene remain queued.
