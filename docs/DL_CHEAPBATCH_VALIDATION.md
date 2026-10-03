# debt-cheapbatch validation (2026-10-02)

Branched from master `a74c400b2`; changes remain in `debt-cheapbatch`.
Source red proofs, final full-suite variance and serial gates are recorded below.

## DL-311 stopped experiment

Excluding non-finite/infinite emitter power made the images finite, but
removed BDPT's camera-visible emitter coverage. Scale-2 infinite-plane
means were PT 0.453129, BDPT RGB 0.0305721 and BDPT spectral 0.0309033;
finite-area controls were 0.443173 / 0.443129 / 0.446933. Finishing this
requires the reserved BDPT integrator. The guard and transient test were
withdrawn; only the ledger disposition was committed.

## DL-332 initial slice calibration (superseded budgets below)

Part B now uses explicit, independent Sobol salts on the two sides of
BDPT/spectral pairs. RGB PT retains common random numbers. Standard error
(SE) below is the uncertainty of the default four-replicate mean, not a
single-render standard deviation.

| Row | spp | measured SE | retained/new band | band/SE |
|---|---:|---:|---:|---:|
| smooth BDPT | 512 | 0.004860 | 0.020 | 4.1 |
| rough BDPT | 2048 | 0.0005355 | 0.006 | 11.2 |
| dense smooth BDPT | 2048 | 0.002168 | 0.008 | 3.69 |
| random-walk BDPT | 128 | 0.007497 | 0.100 | 13.3 |
| smooth PT spectral | 256 | 0.007296 | 0.040 | 5.48 |
| random-walk PT spectral | 256 | 0.006355 | 0.050 | 7.87 |

The original-spp salted measurement used n=16 and took 496 seconds.
The raised-spp BDPT subset used n=4 and took 388 seconds (including its
matching Part E control). At this initial isolated calibration, historical dense DL-49 red 0.9839
was 7.4 estimated SE from 1; rough red 0.9929 was 13.3 estimated SE.
These estimates are superseded by the final full-suite measurements below. The 256-spp spectral
rows took 22.2/22.6 seconds. Restoring the original unsalted helper while
keeping the new capture contract failed 8/8 salt assertions (110/8);
restored committed helper passed 118/0. The initial red-proof filter
matched no rendered rows; it was corrected before accepting any evidence.

## DL-355 / DL-390 calibration

The explicit-salt helper previously had its salt overwritten by Render.
The corrected helper is protected by a distinct-capture regression.
Its first version falsely stayed green with the salt removed because other
RNG draws changed. The final version holds libc and cached global Mersenne
RNG state fixed, so the value salt is the only changed input. Shared SMS
paired/OIDN fixtures likewise reset both RNGs before scene load and render.
The committed zero-salt red failed 3/3 distinct-capture comparisons (4/3);
restored helper passed 7/0. Both builds were warning-free.
The composite BDPT row's actual n=16 single-render normalized ratio mean
was 0.994511 with sd 0.088761. Averaging n=32 predicts SE 0.01569; band
0.06 is 3.82 SE. Other scalar closed forms/layer rows average n=8; narrow
spot BDPT/VCM retain 1024 spp. An initial 8192-spp audit measured tiny
closed-spot error (BDPT gap-0.3 relative SE 0.000022), so the unnecessary
spp increase was withdrawn; the final 1024-spp audit is recorded below.
Every averaged row prints its measured SE;
ROI edge captures remain individual salted images.

The first full salted run passed 217/0 in 592.9 seconds. Composite BDPT
L/L0=0.09200, gap-image mean SE=0.0000193223 (normalized ratio SE about
0.0147), inside the derived 0.06 band. At 1024 spp, simple BDPT spot gap
0.1 normalized SE is about 0.00060, inside its 0.02 band. The layer
BDPT n=8 mean SE is 0.46% (box) / 0.69% (two planes), against 8% bands.
Corrected n=4 fov audit is in DL294's closure; Gaussian sample sds at
fov 1/2/3/5/10 are 0.0086764 / 0.0018315 / 0.0004946 / 0.0005194 /
0.0005235 (ratios), preserving >=3 sd margins at every retained band.


## DL-360 policy

Auto resolves from pixel count, configured sample budget and a fixed
rasterizer-family quality policy. The constants are policy weights, not
hardware performance measurements. Adaptive budgets use the maximum of
the configured sampling and adaptive target. Region and direct-companion
paths use the same family policy. The implementation carries spp times
family weight directly in per-megapixel policy units, avoiding area
multiply/divide roundoff at preset thresholds. Explicit quality settings retain their
meaning. Eight fabric replays hold raw input constant and alternate an
8-second intermediate-output delay, which perturbs the old clock policy. Auto must also equal a pinned Balanced
reference (64 spp times legacy policy weight 0.1 gives r=6.4), so an old
clock that picks High on every replay cannot produce a false green.
Measurement tests disable OIDN; FrameStore's denoise test pins Fast/CPU.
AgentEvalCheckTest's render fixtures explicitly disable OIDN.

Green: 51/0 in 133.02 seconds. All eight Auto frames have composited
RGB-average mean 0.0496695, raw FNV hash 4322452475926386811 and final hash
10375815737409270508, matching the explicit Balanced reference. Delay
execution and a nonempty raw capture are checked on every replay.


## DL-340 / DL-347 / DL-353

