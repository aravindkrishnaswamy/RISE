# r226 — projection refusal after the statistics-window entrance

The r225 from-zero tier-8 window stopped with exit 94 after accepted step
6531, at 10.653518881154014 s. This is incomplete evidence, not a spectrum
or a window claim. The last accepted velocity was 5.4679288864135742 m/s;
RSS was 2152251392 bytes, compressed memory zero. The host did not kill it.

The refused owner's terminal physical projection reports, in identical
units, a maximum residual of 0.023551395162940025 s^-1 against its existing
0.0091240443289279938 s^-1 band (approximately 2.58 times the band).
Pre-projection residual was 430.44442749023438 s^-1. Dynamics passed with
maximum EOS deviation 0.00052201747894287109; no tail cells engaged.
This is not another EOS ordering refusal.

## Diagnostic decision before code

The resident physical projection currently performs 17 V-cycles, then
checks its physical residual. A fixed amount of work is not a convergence
proof. Insufficient convergence is a hypothesis, not yet the diagnosis:
operator inconsistency or a rounding floor must be distinguished from it.
Reuse the existing immutable-checkpoint diagnostic entry to inspect each
projection in the failed step, recording stage, cycles, and residual/band.
The checkpoint is diagnostic input only: no migration or continuation
authority is created. All future production kernel changes run from zero.
No band is widened and no downstream refusal is suppressed.

The existing diagnostic admits one additional exact checkpoint/build pair;
this is the smallest reproduction needed to repair the blocked window,
not a new certificate system. Any further solver change requires measured
convergence evidence and the existing owner gates and review.

## Preserved evidence

Paths below are relative to this worktree. Payloads remain outside git.

- Log: `rendered/fire_production_calibration/r225_full_window/tier8_window.v1.log`,
  SHA256 `6ff1f72b82a88e15fa4cef3de3c38788c76c1870583fd2ba1fcd6bd2287d3d31`.
- Stop receipt: `rendered/fire_production_calibration/r225_full_window/tier8_window.v1/window_stop.v1`,
  SHA256 `fca10f26979b6cf4c66ded4a3e61947d0b1eea4464c448fb5bfb885686438cc5`.
- Checkpoint: `rendered/fire_production_calibration/r225_full_window/tier8_window.v1/production_run.checkpoint`,
  SHA256 `d7c0632c7fccf71012f1bc618e9cc352d068068c7979cc3622eb17c47c5a9e9f`,
  v2 root `e7fbb2cec7c4974582c99f7b41b08a03e4e3557deae64a9caf9cd6c716506733`.
  Native inspection identifies accepted step 6494 (the last periodic
  checkpoint, not the final accepted step); the diagnostic allowance is
  therefore 38 attempted steps to reproduce the refusal after step 6531.
- Original build `461cd6c90adab0ded52c70175ff148ef31bc7ff55ac03762c875b8282cee76a9`;
  executable and launch receipt remain as sealed in r225.

The run monitor is paused because the worker has exited. The golden and
all historical checkpoints remain untouched. No movie or spectrum is
claimed from this incomplete run.

## Measured diagnosis and repair decision

The first diagnostic omitted the frame event: its 37 accepted steps match
the original exact times and input/output v2 roots, but its next CFL step
passes. This is not reproduction of the refusal. Preserved log
`rendered/fire_production_calibration/r225_full_window/r226_projection_diagnostic.v1.log`
has SHA256 `ff14ed317c469eea468692cf83fdc66eaabb2c674021d534f353b6fd161e0a8b`.
The corrected diagnostic uses the existing event limiter at the same pinned
16-fps cadence without producing frames. All 37 accepted steps again match.

The next step is 2.1246003598207608e-5 s, ending at the next frame event.
The first invalid projection is R0 invocation 2, not the final R2 where the
production adapter eventually refuses. Density, target, provisional momentum,
and boundary inputs are held identical in the following independent solves:

| V-cycles | physical residual (s^-1) | unchanged band (s^-1) | device ms |
| --- | --- | --- | --- |
| 17 | 0.023550970479846001 | 0.009120400995016098 | not isolated |
| 18 | 0.017061769962310791 | 0.009120645932853222 | 9.686 |
| 20 | 0.008745772764086723 | 0.009120849892497063 | 10.421 |
| 24 | 0.002043724991381168 | 0.009120845235884190 | 12.964 |
| 34 | 0.000152587890625 | 0.009120744653046131 | 18.231 |
| 64 | 0.000225067138671875 | 0.009120738133788109 | 32.180 |

