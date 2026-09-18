# r240 — use canonical source temperature on the ported owner

## Finding and evidence

r239 live admission (`7dd662e5`) passed its mixed/cold owner and actual
one-step launcher/retry gates; two fresh independent reviews found no P1/P2.
The corrected tier-8 full window then refused step 2, after one accepted step
at 0.0016462659696117043 s. It is incomplete, not a movie or empirical result.
The pipeline stopped before rendering. Its evidence remains at
`/Volumes/SSD/RISE/fire_production_calibration/r239_tier8_full_window.v1`:
stop SHA256 `d61e47c6a018acd3bc53218a8a35501f38467eaac1ba50e3ae7bbaa5996b0169`,
adjacent log SHA256 `c0f22ea11db9256971c9bd415f5aad915d5a977463791c6af9081f360056af3f`.

A from-zero short diagnostic reproduces the refusal at cell 1. Stored sensible
energy is 3.16004895861699e-11 J/m3; forward evaluation at its accepted 300 K
gives 3.1777833835907163e-11 J/m3 (binary32 projection
3.1777833836565961e-11 J/m3). Both accepted inversion implementations select
the lower endpoint, 300 K, under the existing affine feasibility envelope.
Thus an envelope-qualified endpoint is not an exact forward-roundtrip root.
The optional legacy evaluation-temperature override demands that stronger
property, and rejects it. All 20 timestep reductions repeat the same mismatch.
Diagnostic log SHA256 `911dda1361db4db4b2aeb4735cd5b1285bb71fa80c4ad0a6a000ba9ec2d8ef49`
is retained under `rendered/fire_production_calibration/r239_burner_projection/`.

## Amendment, before implementation

The ported production launcher must use the canonical source authority's
existing default: derive source evaluation temperature from its own accepted
conservative input. Do not submit the optional legacy forward-exact override.
This removes a redundant caller-supplied temperature surface, not an EOS gate:
the canonical inversion/admissibility checks and the override's exact check
stay unchanged. Historical replay and diagnostic callers retain their explicit
override and its refusal behavior. The same producer still creates packet
bytes once for transport and persistence.

This is a source-evaluation semantic change (the default publishes a binary32
temperature); version the production burner policy to v2 so case identity
changes. No bit-identity to the failed v1 run is asserted, and no migration is
allowed: restart from zero. Do not alter conservative bytes, nominal fuel,
pilot controls, physical gates, or any enclosure.

Regression: the recorded tuple must be admissible through the canonical
default while the exact-temperature override still refuses; print the existing
energy-envelope comparison in J/m3. Re-run owner/local enclosure, launcher
retry and publication gates, plus the existing 24-step from-zero diagnostic
to cover continuation (the one-step fixture alone missed this boundary).
Fresh review precedes another full-window launch. A short diagnostic is never
substituted for the statistics window.

The first amended 24-step diagnostic gets past forward closure but exposes
the matching downstream caller error: pilot-ledger recomputation still uses
the old binary64 evaluation temperature, while the canonical producer used
its binary32 temperature. Preserve exact ledger equality by recomputing from
the producer's sealed beginning temperature on the ported path. This changes
the ledger's input binding, not its equality check or physical pilot ceiling.
Historical override callers keep their original evaluation temperature.
The diagnostic is retained as `r240_tier8_prefix.v1` on the external disk;
it stopped after one accepted step and is not a completed-prefix claim.

## Implementation gate

The ported launcher omits the legacy temperature override and uses the sealed
source beginning temperature for pilot-ledger recomputation. Policy preimage
is now `rise.fire.production.burner.v2` with `canonical-source-temperature`.
No source-authority comparison was removed. The only other override caller
is the historical checkpoint physics diagnostic; it intentionally retains
the legacy evaluation-temperature comparison. Non-ported replay in the shared
launcher also retains it. Source persistence still carries the canonical
packet bytes, with no independent reconstruction.

Recorded-cell regression: residual 1.7734424973726328e-13 J/m3 versus the
existing endpoint feasibility enclosure 335.73580277154764 J/m3. The latter
is the pre-existing affine admissibility envelope, not a new source error
budget. Canonical default accepts at 300 K; explicit legacy override refuses
at cell 0 in the repeated-tuple fixture. Both outcomes are asserted.

