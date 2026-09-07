# r212 — adopt compatible transport, price the hot owner

The owner accepts the r211 survival result as the engineering verdict and
adopts the complete resident §3.7 R0/R1/R2 owner as production transport.
The formal three-way contract verdict remains pending same-tier, exact-time
oracle composition. Engineering progress no longer waits for that verdict.
This is not readmission, an empirical row, a completed statistics window, or
a new movie. Historical remap and rejected hybrid configurations remain
explicit diagnostic/baseline paths; the temporal production entry now selects
the ported full-Picard owner. No fixed-k variant is enabled by this entry.

## Engineering evidence

The sealed continuation accepted 846 additional steps from step 1300 through
step 2146 at exactly 3.5 s, with no 15 m/s crossing and no recorded retries.
Peak velocity over that continuation was 9.3974685668945312 m/s at
2.1787969120778143 s; final velocity was 6.6301217079162598 m/s.
The r194 baseline reached 63.960872650146484 m/s at 2.6991133776609786 s with
aligned advection 11691.86634461989 kg/(m² s²). The port's first sample after
that time is 6.2560162544250488 m/s at 2.7006446901941672 s. These are not
equal-time samples and are not silently promoted into a contract comparison.

At beginning time 2.1080244191689417 s, column face (30,33,33), the signed
rates in kg/(m² s²) were: advection -184.08273315429688; buoyancy
+5.8219738006591797; stress -3.4076552391052246; phase source 0; combined
projection +16.309048442942053; total -165.35929489452786. The projection
includes tangent/source/tail contributions; a separated tail impulse is not
available. No standalone restoration projection ran. The observation's tail
targets were zero. These rates are observations, not the tier-ten 159.01
criterion reinterpreted at tier eight.

Terminal summary SHA:
`0593230f7bc977d543f45d0b8627c88a3066631e8c171c32a0df1dca2e06d66a`.
Trajectory SHA:
`cb10278dc49f33c9a9718496804bf81fe5493faf4af0eb27cd1a3b7db3d1a057`.
Column SHA:
`f26f66da5842ca7e38d21e5ef92d867e646905bd8a12b3777cf61d09f18938fd`.
Final checkpoint SHA:
`c46aa2dbfaa55396175f23153ad3f2d5802affc6c49167cf30f1133f3e235ffd`.
All live under `rendered/fire_production_calibration/r211_resident_migration/continuation.v2/`.

## Cost reconciliation — not observer overhead

| Scope | Device mean / p95 | Wall mean / p95 | Total Picard iterations, mean |
|---|---:|---:|---:|
| r206 cold: three processes × three steps | 2.908 / 3.244 s | 4.312 / 4.737 s | 8.00 |
| r211 production: 846 resumed steps | 16.245 / 20.678 s | 18.317 / 22.959 s | 15.51 |
| r212 hot: three processes × eight matched steps | 16.521 / 17.723 s | 18.442 / 19.689 s | 13.50 |

The r211 convergence observer executed once, at 19.593469709 s wall, outside
the production timing rows. It cannot explain the per-step increase. Production
rows exclude checkpoint/publication and outer-loop work: total continuation wall
was 16890.882721416 s (4.69 h), not just their sum. No render occurred in-loop.
The production row's average wall-minus-device residual is 2.071638269 s. It is
not a measurement of hashing duration; prior CPU samples are not wall fractions.

Three uncontended hot profiles from the same untouched step-1300 checkpoint
reproduced all eight accepted times, input/output roots and kernel-set roots.
The existing diagnostic intentionally returns 93 on successful EOS acceptance;
the explicit successful, unchanged-checkpoint terminal and all eight rows are
required, not inferred from exit status. Mean device time across repeats was
17.113, 16.515 and 15.934 s/step; do not mistake this temporal drift for a speedup.
Inclusive EOS kernel cost was 13.547 s/step (694.739 ms/call), target terms
2.340 s/step (119.986 ms/call). Inclusive intervals may overlap. The report
separately reconciles exclusive owner phase totals to device time, using only
the decimal-output accounting bound, not a solver tolerance. r206's corresponding
EOS mean was 0.904 s/step. Hot inversion work and increased iteration count,
not the retired observer, dominate. Even eliminating all target assembly cannot
meet 300–500 ms while hot EOS alone costs thirteen seconds.

`tools/report_fire_r212_cost.py` reproduces the cost report from SHA-indexed
executed inputs. Report SHA:
`9b8bb7f74733d3d16283dd1ed3362d04579cefc806055b128d71bcb7eabd18a6`.
Output: `rendered/fire_production_calibration/r212_transport_adoption/cost_reconciliation.v1.json`.
The 300–500 ms goal remains unmet. Extrapolating 18.317 s/step at the audited
1.646 ms step to 25 simulated seconds is about 77 wall hours, not an overnight
forecast or an under-hour claim. Future state costs and frame I/O remain extra
uncertainties. The measured hot EOS route is critical again.

## Convergence nomination, not a hidden acceptance change

The captured column has R0 successive velocity changes 0.0517543, 0.0463344,
0.0463682 and 0 m/s at iterations 2–5; R1 reaches zero at iteration 3; R2
changes 2.58684e-5 at iteration 4 and 3.23169e-7 m/s at iteration 5. This
nominates (5,3,5) for matched-state qualification, not k=2 or 3 globally.
The exporter explicitly lacks convergence enclosures and next-candidate
momentum capture; its provisional momentum is the frozen projection input.
Zero provisional deltas cannot certify truncation. Existing local enclosures
and discharged branch envelopes must still admit the candidate against the
full owner before fixed-k production is enabled. No pooled ratio or tolerance
widening substitutes. The prohibition on fixed counts remains oracle-scope.

Convergence artifact SHA:
`a9e8108d5d53535c6369cbf98ffd69cfa0ee4e950f02e9c2c59070c1045bcc4f`;
CSV SHA `facca1f2af06b3df0c87490e8f3ea2b725fe58da90a4febdb0834c534e5c1eb8`.

## Parallel reference and remaining sequence

`--fire-oracle-tier8-reference OUT` runs the fp64 oracle from zero, tier eight,
seed 1234, original three-second case identity, to exactly
2.1080244191689417 s. It retains periodic checkpoints and records the executing
build/binary. A final checkpoint of another tier, one-bit-different time or
another build refuses qualification. This constructs the matched beginning
state only; the subsequent substep composition, temporal terms and tier-eight
column criterion must be measured, not borrowed from the tier-ten checkpoint.

Then: qualify fixed-k; continue EOS/target/host work one change at a time;
tier-eight full statistics window, spectrum and preview movie delivery;
tier-ten onset then window/rows/movie; report, fresh reviews, revision digest.
No numerical or performance qualification is inferred from this schedule.