Medium scatter clears the SMS anchor in the shared RGB/NM loop and HWSS
medium-walk handoff. Emitter evaluation reconstructs the sampled surface
payload and evaluates its own RGB/NM emission toward the solved chain.
The ledger's suggested minus sign is reversed: `mkVector3(b,a)` is `b-a`,
and the existing variable `dirSpecToLight` already points LIGHT TO CHAIN.
A diagnostic confirmed +Y normal and +Y outgoing direction when the
incorrect negation was used on the turned emitter; the diagnostic was
removed after correcting all four sites.

Uniform spectral eta override updates the material side of etaI/etaT,
matching the companion replay and retaining the seeded exterior. Nested
dispersive exterior handling remains the existing companion contract and
is separately filed as DL-391 (code-confirmed, not render-measured). Its
resolution needs wavelength stack propagation, outside the requested small
material-side override. The user's design/refactor stop rule applies.

Initial n=4, 512-spp centroid measurements (pixels):

| mode | constant-index shift / sd | N-SF11 shift / sd |
|---|---|---|
| NM | 0.000202 / 0.000237 | -0.037250 / 0.000302 |
| HWSS | -0.000417 / 0.000271 | -0.036869 / 0.000590 |

The constant band 0.005 and dispersive displacement floor 0.02 separate
the controls by many standard errors. The final test uses 128 spp, with
its actual spread recorded in the gate results.

Final-budget measurements before source A/B:
- DL-340 HWSS at 256 spp: no slab 1.00393 (sd 0.016029), slab
  1.00058 (sd 0.00701908), n=4. Combined mean SE 0.008750 =
  sqrt(sd_no^2+sd_slab^2)/2; parity band 0.03 is 3.43 SE.
- DL-347 eight turned-emitter combinations each have ratio 1 and sd 0,
  with common random inputs within each on/off pair and four independent
  pairs; 168/0. Exact equality is expected when no SMS term is physically
  allowed; its source A/B below determines whether this is a false green.
- DL-353 final 128-spp shifts: NM constant -0.00000808026
  (sd 0.000000543491), SF11 -0.0372319 (sd 0.000535546);
  HWSS constant -0.00000825263 (sd 0.00000269954), SF11 -0.036822
  (sd 0.000387248). n=4; 22/0. The dispersive floor 0.02 is over
  64 standard errors from the closest measured mean; constant band 0.005
  is over 3700 standard errors. Shipped legacy/uniform smoke 10/0.

Self-audit surfaces for independent review: the light-to-chain direction
sign and all four emission call sites; surface payload/sidedness of area
emitters; explicit versus legacy IOR fields at entry/exit; clearing every
medium SMS hand-off without changing covered surface anchors; static OIDN
policy at regions/animation/direct companion; shared RNG controls and
whether regressions fail with old sources; Monte Carlo bands and the
separate nested-exterior residual. Static OIDN weights are policy choices,
not measured performance claims. Timing numbers here are observed test
runtimes only.

## Master-source red proofs

DL-360: restoring the 13 rendering source/header files from master gives
43/8 in 160.70 s. All raw hashes stay 4322452475926386811, but the old
clock policy selects High and produces hash 8833268842144745444 instead
of the explicit Balanced reference 10375815737409270508. Restoring HEAD,
rebuilding with zero warnings and rerunning gives 51/0 in 165.47 s.

DL-340: master PathTracingIntegrator.cpp gives 24/3 in 565.54 s:
slab/no-slab ratios RGB 0.955163/1.00756, NM 0.942610/1.00556,
HWSS 0.949211/1.00393. All three parity assertions fail. Control values
match the fixed-source inputs; the branch source was then restored.
Restored DL-340 HEAD rebuilt without warnings and passed 27/0 in 528.08 s;
RGB/NM/HWSS values reproduce the calibrated measurements above.

DL-347: master ManifoldSolver.cpp gives 162/6 in 83.87 s. Turned
Phong ratios RGB Snell/uniform 6.90236/1.82746, NM Snell/uniform
2.07975/1.15776; all four implementation paths fail their tests.
Lambertian RGB Snell/uniform 1.31363/1.04778 and NM 1.06652/1.00975;
the two smaller uniform defects are inside the 0.05 band, but the Phong
sibling catches each site. Fixed green is exactly 1 on every pair, with
sample sd 0. The 0.05 band is above three measured green standard errors;
it is not asserted to detect every small old Lambertian defect alone.

DL-353: master ManifoldSolver.cpp gives 20/2 in 253.54 s. Old SF11
centroid shifts NM +0.00356691 (sd0.000555835), HWSS +0.00394549
(sd0.000326965), both below 0.02. Constant-index controls are unchanged.
Master's solver includes unrelated DL-345 changes beyond the branch point;
the mandatory master-source red is retained here, while canonical pre/post
controls below use branch-point a74c400b2 for an isolated comparison.

Restored DL-347 and DL-353 HEAD rebuilt without warnings and passed
168/0 in 58.64 s and 22/0 in 261.93 s respectively, reproducing all
fixed-source values above.

## Canonical isolated controls

Using branch-point a74c400b2 versus fixed HEAD, with only the solver
source switched, all four canonical scenes at seeds3400/4400/5400
are pixel-hash identical: 12/12 pairs. Baseline/fixed calls alternate for
each seed. Each binary rebuilt with zero warnings; each invocation
passed4/0. Runtime per four-scene invocation12.97–13.30s.