The band changes only because its existing U/L functional is evaluated on
each solved velocity, not because its coefficient changed. This demonstrates
insufficient fixed work, followed by a much smaller floating-point floor;
it is not a floor above the physical contract. No alternative diagnostic
candidate was published.

Repair decision, before production code: keep 17 cycles as the bit-identical
first attempt. If its physical validation fails, independently solve the same
sealed inputs with doubled work, clipped to the existing request-domain cap
of 64 cycles (17, 34, 64). Validate after each attempt; refuse atomically if
the cap fails. Doubling is a bounded search, not a fitted cycle constant.
Charge all attempted solves to timing/invocation/allocation diagnostics.
Check validation before consuming *any* projection, matching the CPU owner's
existing rule; the prior resident adapter only checked final R2. No kernel,
physical band, target, limiter, or manifold policy changes. A new production
run nevertheless starts from zero because the acceptance schedule changes.
Existing build-bound case records identify the changed executable; no new
certificate or authority surface is needed.

Required checks: fixed-17 diagnostic still refuses this exact frame event;
the repaired owner accepts it within unchanged bounds; the r201 owner gate
retains the passing-input bit/enclosure contract, residency, and atomicity.

The corrected fixed-input curve is sealed in
`rendered/fire_production_calibration/r225_full_window/r226_projection_diagnostic.v2.log`,
SHA256 `457bd4482bfe525788f84c1f0b2c4e409e455a01d786129ef7ef67526c5adc8c`.
The fixed-17 diagnostic executable is preserved outside git at
`rendered/fire_production_calibration/r225_full_window/executables/FireSequenceTest_r226_fixed17_diagnostic`,
SHA256 `536c6664bca6bc570c548544ca71015952fd750a29b4fbff0a00662d3e609472`.
Its diagnostic exits successfully only on reproducing the refusal; it is
not a production continuation executable.

## Implementation qualification (in progress)

The resident owner now searches 17/34/64 at fixed inputs and checks before
any stage consumes the projection. Failed attempt outputs are discarded;
only the validated output receives the downstream parent capability. Every
attempt contributes to device time, invocation, transfer, and peak-allocation
accounting. No additional payload seal is introduced.

Sibling audit: all resident R0/R1/R2, canonical active-class branches, and
terminal re-projections use this one `Project` method. Both CPU owner paths
already check projection validation before consumption; the reviewed r190
reference and raw standalone projection arithmetic remain unchanged. Raw
projection APIs still return validation diagnostics to their callers, rather
than asserting that a fixed budget necessarily converges.

The existing `--fire-production-resident-target-metal` gate exits 0 on the
repair (r190/r201 comparisons plus existing lineage/residency/atomicity REDs).
The incremental strict test / safe-fp32 Objective-C++ / full-LTO link emits
no warnings. This is not a claim of a new whole-repository clean GUI build.
The first repaired exact-event replay exits 0: step 6532 ends at
10.653540127157612 s with dt 2.1246003598207608e-5 s, maximum velocity
5.9178409576416016 m/s, terminal residual 0.00018310546875 s^-1 against
0.0091243870556354523 s^-1. The step uses 29 projection invocations, including
failed attempts; device/wall are 4737.096/6322.202 ms. This single-step timing
is not a p95 or whole-window forecast. The first 37 steps retain their original
times and roots. Input checkpoint bytes remain unchanged. Log
`rendered/fire_production_calibration/r225_full_window/r226_projection_recovery.v1.log`
has SHA256 `8f960d450b122db3bed0763b27d5f909a26a68994fff700c2610d7cfa3d83c45`.
The primary owner-gate log `r225_full_window/r226_owner_gate.log` (under the
same calibration directory) has SHA256
`47e3be883ce2db842b95c9ba3ce2f0b521eb129a7ca159cfdd85403c652026f5`.

Fresh review found no P1. One P2 was confirmed and fixed: recovery must assert
the exact represented dt and endpoint, not merely success and step count.
The omitted-frame-event outcome from diagnostic v1 is now an explicit RED.
A closed/periodic-only sibling diagnosis was also corrected: those raw solves
use 12 cycles and do not honor the pressure-open work control, so they now
refuse after the initial invalid solve rather than repeating 12 cycles and
misreporting exhaustion of 64. No closed-boundary band changes.