The repaired 24-step diagnostic (`r240_tier8_prefix.v2`) completes at
0.039510383270680904 s, Tmax 369.108863 K, maximum EOS deviation
0.00236682603. Sampled RSS reaches 1,843,445,760 bytes. This is a cold-start
continuation check, not ignition, statistics, long-run memory, or speed
qualification. The preceding v1 ledger mismatch is its retained mutation
witness (log SHA256 `9b413e47b72f9b189b26b3585aab662ffad340c8d9f02243e54d1513bfb2837f`).

Focused warning-free build, paired mixed/cold owner gate, endpoint RED,
actual launcher retry, input and publication gates all pass. No clean whole-
repo/Opto rebuild is claimed. Fresh review remains before the full run.
Log paths below are under `rendered/fire_production_calibration/r239_burner_projection/`.

| Artifact | SHA256 |
|---|---|
| Executable | `f542462b288dee29b2c9c5fbf1ba51cc27c51e14ff5ca0445ec995fd008f4ca8` |
| `r240-build-ledger.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `r240-prefix-v2.log` | `f27de695fed414932a9b3ba5c94afb121e9ce0614e98827cdfd0e6c50fafe7a9` |
| `r240-paired.log` | `51af9fc8cf14cd082b270bede62b7353eff5de5db8b6ca568bed7afc8e9093b3` |
| `r240-launcher.log` | `e31640f994fa1ef125107180af03c44c7608102baebe9ac1c451ea69f684954d` |
| `r240-inputs.log` | `3a9dc882e11f61b7350491f86b40bcbc89d633b6fe5dabcb6385aba3f1e98b47` |
| `r240-publication.log` | `b15d589bb9734223294ec80d46d3a7c931daf107b09c3333642df60fbbc04d56` |
| External v2 diagnostic checkpoint | `90de274d476fde2eb7b0735f040c0ac80e9b157862021bb1134b448ec23c84c5` |

## Reviewed launch

Fresh numerical/formulation and contract/test/provenance reviews of
`3331b0cd` each report zero P1/P2. They confirm input binding rather than
envelope widening; neither independently ran heavy tests. No code changed
after that review.

The from-zero full tier-8 window is running on the preserved tested executable
at `/Volumes/SSD/RISE/fire_production_calibration/r240_runtime.v1/FireSequenceTest`.
Output is `r240_tier8_full_window.v1` in the same calibration parent. Case ID
`fb00b7f787af097d68ef56712c8fb8cf74d30ea68084e5965fa99fefe091bc41`;
build ID `777ac6f054a78710db98aa2e54a5f1bb01f73a74b99169003e70b72e0d73e8cd`.
The full target is 25.091040127156969 s: discard 10.466040127156969 s, then
14.625 s statistics at 0.0625-s frame cadence. No resume or shortened window.
Launch receipt SHA256 `6082e91973c8427c4f51e1edcaf207ad42dbaf612b11b1b87c74bf8e75b0a9dd`;
host-memory admission SHA256 `5d6bc6d52f7f3527d4e55f40cb2e55bdd5de44553acf9457ff81f1c993f01a96`.
At this receipt it has accepted 25 steps (0.041156649240292609 s), RSS
1,878,409,216 bytes. This is launch progress, not a completed window.

The sequential shell pipeline (session 86530) runs the existing full-sequence
preview renderer only on successful simulation completion, then the preserved
exact-cadence MOV encoder/verifier, then the spectrum exporter. Planned outputs
are `r240_tier8_preview.v1/temporal_fire_preview_16fps.mov` and
`r240_tier8_spectrum.v1`; neither is claimed to exist yet. Failures stop the
pipeline. Label remains tier-8 `preview_primary`, with no empirical rows or
final-fidelity claim. No builds, qualifications, oracle or other render run
concurrently. A narrow read-only app monitor (`rise-movie-pipeline-monitor`)
checks for failure or verified movie readiness every 30 minutes, stays quiet
while healthy, and delivers the movie link only after decoded verification.