CST integration: preliminary coverage gate457 MATCH/0 DRIFT/1 UNCOVERED
identified only the newly added uniform twin. Reviewed regeneration adds
exactly its one manifest row, no existing digest changes. Its object-state
digest matches the original scene because the geometry/materials are
identical; rasterizer-mode behavior is covered by the shipped smoke.
Final clean gate below includes the updated458-entry manifest.

## Full-suite calibration adjustment

The first preliminary full SSS run passed406/0 in465.14s. The second
was intentionally stopped at385.63s (own verified PID15570, SIGTERM)
after decisive Part B evidence; it is not a full gate pass. Spectral
diffusion at256 spp gave0.981429 and1.02423 in the first two full-suite
seed groups. Even if three remaining means were their midpoint, n=5
sample SD would be at least0.015132, so band0.04 cannot be3 SD.
Full-suite rough/dense BDPT at2048 spp also printed mean-SE estimates
0.00292615/0.00284663: bands0.006/0.008 are only2.05/2.81 SE.
The isolated slice underestimated that variance. Raise rough/dense BDPT
to8192/4096 spp and spectral diffusion to1024 spp; keep every band
unchanged. Final five full-suite results below validate the new budgets.
Random-walk spectral remains256 spp; smooth BDPT512 and random-walk
BDPT128. The five completed weave gates (217/0 each) remain valid: its
test and executable renderer behavior did not change in this adjustment.
API follow-up only replaced comments; final build refresh includes it.

## Withdrawn RNG audit (DL-392 candidate)

A code audit proposed controlling libc before scene load and cached global
Mersenne state in RenderMean. The first increased-budget full SSS run was
stopped intentionally after233.91s (own verified PID38947), not a gate pass.
Two candidate four-render fixed-seed contracts varied caller RNG streams.
Both stayed green with the original helper: BDPT rough mean
0.010877361946996062 and legacy PixelPel dipole0.0054271243319558016,
117/0 each. They did not establish a defect. The helper change, contract
and candidate ledger row DL-392 were withdrawn; no causal claim attributes
the full-suite spread to those RNG streams. Retain the measured-budget
adjustment and existing explicit value-salt contract. At that audit point, the new rows were
DL-390 and DL-391 only; review subsequently added DL-393 and DL-394.


## Final five-run Monte Carlo gate

Each full run uses a distinct seed base; each tested default SSS ratio
averages four independently salted renders per side. The SD below is
measured directly across five full-run tested means, rather than inferred
from square-root sample scaling or one run's local delta-method estimate.
Some individual local SE estimates are larger; these empirical five-run
SDs describe the tested statistic and are not a universal variance bound.

| SSS row | final spp | five tested ratios | sample SD | band | band/SD |
|---|---:|---|---:|---:|---:|
| smooth BDPT | 512 | 0.989989, 1.005970, 1.005010, 0.998938, 1.001830 | 0.00641939 | 0.020 | 3.12 |
| rough BDPT | 8192 | 1.000150, 1.000750, 1.001050, 1.000510, 1.000440 | 0.000338821 | 0.006 | 17.71 |
| dense smooth BDPT | 4096 | 0.997308, 0.999644, 1.002450, 1.002760, 0.999146 | 0.00231197 | 0.008 | 3.46 |
| random-walk BDPT | 128 | 0.990527, 0.993888, 0.999516, 1.013200, 0.991854 | 0.00926903 | 0.100 | 10.79 |
| smooth PT spectral | 1024 | 0.997182, 0.999543, 1.008230, 1.004790, 1.004640 | 0.00444328 | 0.040 | 9.00 |
| random-walk PT spectral | 256 | 0.984862, 1.006510, 0.997813, 0.984752, 1.001690 | 0.00991049 | 0.050 | 5.05 |

Historical dense 0.9839 is 6.96 final SD from 1; rough 0.9929 is
20.96 SD from 1 and 3.25 SD below the lower acceptance edge. The original
DL-49 physical reds (0.62–0.66 and 0.89–0.91) remain many SD outside.
SSS seed offsets 0/1000/2000/3000/4000 each passed 406/0 in
1119.27/1167.01/1178.21/1176.33/1119.20 seconds. The earlier 465.14-second
preliminary pass used smaller budgets; these are observed whole-test costs,
not a controlled benchmark of the individual changes.

Weave bases 1000/2000/3000/4000/5000 each passed 217/0 in
625.98/613.48/607.29/614.11/601.66 seconds. Its n=32 composite BDPT
normalized means were 1.02215, 1.01815, 0.98449, 0.98443, 0.99486:
sample SD 0.018208, retained band 0.06 = 3.30 SD. The narrow-fov gap
error SDs across five full runs were 0.80565/0.24952/0.54308/0.16822
percentage points for BDPT/VCM/merging-off/Gaussian; their retained
3/2/3/3-percent bands exceed 3 SD. These five runs predate comment-only
API corrections and the SSS-only budget adjustment; their executable
Weave behavior is unchanged on the final production tree.


## Pre-review clean build and serial integration gate

Clean library rebuild (`make -C build/make/rise clean`, then `-j8 all`)
completed in 68.69 seconds with zero warnings. All 17 test targets were
built individually, checking successful make exit and zero warnings before
execution. The later SSS-only budget/helper withdrawal was rebuilt with
zero warnings before its final five runs. Production source is unchanged
since the clean rebuild; no builds overlapped any test execution.