Backlog under the owner's output-first cap: the inherited combined allocation
metric conservatively counts borrowed projection inputs twice; it is not a
unique-buffer census. It does not under-report memory or admit an over-budget
state. A unique-buffer accounting RED and separate failed-34/successful-64 /
failed-64 fixtures are deferred; the bounded control flow still refuses if
available work cannot meet the unchanged physical gate. Current r226 measured
recovery exercises the 17-to-34 path, not those extra fixture cases.

## Final qualification and from-zero restart

Source commit `775c69ff757762c4b44b5fb21e7203f8b7a3f2ed` passes the
primary owner gate and window preflight. Two fresh independent reviews of
the final numerical and evidence changes report no P1/P2 in this scoped
repair; the explicit historical/backlog fixture limitations above remain.
The exact-event regression exits 0 with `exact_event=1` and
`omitted_frame_event_refused=1`. All 37 preceding accepted times and input/
output roots match the original; the input checkpoint SHA is unchanged.
Final regression log SHA256:
`c4876cb03ab2f40fa8e0d64a167ef7108b180fcf467fb2453b63070ca7a472aa`;
trajectory SHA256:
`931e099222ad6e6aa055f888e40c09f40d37dc254b2e6d5fb5844efc29464ffe`.
They are `r226_projection_recovery.committed.v1.log` and the corresponding
`budgets/maximum_velocity_trajectory.csv` under the r225 calibration root.

The full tier-8 window has now started FROM ZERO on the identical qualified
executable bytes, SHA256
`6acc6871c80d4eb7744403039cf7db5271a4b603509d7fafd11f3cb8c3da864d`.
No migration is used. See `docs/evidence/fire/r226_window_launch.v1.json`.
The authored case ID remains unchanged because the physical case is unchanged;
the executable/build receipt binds the changed acceptance schedule. The native
build identity includes executable path (`Job.cpp`, renderer_binary.path), so
the frozen-path build ID differs from the gate-path build ID even though the
executable SHA is identical. Both are recorded, not conflated.
The earlier paused-monitor statement describes the stopped r225 run; its
monitor is repointed to r226. No completed window, spectrum, or movie is yet
claimed. No builds or qualifications may overlap the production worker.

## Full-window publication — 2026-09-13

The from-zero tier-8 run completed 15,355 accepted steps to
25.091040127243105 s (authored target 25.091040127156969 s), publishing
all 235 statistics frames at the pinned 0.0625 s cadence. Independent
readback verified every VDB SHA and v2 payload seal, all frame times within
1e-9 s of the authored cadence, and the terminal checkpoint, diagnostics,
summary, frame-times and sequence-manifest seals. This records the existing
cadence representation discrepancy; no solver tolerance was changed.

Wall time was 140637.04271045799 s (39.07 h). Over all accepted steps,
device/wall p95 were 7420.128501486033 / 9734.340792 ms (sorted sample at
floor(0.95*(N-1))). Peak velocity was 11.152058601379395 m/s; final velocity
7.2632589340209961 m/s. Peak recorded process RSS was 2409578496 bytes,
peak footprint 2308819584 bytes, compressed memory zero throughout.
The summary records zero hard-bound and physical-projection step retries;
this does not imply zero internal projection-work recovery attempts.

Payloads remain outside git under the r226 calibration directory. SHA256:

- `tier8_window.v1/production_capstone_summary.txt`:
  `1425ddf0be406e3dd3779e534b1f45cac8ab12eaa8604e17e0c8df3fd4492625`
- `tier8_window.v1/production_final.checkpoint`:
  `1fd89f38ea7a5bb3bef83dfd0d66a9852385fc606d3dccc6cecdf6570af4b011`
- `tier8_window.v1/frame_times.csv`:
  `6ac2fd488112dde0d5f3ad128ab0fc267545964d64bb5f5403b7b126b364479f`
- `tier8_window.v1/sequence_manifest.rise-fire.cbor`:
  `47655fb16eb0f3399bf02b684271cbc412d38ad0adee99fa98e361698b9c263d`
- `tier8_window.v1/monitored_diagnostics.csv`:
  `14d1f708fb743f4b92cc36453a0a71afce9352b9ab372e2297f2f66f93c1cceb`

The existing temporal preview renderer is now consuming this full sequence,
after the simulation worker exited. It preserves the pinned 64x64 preview
path (including its existing denoise-off setting), not a final-quality
spectral claim. Full centerline/lit-area spectra await those display frames.
The summary's single peak is not the requested spectrum verdict. Tier-8
empirical rows and the pending formal filtered-field contract are not claimed.