| Gate | passed / failed | runtime seconds |
|---|---:|---:|
| SourceHygieneTest | 167 / 0 | 1.21 |
| CstDeriveGoldenTest | 458 MATCH, 0 DRIFT, 0 UNCOVERED, 0 STALE | 29.74 |
| ManifoldSolverTest | all assertions pass | 0.01 |
| WeaveGapShadowTransmittanceTest | 217 / 0 each, five bases | 3062.52 total |
| SSSExteriorIndexInvarianceTest | 406 / 0 each, five offsets | 5760.02 total |
| ExteriorIndexInvarianceTest | 231 / 0 | 447.71 |
| PTGuidingMISPartitionTest | 185 / 0 | 50.29 |
| MediumInsideOutsideInvariantTest | 52 / 0 | 489.14 |
| OIDNAutoDeterminismTest | 51 / 0 | 130.98 |
| SMSMediumAnchorTest | 27 / 0 | 520.50 |
| SMSEmitterDirectionTest | 168 / 0 | 60.59 |
| SMSUniformDispersionTest | 22 / 0 | 238.11 |
| SMSUniformDispersionTest --shipped | 10 / 0 | 0.24 |
| SSSRadianceScalingTest | 576256 / 0 | 72.92 |
| DoubleSidedEmitterTest | 34 / 0 | 14.55 |
| FrameStoreTest | 123 / 0 | 0.77 |
| RasterizerDefaultsConsistencyTest | 164 / 0 | 0.36 |
| AgentEvalCheckTest | 2075 / 0 | 50.09 |

Source proofs and canonical controls are recorded above. The independent
review verdict is reported with the final branch HEAD after this record
is committed, so no later documentation edit can stale that verdict.


## Independent review round 1 and repairs

Round1 on `ddc418273` found two P1s: missing UV in uniform/Snell spectral
material queries, and full/region Auto threshold roundoff. Its third
contract/cost lens found no P1/P2. The test lens also requested broader
policy coverage (P2). Fixes are filed as DL-394 and DL-393 respectively.
Uniform and Snell companion rigs now pass vertex UV; the companion also
passes geometric normal. OIDN carries the algebraically cancelled
per-megapixel policy rate directly through every caller, avoiding
area multiply/divide threshold drift. Nonvirtual diagnostic accessors
expose actual configured family rates and the last configured OIDN preset.

New UV regression uses an origin-only low-IOR texel with literal1.78
elsewhere; four paired salts for each NM/HWSS and uniform/Snell combination
must match the constant 1.78 pixel hash. Initial arithmetic/stripe controls
were rejected because they were not identical physical inputs on every
path. Final fixed32/0; restoring pre-review solver `ddc418273` while
retaining new assertions gives16/16 (all 16 hash comparisons fail).
Examples: HWSS uniform mean0.446594 vs0.417150 with old query context.
The dispersion portion resets its salt index to preserve its original
calibration; UV controls are a separate paired-input contract.

OIDN threshold regression checks configured preset AND output against
explicit presets at rates2.8/3/3.2/19.2/20/20.8, full/cropped19x24 and16x32.
Pixel-only version red20/4, green24/0 exposed a coverage limit: Balanced
and High can produce identical CPU pixels on these small inputs. Adding
a read-only last-configured-preset accessor strengthens it to red28/20,
green48/0. Red restores all pre-review rendering source files; only the
read-only preset accessor is retained as instrumentation. Further cheap
scene-loading checks cover ten rasterizer variants, family weights and
configured/adaptive max budgets. No renders are needed for those checks.
All preliminary repair builds were successful and warning-free.
Final restored-source clean re-gate follows; fresh review will inspect
its committed HEAD rather than reuse round1 verdicts.


The first post-review OIDN full run stopped the serial gate at117/22:
family scene loading initialized cached options before Render's worker
setup, so raw replay inputs varied (e.g. replicate7 raw6645350380224412891).
This was an invalid clock-comparison fixture, not an Auto policy failure.
Move ConfigureTestWorker to main's start, before any job/scene load;
rebuild and resume the remaining serial gate. Earlier completed tests and
production source are unchanged; they are not rerun for this test-only
precondition repair. The stopped attempt is not counted as a pass.


## Post-round1 integration gate (before round2 repairs)

Fresh clean library build67.91s and all17 serial test builds succeeded
with zero warnings. Every integration test was repeated once on the
repaired production tree; the earlier five independently seeded full-run
measurements remain the variance calibration. Their budgets, bands and
Weave/SSS test behavior did not change in the review repairs. The OIDN
fixture-only configuration fix was rebuilt with zero warnings before
resuming the serial gate. No build overlapped a test execution.

| Gate | passed / failed | runtime seconds |
|---|---:|---:|
| SourceHygieneTest | 167 / 0 | 1.53 |
| CstDeriveGoldenTest | 458 MATCH, 0 DRIFT, 0 UNCOVERED, 0 STALE | 31.93 |
| ManifoldSolverTest | all assertions pass | 0.53 |
| WeaveGapShadowTransmittanceTest 1000 | 217 / 0 | 614.29 |
| SSSExteriorIndexInvarianceTest --seed 0 | 406 / 0 | 1125.52 |
| ExteriorIndexInvarianceTest | 231 / 0 | 460.01 |
| PTGuidingMISPartitionTest | 185 / 0 | 44.08 |
| MediumInsideOutsideInvariantTest | 52 / 0 | 465.86 |
| OIDNAutoDeterminismTest | 139 / 0 | 138.44 |
| SMSMediumAnchorTest | 27 / 0 | 534.90 |
| SMSEmitterDirectionTest | 168 / 0 | 58.64 |
| SMSUniformDispersionTest | 54 / 0 | 275.09 |
| SMSUniformDispersionTest --shipped | 10 / 0 | 0.28 |
| SSSRadianceScalingTest | 576256 / 0 | 86.74 |
| DoubleSidedEmitterTest | 34 / 0 | 17.78 |
| FrameStoreTest | 123 / 0 | 0.71 |
| RasterizerDefaultsConsistencyTest | 164 / 0 | 0.25 |
| AgentEvalCheckTest | 2075 / 0 | 64.69 |

All32 UV checks pass in the expanded dispersion gate, whose22 original
wavelength/scene checks still reproduce the calibrated means. The shipped
smoke remains separate10/0. OIDN139/0 comprises40 scene-load/rate checks,
48 boundary/preset/output checks and51 original fabric replay checks.
The raw/final fabric hashes remain4322452475926386811 /
10375815737409270508. Header-only comment cleanup after this gate clarifies
that buffer converters remain available without OIDN, and that cached
preset diagnostics must be read while idle (High before initial setup).
It changes no compiled behavior. Library dependency rebuild and final
SourceHygiene check are recorded before the committed-head fresh review.

Final header-comment dependency rebuild succeeded with zero warnings;
SourceHygieneTest rebuilt successfully with zero warnings and passed167/0
again after the worker-setup ordering repair. That committed tree was presented to fresh round2 reviewers; their findings
and the subsequent gate are recorded below.


## Independent review round 2 and repairs

Fresh read-only math, test/statistics and public-contract reviewers examined
`b138872483861433a40223f24651e44aca89bef0`. Math found two P1 query-context
gaps: uniform/Snell NM records lost exact object-frame Po, and photon
reconstruction lost UV (and Po in the sibling audit). These are distinct
from the explicitly stopped nested wavelength/exterior stack DL-391.

DL-395 carries the captured hit Po through seed creation, surface updates
and PT chain records; photon deposits now retain both UV and Po. All RGB
and NM photon material queries restore them. The Snell NM photon path
uses the shared reconstruction helper's wavelength override. Exact captured
Po is used rather than inverse-transforming world position, which would
lose the winning-child coordinate frame for CSG. This fixes the origin
query bug; it does not claim general spatial-IOR Newton/Jacobian support.

The render regression extends the origin-only UV fixture with a Po-origin
scalar expression: all sampled pane points have literal IOR 1.78, while the
synthetic origin has 1.1. Four paired salts per NM/HWSS and uniform/Snell
combination compare exact pixel hashes with constant 1.78, with no noisy
band. Together UV and Po checks pass 64/0. A deterministic ManifoldSolver
control invokes the production photon reconstruction at RGB and 450/650nm
with a context-sensitive scalar material; it asserts restored coordinates,
RGB attenuation and NM IOR. This directly tests the material query and is
not a measured photon-map yield or accepted-additional-root render.

Contract review also found the pre-existing OIDN warmed-backend selector
defect (DL-397, P2): device resolution happened only once regardless of
later CPU/GPU/Auto requests. The fix releases filter/aux/buffer references
before replacing the device, retaining staging vectors because their raw
pointers may already be Denoise inputs. Unchanged requests keep the device
across quality changes. Read-only diagnostics expose actual resolved
CPU/GPU and successful device-creation generation. Tests reuse one context
through Fast/Balanced/High/Fast, comparing presets and exact outputs with
fresh explicit contexts, then request GPU/CPU/Auto/CPU and compare actual
backend/output with fresh resolution. The install reports GPU unavailable:
CPU fallback/re-resolution is verified; an actual GPU-to-CPU transition is
UNMEASURED. Focused policy/family/cache coverage is 112/0.

The stale Auto enum comment and current log contract were corrected.
Historical measured wall-clock timings/log quotations remain historical.
The emitter review's missing pure-SPD light support is recorded as STOPPED
DL-396 under the user's design/reserved-file rule: selection power must be
defined consistently across all MIS callers and photon emission, including
reserved BDPT zero-RGB light coverage. This code-confirmed defect is
UNMEASURED; the directional regression gates turned RGB-white emitters,
not a forward-Phong or physical-SPD caustic. Native NM Le evaluation is
fixed only for emitters that the selector admits.

Two intermediate test compilation attempts were rejected (ScalarTriple
access and Denoise argument order); no stale test binary was executed after
either failed build. Successful focused library/test builds have zero
warnings. Round2 fixes were committed as 628d706ee (DL-395) and 51bbc63f5
(DL-397) before the source-reversion proof.


Round2 source proof restored these three implementation files from
pre-round2 HEAD b138872483861433a40223f24651e44aca89bef0: ManifoldSolver.cpp,
SMSPhotonMap.cpp and OIDNDenoiser.cpp. New assertions were retained. The
new coordinate storage/header layout stayed as compile instrumentation;
the old solver still neither populated nor queried those fields. The
photon helper retained only its new wavelength argument/dispatch so the
old origin query could be tested at 450/650nm; no coordinate restoration
was retained. OIDN retained only actual-device/generation diagnostics and
the increment at successful device creation, with its old lifetime cache.
Thus the original defects remained while the tests could compile.

The restored source library and all three test builds succeeded with zero
warnings. Photon RGB attenuation asserted at the wrong origin and aborted
(exit -6); UV/Po controls failed all 16 Po hash comparisons, 48/16 overall
(20.37 s). Example HWSS/uniform control mean 0.447685 versus old Po mean
0.418366. OIDN 112 focused checks returned 108/4 (0.48 s), one missed device
re-resolution per changed request; warmed quality transitions still passed.
Fixed focused results before reversion were all photon assertions passed,
64/0 UV/Po and 112/0 OIDN. All three sources were restored from HEAD before
the final clean rebuild/full gate below.


The seed-context sibling audit also files DL-398 as a design STOP under
the user's scope rule. NM material overrides occur before Solve; Newton
geometry updates do not re-query indices or attenuation as positions change.
Thus arbitrary spatial-IOR/attenuation painters are not generally evaluated
at a moved root. Supporting them needs wavelength/exterior-stack state inside
the solve, final-root attenuation queries, and index-gradient Jacobian terms,
consistently in companion replay.
This is code-confirmed and UNMEASURED; the constant-interior UV/Po controls
prove seed-query coordinate restoration, not this broader solver contract.


## File manifest by item

All paths below are repository-relative. Ledger/validation and tests/README
are shared records; CLAUDE contains one dated behavior sentence. No Library
file was added or removed, so the five-project source-list rule is not
triggered. No reserved implementation file was changed.

| Item | Implementation / test / fixture files |
|---|---|
| DL-311 stopped | docs/DEBT_LEDGER.md; this validation record. LightSampler guard and transient test withdrawn. |
| DL-332 | tests/SSSExteriorIndexInvarianceTest.cpp; shared records. DL-392 helper experiment withdrawn. |
| DL-355 / 390 | tests/WeaveGapShadowTransmittanceTest.cpp; docs/DL294_NARROW_FOV_SPLAT.md; shared records. |
| DL-340 | src/Library/Shaders/PathTracingIntegrator.cpp; tests/SMSMediumAnchorTest.cpp; tests/SMSRenderTestSupport.h; shared records. |
| DL-347 | src/Library/Utilities/ManifoldSolver.cpp; tests/SMSEmitterDirectionTest.cpp; tests/SMSRenderTestSupport.h; shared records. |
| DL-353 / 394 | src/Library/Utilities/ManifoldSolver.cpp; tests/SMSUniformDispersionTest.cpp; tests/SMSRenderTestSupport.h; scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene; scenes/Tests/README.md; tests/data/cst_derive_golden.txt; shared records. |
| DL-395 | src/Library/Utilities/ManifoldSolver.cpp and .h; SMSPhoton.h; SMSPhotonMap.cpp; tests/ManifoldSolverTest.cpp; tests/SMSUniformDispersionTest.cpp; shared records. |
| DL-397 | src/Library/Rendering/OIDNDenoiser.cpp and .h; src/Library/Utilities/OidnConfig.h; tests/OIDNAutoDeterminismTest.cpp; docs/OIDN.md; shared records. |
| DL-399 | tests/ExteriorIndexInvarianceTest.cpp; shared records. |
| DL-391 / 396 / 398 stopped | docs/DEBT_LEDGER.md; this validation record. No implementation patch for the stopped contracts. |

DL-360 / 393 files (including API comment consistency):

- src/Library/Rendering/OIDNDenoiser.cpp, OIDNDenoiser.h,
  PixelBasedRasterizerHelper.cpp, PixelBasedRasterizerHelper.h, Rasterizer.h,
  PathTracingPelRasterizer.cpp, PathTracingPelRasterizer.h,
  PathTracingSpectralRasterizer.h, BDPTRasterizerBase.cpp,
  BidirectionalRasterizerBase.h, MLTRasterizer.cpp, MLTRasterizer.h,
  MLTSpectralRasterizer.cpp, MLTSpectralRasterizer.h.
- src/Library/Utilities/OidnConfig.h; src/Library/RISE_API.cpp and RISE_API.h;
  src/Library/Job.cpp and Job.h; src/Library/Interfaces/IJob.h.
- tests/OIDNAutoDeterminismTest.cpp, SourceHygieneTest.cpp,
  SMSRenderTestSupport.h; docs/OIDN.md; shared records.


## Stopped post-round-2 gate and exterior fixture audit

The clean post-round-2 gate rebuilt the library in 70.88 s with zero
warnings and built all 17 targets individually with zero warnings.
Source167/0, CST458 MATCH/0 DRIFT/0 UNCOVERED/0 STALE, all Manifold
assertions, Weave217/0 (618.04 s) and SSS406/0 (1145.77 s) passed.
It then stopped at Exterior230/1 (437.40 s): glass-block SMS/VCM
1.06310 with estimated mean SD0.0565725 exceeded [0.842,1.040].
This attempt is a failed gate, not an integration pass. Its logs and JSON
are `/tmp/cheapbatch-review3-gate-*` and `/tmp/cheapbatch-review3-gate.json`.

The DL-399 candidate audit controls worker count before cached options load,
libc and global Mersenne state before both scene construction and rendering,
and Sobol value salts for all repeated renders. Four independently salted
8-spp replay pairs per shipped row compare the full numeric RGB-sum image.
All glass-block replay pairs passed, but the controlled 256-spp comparison
gave ratio1.00502, estimated mean SD0.0636306 (111/1 with the new
precision assertion). The 1024-spp comparison gave1.05370, SD0.0264142
(110/2). These pilots demonstrate inadequate precision, not a production
solver defect. No band is justified by repeating the same Sobol pattern.

A higher-budget pilot restricts only SMS eye work to the rectangle containing
the unchanged slab mask. `RasterizeRegion` preserves the full film and camera
coordinates; pixel sample keys use absolute film coordinates. OIDN is disabled.
VCM keeps its complete film, light passes, normalization and auto-radius.
This changes legacy RNG consumption; cropped and full numeric images are not
claimed identical. Calibration must measure the cropped fixture directly.
The initial candidate needed the sampling adjustments recorded below;
mutation proof and the final clean gate are recorded after calibration.

The first controlled full exterior calibration exposed Part B RGB
uniform/SMS glass-sphere k1 at16 spp: ratio0.975822, uncorrelated
mean-SD estimate0.0232611, versus its unchanged0.01 band. Other
completed Part B rows passed, including NM/uniform0.999912 and
RGB/uniform k2 ratio1. This calibration is not a successful gate.
Its Part C flat-slab measurement is retained; repeated glass-block
work is intentionally avoided because the completed16384-spp pilot
already measured the same controlled inputs. The next fixture revision
uses covariance-aware paired uncertainty for Part B, checks precision
against each gated row's band, and preserves full-suite row seeds when
filtering focused runs.

The controlled full calibration was intentionally terminated on verified
own render PID93419 after545.96 s, after flat-slab and glass-block replay
checks, with two known failures (RGB uniform k1 mean and flat-slab precision).
It has no full pass count. Logs were renamed
`/tmp/cheapbatch-dl399-full-calibration-stopped.*` to prevent a green label.
Flat-slab4096/512 measured ratio1.00050, estimated mean SD0.00551936:
VCM supplies most variance, so increase only its budget to1024.
The1024-spp focused RGB uniform k1 run measured0.989407 and covariance-aware
mean SD0.00577986 (103/2,161.95 s under the old0.01 band).
Derive a0.02 band (3.46 estimated SDs) from these four salted paired inputs,
while retaining1024 spp. This band change is measured, not an arbitrary
widening; the16-spp controlled ratio0.975822 still fails its mean threshold
and its much larger uncertainty fails the precision threshold.
Part B pairs used residual variance of scaled_i-ratio*air_i to retain
covariance; the Part C pilot values above used an uncorrelated delta-method
approximation. The final fixture correction below supersedes that assumption.
These n=4 estimates are not claimed as the five-independent-default-statistic
calibration performed for the user-requested SSS/Weave rows.

## DL-399 sampling-budget calibration (Part C pilot covariance approximation)

Every row below uses four independently salted pairs. Uncertainty is the
estimated SD of the ratio of default means, not a single-render SD.

| Row | SMS / reference spp | Ratio | estimated mean SD | Band / SD |
|---|---:|---:|---:|---:|
| RGB uniform glass-sphere k1 |1024 /1024|0.989407|0.00577986|0.02 / SD =3.46|
| shipped flat-slab |4096 /1024|1.00333|0.00185025|0.036 / SD =19.46|
| shipped glass-block |16384 /512|1.01021|0.00475053|0.040 upper margin / SD =8.42|

Flat-slab focused checks passed112/0 in226.80 s. Glass-block16384
focused checks passed112/0 in its completed pilot. Both shipped bands
also leave more than3 SD after reserving the historical2% reference
allowance (8.65 and4.21 SD). RGB uniform k1's earlier103/2 result
used0.01; its measured0.02 threshold is validated in the final gate,
rather than inventing a rerun count. The original low-budget inputs
are restored only after committing the new assertions, below.

## DL-399 committed mutation proofs

After commit3c6014900, the test retained all new assertions while restoring
original helper inputs (no worker pin, no pre-load/global RNG reset, fallback
Sobol salt0). Its build succeeded with zero warnings. Four8-spp replay
pairs failed all four equality assertions:103/4 in1.46 s.

Restoring committed inputs, then mutating only budgets back to original
RGB uniform k1=16, shipped SMS=256 and VCM=512, kept the newly measured
0.02 RGB band and all precision assertions. The SMS subset returned191/4
in334.70 s: RGB uniform ratio0.975822, paired mean SD0.0235596 failed both
mean and precision; flat-slab1.01377, estimated SD0.0191912 and glass-block
0.958070, estimated SD0.0315601 failed precision despite passing their
physics means. Thus these assertions detect under-sampling independently
of a lucky mean. Build exit0, zero warnings.

The test source was restored from HEAD, rebuilt with exit0/zero warnings,
and its replay checks passed107/0 in3.79 s. Whole-fixture green follows
in the final integration gate. Proof logs/JSON:
`/tmp/cheapbatch-dl399-proofs.json` and its listed log files.

## ASan/UBSan focused gate

A separate clean build instrumented every library object and the three
focused test targets with AddressSanitizer and UndefinedBehaviorSanitizer.
All builds exited0 with zero warnings; the library build took140.90 s.
OIDN policy passed112/0 (1.22 s), all Manifold assertions passed (0.32 s),
and UV/Po controls passed64/0 (130.15 s). No sanitizer report occurred.
UBSan was configured to halt on a finding; leak detection was disabled
on macOS. Prebuilt external dependencies were not instrumented.
This validates the exercised query/cache paths, not every renderer path.
Logs and build/run timings are in `/tmp/cheapbatch-sanitizer.json`.
The normal integration gate performs another clean rebuild afterward,
so it does not execute sanitizer binaries by accident.

## DL-399 covariance correction before shipped final-gate execution

Self-audit found that Part C pairs share legacy RNG inputs despite distinct
Sobol salts. Ignoring covariance is therefore not generally justified.
A common PairedRatioMeanSD helper now uses residual variance for both Parts
B and C. Two deterministic four-sample controls pin the statistic:
proportional pairs have zero uncertainty; perfectly anti-correlated
means2.5 with sample variances5/3 have ratio variance4/15. The old
uncorrelated formula produces a false nonzero proportional uncertainty
and understates the anti-correlated uncertainty.

The integration runner pauses between tests while the existing SSS child
continues; no build overlaps that render. The affected exterior target
is rebuilt before its first final-gate execution, and SourceHygiene is
rescanned after the correction. Production sources remain identical to
the successful clean rebuild. The pilot estimates above remain historical
and are not called the final paired SDs; final measured C values follow
below. Mutation proof is recorded after committing this correction.

The covariance correction was committed as2c56071d4 before mutation.
Retaining its helper API and both unit controls, replacing only the helper
body with the original Part C uncorrelated formula gave99/2 in3.04 s.
The proportional and anti-correlated checks both failed. Both old/restored
builds exited0 with zero warnings (27.45/26.85 s). Restoring HEAD passed
101/0 in0.49 s. Logs: `/tmp/cheapbatch-dl399-covariance-proof.json`.
The verified paused integration runner then resumed before Exterior's
first execution. SSS completed406/0; its1344.34-second wrapper wall
time includes the post-child collection pause for this test-only proof,
so it is not a renderer-runtime benchmark.

## Final paired Exterior result

The affected target was rebuilt with zero warnings before its first
integration execution. It passed275/0 in1259.75 s: the24 gated
Part B rows pass covariance-aware precision checks,16 shipped replay
checks pass, both shipped physics/precision comparisons pass, and the
two covariance controls pass. The non-gated photon residual and two
printed known-failure configurations remain explicitly non-gated.

| Statistic (four independently salted pairs) | Mean ratio | paired mean SD | band / SD |
|---|---:|---:|---:|
| RGB uniform glass-sphere k1 (1024 spp) |0.989407|0.00577986|0.020 / SD =3.46|
| flat-slab (SMS4096 / VCM1024) |1.00333|0.00161672|0.036 / SD =22.27|
| glass-block (SMS16384 / VCM512) |1.01021|0.00477052|0.040 upper margin / SD =8.38|

After reserving the historical2% reference allowance, flat/glass upper
margins cover9.90/4.19 paired mean SDs, both above3. These are n=4
delta-method estimates from paired residuals; they are not a universal
variance bound or the separate five-default-statistic calibration used
for DL-332/DL-355. Correcting covariance changes only the reported
uncertainty and precision guard, not the rendered means.

## Final normal integration gate

The clean library rebuild and all17 sequential test builds exited0 with
zero compiler warnings. The test-only covariance correction was rebuilt
before Exterior's first execution; production sources match the clean build.
All18 serial runs exited0. No render overlapped a build.

| Test / arguments | Result | Wrapper seconds |
|---|---:|---:|
| SourceHygieneTest | 167/0 | 1.46 |
| CstDeriveGoldenTest | 458 MATCH; 0 DRIFT/UNCOVERED/STALE | 30.67 |
| ManifoldSolverTest | all assertions passed | 0.27 |
| WeaveGapShadowTransmittanceTest 1000 | 217/0 | 617.93 |
| SSSExteriorIndexInvarianceTest --seed 0 | 406/0 | 1344.34 |
| ExteriorIndexInvarianceTest | 275/0 | 1259.75 |
| PTGuidingMISPartitionTest | 185/0 | 44.5 |
| MediumInsideOutsideInvariantTest | 52/0 | 445.03 |
| OIDNAutoDeterminismTest | 163/0 | 141.0 |
| SMSMediumAnchorTest | 27/0 | 522.24 |
| SMSEmitterDirectionTest | 168/0 | 56.73 |
| SMSUniformDispersionTest | 86/0 | 262.04 |
| SMSUniformDispersionTest --shipped | 10/0 | 0.26 |
| SSSRadianceScalingTest | 576256/0 | 77.32 |
| DoubleSidedEmitterTest | 34/0 | 14.94 |
| FrameStoreTest | 123/0 | 0.73 |
| RasterizerDefaultsConsistencyTest | 164/0 | 0.28 |
| AgentEvalCheckTest | 2075/0 | 49.75 |

SSS wrapper1344.34 s includes the collection pause described above;
its render log spans1126.87 s. Weave base1000 and SSS seed0 repeat earlier
calibration inputs and are not additional independent calibration samples.
CST covers465 corpus entries, including7 expected negatives.
After every render finished, SourceHygiene was rebuilt (exit0, zero warnings)
and rescanned the final source:167/0 in1.21 s,445 test files.
Gate logs: `/tmp/cheapbatch-review4-gate.json`; final rescan logs:
`/tmp/cheapbatch-final-source-build.log`, `/tmp/cheapbatch-final-source-run.log`.
Final review verdict is reported against the resulting full HEAD; no
post-verdict edit is used to record that verdict in this committed document.
