# debt-cheapbatch validation (2026-10-02)

Branched from master `a74c400b2`; changes remain in `debt-cheapbatch`.
Source red proofs, final full-suite variance and serial gates are recorded below.

## DL-311 stopped experiment

Excluding non-finite/infinite emitter power made the images finite, but
removed BDPT's camera-visible emitter coverage. Scale-2 infinite-plane
means were PT 0.453129, BDPT RGB 0.0305721 and BDPT spectral 0.0309033;
finite-area controls were 0.443173 / 0.443129 / 0.446933. Finishing this
requires a BDPT design for the missing camera strategy. The guard and transient test were
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
| DL-347 | src/Library/Utilities/ManifoldSolver.cpp; tests/SMSEmitterDirectionTest.cpp; tests/SMSRenderTestSupport.h; docs/DL44_LIGHTSAMPLE_UV.md (historical audit qualification); shared records. |
| DL-353 / 394 | src/Library/Utilities/ManifoldSolver.cpp; tests/SMSUniformDispersionTest.cpp; tests/SMSRenderTestSupport.h; scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene; scenes/Tests/README.md; tests/data/cst_derive_golden.txt; shared records. |
| DL-395 | src/Library/Utilities/ManifoldSolver.cpp and .h; SMSPhoton.h; SMSPhotonMap.cpp; tests/ManifoldSolverTest.cpp; tests/SMSUniformDispersionTest.cpp; shared records. |
| DL-397 | src/Library/Rendering/OIDNDenoiser.cpp and .h; src/Library/Utilities/OidnConfig.h; tests/OIDNAutoDeterminismTest.cpp; docs/OIDN.md; shared records. |
| DL-399 | tests/ExteriorIndexInvarianceTest.cpp; tests/SSSExteriorIndexInvarianceTest.cpp (separate Part C furnace salting); shared records. |
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

## Pre-round4 normal integration gate (superseded below)

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

## Independent review round 3 comment correction

Fresh physics, test/statistics, and contract reviewers examined
`33de960d9fde522a67f0e7fb3a01a6bdb8bbadfc`. Contract review found a P1
stale animation comment in PixelBasedRasterizerHelper.cpp: it still said
Auto used measured frame seconds. The comment now describes per-frame
wall-time telemetry and explicitly says Auto uses configured work.
The code and executable behavior are unchanged. A search of the OIDN
policy source/comment family found no other current wall-time Auto claim.
The preceding18-run integration gate remains the behavior gate; this
comment correction receives a checked warning-free library rebuild and
SourceHygiene rescan before the next fresh review.

The other round3 P1 was stale DL-44 audit framing in tests/README: it
claimed current SMS never rebuilds an emission query. README, CLAUDE,
the DL-44 closure document and ledger historical paragraph now distinguish
the old audit from DL-347 directional RGB/NM queries with UV/Po/surface
context. Two P2s are also corrected: the obsolete exact device-log suffix,
and OIDN buffer documentation that confused CPU shared handles with GPU
storage. Physics and statistics reviewers reported zero P1/P2 on33de960d9.
All round3 corrections are comments/documentation only.
The library rebuild exited0 with zero warnings in36.15 s; checked
SourceHygiene build exited0 with zero warnings in4.40 s, run167/0.

## Independent review round 4 and furnace salting repair

Two fresh reviewers examined cf11c9df97d1acbee9d1fa68b833a01b7a6d6f51
(the third spawn hit the agent thread limit; the required2-4 reviewer
range is met). Physics/API/cache lifetime found zero P1/P2. Statistics/
document fidelity found one P1: the separate RenderFurnaceH helper in
SSS Part C changed libc seeds but did not apply Sobol ValueSalt. It now
applies a nonzero salt derived from each trial seed, checks that salt
at capture, and resets it after rendering. Filtered runs preserve the
full-suite row seeds, and each row guards its existing band against
three measured SDs of the independently salted mean. Minimum trials is4.
Part B behavior and its completed five-run calibration are unchanged.
The OIDN source-comment twin of the shared-buffer documentation issue
(P2) now distinguishes CPU handle rebuilding from GPU storage reuse.

Library/test builds exited0 with zero warnings (37.26/26.37s). The
focused nine-row C run passed177/0 in7.90s at unchanged bands/budgets:

| Furnace row | H mean | SD of n4 mean | band/SD |
|---|---:|---:|---:|
| Lambertian PT eta1 |1.00026|0.000337534|29.63|
| RW PT eta1.05 |1.00033|0.000303296|32.97|
| RW PT eta1.128 |0.999851|0.000302985|33.00|
| RW PT eta1.5 |0.999558|0.000536454|7.46|
| RW PT eta0.8867 |1.00058|0.000260409|38.40|
| RW BDPT eta1.05 |0.999626|0.000669444|14.94|
| RW BDPT eta0.8867 |0.999586|0.000156084|64.07|
| RW spectral eta1.128 |0.996364|0.00106498|9.39|
| diffusion PT eta1.05 |1.00108|0.00198353|5.04|

These are local n4 sample SD/sqrt(n) estimates, not a universal bound.
Log: `/tmp/cheapbatch-r4-furnace-calibration.log`. Source mutation proof
and refreshed full integration results follow after committing the fix.

After committing11c1ca552, the new capture/precision assertions were
retained while only the furnace salt application was removed. Checked
red build exited0, zero warnings (26.46s); all36 per-render salt checks
failed,141/36 in8.25s. Restoring HEAD, checked build exited0, zero
warnings (31.88s), passed177/0 in8.34s. Logs:
`/tmp/cheapbatch-r4-furnace-proof.json`. No production behavior change
accompanies this test helper correction. The full18-run normal integration
gate is now repeated on the restored final tree.

The DL-352 ledger description of Part C pinning a~0.89 glass-block
deficit is also qualified as historical: controlled calibrated DL-399
SMS/VCM gives1.01021. The current gate does not reproduce the historical
deficit and does not claim a solver/transport repair. DL-352 remains open
for its dedicated displacement audit and PT-no-SMS comparison.

## Final refreshed normal integration gate

After the round4 furnace repair and committed red/green proof, the
library was cleaned and rebuilt (74.39s, exit0, zero warnings); all17
test targets built sequentially with exit0 and zero warnings. All18
serial runs below exited0. No builds overlapped renders, and this run
had no collection pause. The source/test tree stayed unchanged during
the gate; subsequent edits only record results and qualify historical
claims. No production memory/lifetime behavior changed after the clean
focused ASan/UBSan gate.

| Test / arguments | Result | Seconds |
|---|---:|---:|
| SourceHygieneTest | 167/0 | 1.43 |
| CstDeriveGoldenTest | 458 MATCH; 0 DRIFT/UNCOVERED/STALE | 32.16 |
| ManifoldSolverTest | all assertions passed | 0.22 |
| WeaveGapShadowTransmittanceTest 1000 | 217/0 | 626.22 |
| SSSExteriorIndexInvarianceTest --seed 0 | 451/0 | 1139.32 |
| ExteriorIndexInvarianceTest | 275/0 | 1322.16 |
| PTGuidingMISPartitionTest | 185/0 | 44.18 |
| MediumInsideOutsideInvariantTest | 52/0 | 476.93 |
| OIDNAutoDeterminismTest | 163/0 | 145.04 |
| SMSMediumAnchorTest | 27/0 | 552.04 |
| SMSEmitterDirectionTest | 168/0 | 61.01 |
| SMSUniformDispersionTest | 86/0 | 274.22 |
| SMSUniformDispersionTest --shipped | 10/0 | 0.26 |
| SSSRadianceScalingTest | 576256/0 | 79.11 |
| DoubleSidedEmitterTest | 34/0 | 15.31 |
| FrameStoreTest | 123/0 | 0.72 |
| RasterizerDefaultsConsistencyTest | 164/0 | 0.27 |
| AgentEvalCheckTest | 2075/0 | 54.98 |

Full SSS furnace: all9 precision guards pass; the smallest observed
band/mean-SD is6.01528. The focused C pilot above remains a separate
local estimate, not an extra independent full-suite calibration point.
Weave base1000 and SSS seed0 repeat prior calibration inputs; their
Part B results are not pooled as new independent calibration means.
Exterior reproduces the final paired ratios/SDs above. Existing non-gated
photon/known-failure diagnostics stay non-gated.
Logs and runtimes: `/tmp/cheapbatch-review5-gate.json`.
The final independent review verdict will be reported against the full
resulting HEAD; this record is committed before that review.


## Takeover integration checkpoint (2026-10-03; incomplete)

Master `115aee62e` was merged into this branch. The four conflicts were
resolved; SMS retains both solved-direction emitter sidedness (DL-347)
and master's delta-light endpoint tangent plane (DL-413). The weave helper
uses one explicit/default-salt contract. SSS retains the DL-332 Part B
calibration and master's DL-370 Part F rows. The master DL-345 Part C
bands and minimum eight replicates are retained, with the batch sampling
budgets and paired covariance calculation.

A clean make library build passed with zero warnings (63.03s). Each test
below was built separately, with its build exit checked and zero warnings,
then run serially with RISE_MEDIA_PATH set. No build overlapped a render.

| Test | Result | Seconds |
|---|---:|---:|
| SourceHygieneTest | pass (before later mesh/comment additions) | see checkpoint log |
| CstDeriveGoldenTest | 458 MATCH, zero DRIFT/UNCOVERED/STALE | see checkpoint log |
| ManifoldSolverTest | all assertions pass | see checkpoint log |
| WeaveGapShadowTransmittanceTest 1000 | 244/0 | 769.79 |
| SSSExteriorIndexInvarianceTest --seed 0 | 510/0 | 1069.06 |
| ExteriorIndexInvarianceTest | **273/2** | 1440.82 |

The two Exterior failures are the mean and precision checks on RGB
uniform-SMS glass-sphere k1: at 1024 spp, n4, enclosed/air 0.96636,
paired mean SD 0.00767094, retained band 0.02. Master already tracks this
point-light caustic tail as DL-418, whose recipe calls for n>=8 salted
replicates. A fixed higher-spp pilot (4096 spp, n4, seed offset0) failed
both checks again: ratio 0.971551, paired mean SD 0.0166977; air mean
2.58284 with render SD 1.81623. A second fixed seed panel and larger
replicate measurements remain pending. No band change is claimed.

Both Part C shipped comparisons passed: flat-slab n8 SMS4096/VCM1024,
ratio 0.996254, paired mean SD 0.0118433, band [0.885,1.130]; glass-block
n8 SMS16384/VCM512, ratio 1.03898, paired mean SD 0.011489, band
[0.840,1.180]. The SSS furnace minimum band/mean-SD was 5.25833.

The remaining gate, committed red proofs for the added double-sided
indexed-mesh rows, Xcode gate, and fresh independent review have NOT yet
completed. This checkpoint does not supersede the old isolated-branch
results above with a claim of a green merged tree. No rows are closed,
and no merge to master has occurred. Local logs:
`/tmp/rise-takeover-gate.json`, `/tmp/rise-takeover-exterior-pilot.json`.


### DL-418 tail attribution and DL-434 repair (historical failed checkpoint)

The second fixed4096-spp n4 panel also failed: enclosed/air0.962416,
paired mean SD0.0142349 (605.61s). The larger1024-spp n16 panel at seed
offset0 failed much more severely: ratio1.64433, paired mean SD0.710774;
air1.81994 with render SD0.685688, enclosed2.99257 with render SD5.00188
(595.77s). A three-SD band exceeds200% and would be uninformative. The
second n16 panel and the conditional remaining-gate runner were stopped
by their explicit task-owned process ids after identifying the defect
below; no result from that incomplete panel is claimed.

Source attribution found a concrete broken existing contract, present
on master115aee62e: RGB ComputeTrialContribution neither applies its
clampGeometric flag nor writes outSmsGeometric. Biased uniform/photon
callers initialize the geometric output to0, so their sum clamp cannot
operate. NM does write the raw geometric term, but its individual clamp
treats nonpositive limits differently from the existing sum-clamp paths.
This is filed as DL-434. The absolute-IOR threshold candidate remains
unattributed; no solver threshold/normalization policy changed.

The regression uses a planar mirror and an independently unfolded
virtual point light: raw geometric factor1/(0.6^2+2^2). Four cap settings
(-1,0,0.01,1), RGB/NM exported raw terms, finite positive contributions,
and capped/unclamped ratios pin both helper contracts. An initial fixture
forgot to populate light pdf fields and produced non-finite ratios;
that trial is discarded. The corrected fixture sets both pdfs to1 and
asserts finiteness explicitly. It fails116/9 before the fix.

Commits0cdb610fd/983aaa895 contain the test and repair. On committed
state, both ManifoldSolver.cpp and its header were checked out from
master; the checked build passed with zero warnings (40.60s), then the
unit gate failed116/9 (0.67s). Restoring HEAD, checked build passed with
zero warnings (40.75s), then unit gate passed125/0 (0.63s). The RGB helper
now exports raw geometry and applies a requested positive individual cap;
both helpers disable nonpositive caps, consistently with the four sum
clamps. Default cap10 is unchanged. Logs: /tmp/rise-dl434-proof.json.

At this checkpoint, two fixed n8 panels at1024 spp, seed offsets0/10000,
were pending. Their completed measurements and the final test budget are
recorded below. No band widening or solver-threshold design change was made.


### Repaired tail calibration and added mesh/source proofs

Both repaired1024-spp n8 panels passed131/0: seed offsets0/10000 give
0.999884/0.999867 with paired mean SD0.0000128219/0.0000112882,
in302.51/303.44s. Both16-spp n8 panels also passed131/0, in4.96/4.94s:
0.999687/0.999916 with paired mean SD0.000227643/0.00000245457.
The retained0.02 band covers87.9 times the worst measured n8 mean SD.
Commitac1db39ff restores the16-spp k1 budget and requires at least8
pairs for RGB uniform glass (including k2). All Part B rows reserve a
seed span of max(trials,8), so filtered/full-suite inputs agree and the
longer rows do not overlap siblings. The low-budget panels use exactly
those default k1 seeds. The known photon residual remains nongated.
Logs: /tmp/rise-dl434-pilots.json and /tmp/rise-dl434-lowbudget.json.

With committed source replaced by master cpp+header, the expanded
SMSEmitterDirectionTest built0 warnings and failed337/7 (234.98s);
SMSUniformDispersionTest built0 warnings and failed90/24 (399.14s).
Restoring HEAD, their checked builds passed0 warnings and tests passed
344/0 (230.08s) and114/0 (398.83s). Both geometry kinds resolve the
SF11 displacement at about0.0562pixels, versus master's0.0016pixels;
constant-index controls remain near zero. The added double-sided indexed
emitter rows preserve winding-invariant Lambertian/Phong emission in
RGB/NM, Snell/uniform. Logs: /tmp/rise-takeover-sms-redproof.json.

Commit1376d8d24 lets the photon-context regression compile against the
old solver API using test-only overload/field adapters. Master cpp+header
now builds successfully0 warnings (19.51s) and the test fails at the
intended material-context attenuation assertion in0.35s (SIGABRT), rather
than failing to compile. Restoring HEAD builds0 warnings (23.52s) and
passes the full ManifoldSolverTest in0.25s. The context oracle expects
1.2 + 0.2*0.6 + 0.3*0.7 =1.53 at the captured UV/object point; NM
adds wavelength*0.0001 at450/650nm. Log:
/tmp/rise-takeover-photon-redproof.json.

The final normal gate is now running after a fresh clean library build.
The earlier full weave244/0 and SSS510/0 integration results remain valid:
source/test behavior exercised by them is unchanged by the geometric
helper repair, and neither suite enables uniform seeding or SMS photons.
The final run repeats all remaining/affected targets, including full
Exterior, manifold, both mesh suites and master-overlap targeted gates.
Xcode Deployment/Opto and fresh independent review remain pending.

### Final integrated normal gate, 2026-10-03

The production/test tree through1376d8d24 passed a fresh clean make
library build (60.59s) with zero compiler warnings. Every test was built
individually, its exit/warnings checked before running, with
RISE_MEDIA_PATH pointing at this worktree. No stale binary was accepted.
The table lists test runtime, excluding its individual checked build.

| Target | Result | Seconds |
| --- | --- | --- |
| SourceHygieneTest | 169/0 | 1.47 |
| CstDeriveGoldenTest | 458 MATCH / 0 DRIFT / 0 UNCOVERED / 0 STALE | 28.17 |
| ManifoldSolverTest | all assertions pass | 0.29 |
| ExteriorIndexInvarianceTest | 299/0 | 1295.93 |
| PTGuidingMISPartitionTest | 185/0 | 40.57 |
| MediumInsideOutsideInvariantTest | 52/0 | 427.66 |
| OIDNAutoDeterminismTest | 163/0 | 130.41 |
| SMSMediumAnchorTest | 27/0 | 502.40 |
| SMSEmitterDirectionTest | 344/0 | 231.85 |
| SMSUniformDispersionTest | 114/0 | 401.42 |
| SMSUniformDispersionTest-shipped | 10/0 | 0.26 |
| SSSRadianceScalingTest | 576256/0 | 73.43 |
| DoubleSidedEmitterTest | 34/0 | 14.81 |
| FrameStoreTest | 123/0 | 0.73 |
| RasterizerDefaultsConsistencyTest | 164/0 | 0.30 |
| AgentEvalCheckTest | 2075/0 | 49.07 |
| TransparentShadowPartitionTest | 42/0 | 66.58 |
| OpenSheetIndexConventionTest | 24/0 | 43.26 |

Full Exterior reproduces the calibrated RGB uniform k1 ratio0.999687
with paired mean SD0.000227643 (n8,16spp); k2 is1.000000 with mean
SD6.87161e-08 (n8). Part C flat/glass ratios0.996254/1.03898 have paired
mean SD0.0118433/0.011489 within the retained master bands. The photon
k2 residual1.03978 (mean SD0.00136612) remains nongated under DL-331.
The full suite took1295.93s, so the historical ten-minute estimate is
not the integration runtime. Dispersion includes both plane and
double-sided indexedmesh, with/without HWSS and constant-index controls.
Earlier full SSS exterior510/0 reuse remains applicable because that target
does not enable SMS. The earlier weave244/0 is a historical full-suite
result: its SMS/split cases exercise the changed helper and are being
renewed explicitly on the attenuation repair (see the final gate below).

Normal-gate logs: /tmp/rise-takeover-final-gate.json. Fresh Xcode
Deployment/Opto builds and independent review are pending at this
checkpoint; no master merge is claimed.

### Integration self-audit and review scope

Likely failure surfaces for the fresh review are: (1) four RGB/NM,
Snell/uniform SMS emission sites must retain both solved-direction
sidedness and master's delta-light Jacobian normal; (2) geometric output
and individual/sum caps must agree without altering the existing cap10
policy or claiming an unbiased clipped estimator; (3) UV/child-frame Po
and dispersive eta refresh must survive companion/photon reconstruction;
(4) OIDN's fixed sampling-budget policy must reach every rasterizer, crop,
companion and MLT path, and warmed backend changes must release filters
before devices; (5) salted tests must measure independent inputs, preserve
filtered/full-suite seeds, and use paired covariance in their precision
guards. Critical files are ManifoldSolver, SMSPhoton/Map, OIDNDenoiser,
OidnConfig and all rasterizer/Job/API callers, plus the Exterior, SSS,
Weave, OIDN and SMS test families. No new solver tolerance was introduced.

Review is against master115aee62e plus the completed integration tree.
Known user-directed design stops remain open: DL-311, DL-353 remainder,
DL-391, DL-396 and DL-398. DL-331's printed photon residual is also open.
At this pre-DL-435 checkpoint, the ledger's thirteen strikes and dated
recount are prospective on this branch; they take effect on master only after zero-P1 review and no-ff
integration. No pushed state is involved.

### Fresh Xcode gate, 2026-10-03

Clean Deployment/RISE-GUI and Opto/RISE-GUI-Opto builds pass with zero
compiler or linker warnings in55.78/103.52s. Each reports only Xcode's
AppIntents metadata-extraction notice (no framework dependency), excluded
by the repository's explicit warning-gate rule. The first Deployment
attempt built successfully but failed the strict warning gate because
this worktree lacked extlib/oidn/install/lib. Checkout-only install
include/lib links now resolve to the same CPU-only Homebrew OIDN2.5.0
used by the passing make/test gate. No dependency upgrade, project edit
or warning suppression was made; actual GPU transition remains untested.
Fresh build records: /tmp/rise-takeover-xcode-gate.json.

All required integration gates are now green. Independent review and
master integration remain pending at this committed checkpoint.

### Fresh integration review round1 — stopped before merge

This is checkpoint history before the authorized DL-435 repair below.
Three independent read-only reviewers examined300d4eff4. The test lens
found one P1: README accidentally restored DL-348's historical VCM Z2
deficit instead of master's corrected0.6% gate description. The transport
lens found the incorrect sign still present in DL-347's recipe (source
uses the correct light-to-chain vector) and an existing spectral SMS
throughput omission. The OIDN/API/ledger lens found zero P1/P2. Both
doc defects are corrected; the implementation tree is unchanged.

The spectral finding is new DL-435. A temporary deterministic helper
probe built successfully with zero warnings, then exited1: quarter-grey
mirror RGB0.25/NM1; white entering dielectric1-to-1.5 RGB0.426666666667/
NM0.96. It used the committed library, RISE_MEDIA_PATH set, and was
removed afterward. Its build/run logs are /tmp/rise-dl435-proof-build.log
and /tmp/rise-dl435-proof-run.log. Several spectral specular queries
currently leave attenuation white or inherit an RGB triple; fixing this
properly needs a defined wavelength-resolved metadata contract as well as
the missing throughput factors. No partial production repair was made.

Per the user's explicit stop-on-design-decision rule, integration is
stopped for the DL-435 decision. Round1 is NOT a zero-P1 verdict. The
normal/Xcode gates remain green for the unchanged implementation; the
doc corrections need a fresh review along with the chosen disposition.
Master remains115aee62e, unmodified, and no merge or push occurred. The
prospective integration ledger is284 rows/45 open/239 closed, including
DL-435; master retains273/47/226 until actual integration.

### DL-435 continuation: user-authorized design and repair

The user explicitly requested a practical, performant and thorough
attenuation system and repair. The accepted implementation direction is
recorded in DL435_SPECTRAL_SMS_ATTENUATION.md: explicit cached NM scalar,
material-specific boundary versus interior tau convention, native spectral
painter queries, RGB-only metadata fallback, complete Snell/uniform/photon
propagation, and the missing eta-radiance factor. No new Library file or
allocation/cache is introduced. C++ metadata layouts grow, so clients
sharing those structures must rebuild; C construction ABI is unchanged.

The first deterministic gate passed21/0, then27/0 with photon propagation;
full manifold assertions pass. The initial n4 render pilot used an
unjustified0.001 band and failed86/10. Throughput-dependent continuation
changes sample decisions: paired images do not scale exactly even though
the expectation does. Worst ratio render SD0.00529804 means mean
SD0.00264902; the final new0.01 band is3.775 mean SDs and each row checks
its own three-SD precision. The unchanged8-spp budget passes96/0 in6.22s
across sixteen geometry/winding/NM-HWSS/Snell-uniform configurations.
These are calibration results, not final committed-source proof or review.

The expanded deterministic gate passes49/0, including an actual dielectric
indexed mesh with both windings/approach sides and independent two-unit
RGB/NM tau oracles. Default Manifold assertions pass. The full dispersion
pilot passes210/0 in414.14s. SF11 centroid shifts now resolve about0.3125
pixels after the missing radiance scale is restored; constant-index
controls remain near zero, and the existing dispersion bands are retained.
The source/test/design tree is frozen for committed baseline/master proofs.

### DL-435 committed red/green proof and measured storage/cost

At committedf3061e1ab, all nine fixed source/header files were replaced
first by pre-DL-435f045c80e9, then by master115aee62e. Both library
and individually checked unit/render builds passed with zero warnings.
Each baseline failed the new deterministic gate16/33 and the sixteen-row
attenuation matrix74/22. Restoring HEAD rebuilt warning-free and passed
all manifold assertions (DL-43549/0,0.34s) and the attenuation matrix
96/0 (6.26s). The master's build/test failure is runtime, not a missing
API compilation failure; test-only overload/field adapters preserve the
old-header proof. Exact records: /tmp/rise-dl435-redproof.json.

Measured metadata sizes: pre-DL-435 SpecularInfo48 / ManifoldVertex328
bytes; fixed56/336 bytes, eight additional bytes per structure. Master
ManifoldVertex304 predates this batch's existing UV/Po additions and is
not the appropriate size baseline for DL-435. No extra allocation occurs
in attenuation evaluation.

The focused varying-position, one-vertex microbenchmark uses200000 calls
per trial and five trials per mode, consuming each result. Pre-DL-435
clear/absorbing-labelled cases measure10.50..11.30/9.86..10.53ns per call
(the defective NM helper ignores attenuation in both). Restored fixed
code measures11.90..13.18/13.28..14.32ns. An earlier pilot on the same
repair measured about36..47ns, illustrating execution/scheduling
sensitivity; these are synthetic loop timings, not a renderer speed
guarantee or an interleaved whole-render comparison. The fixed cached
loop performs the missing work. The small n4 render matrix takes6.22s
on the old library and6.26s restored, also not a statistical cost bound.
Logs: /tmp/rise-dl435-proof-{pre435,head}-benchmark.log.

### DL-435 focused sanitizer gate

A fresh clean instrumented library build passes0 warnings in126.61s,
followed by individually checked instrumented builds for ManifoldSolver
and SMSUniformDispersion. Full manifold assertions pass (DL-43549/0,
0.53s); the attenuation matrix passes96/0 in38.51s. No ASan/UBSan report
is present; UBSan halts on findings. As in the earlier macOS gate, leak
detection is disabled and external prebuilt dependencies are not
instrumented, so this is not a leak-free or all-dependency claim. These
checks exercise the new metadata layouts, native/fallback queries,
photon propagation, actual dielectric mesh seeds and rendered matrix.
Records: /tmp/rise-dl435-sanitizer.json. A fresh normal clean library
build and full integration test gate follow; their results are recorded below.

### DL-435 pre-review self-audit

The most likely misses are (1) an NM producer or Snell/uniform/photon
replay dropping the cached scalar or distance-law flag; (2) reversed
photon or double-sided mesh exits charging the wrong segment; (3) a
material boundary multiplier being mistaken for dielectric per-unit tau;
(4) an eta-radiance factor being omitted or applied twice; and (5) a
paired render or cost measurement claiming more than it demonstrates.
The critical surfaces are SpecularInfo, IMaterial/ISPF defaults, native
reflector/refractor/dielectric metadata and all ManifoldSolver metadata
assignments and throughput callers. The deterministic 49-check gate
exercises native/fallback queries, photon replay, scalar/radiance factors
and both windings and approach sides of real dielectric indexed meshes.
The sixteen-row render matrix covers NM/HWSS and Snell/uniform; its band
is derived from salted paired ratios rather than exact-image scaling.

Fresh review must also check the complete cheapbatch diff against master,
including prior emitter-sidedness/Jacobian changes, OIDN configuration
plumbing, test integrity, and ledger/design/cost fidelity. Existing
user-stopped DL-391/396/398/311 and the partial DL-353 remain explicitly
separate; this repair makes no claim to resolve their design choices.

### DL-435 final normal integration gate

The attenuation implementation/test commit is `f3061e1abe455320b564c9ad94674d0ffdc75973`. Subsequent edits record evidence and clarify documentation; implementation and test code remain unchanged. After the fresh sanitizer gate, a normal clean rebuild completed in 61.97s with zero compiler warnings. Every test below was built individually, its exit code and warnings checked, then run sequentially with `RISE_MEDIA_PATH` set to this worktree. All 37 normal build/run steps pass.

| Target | Checked build (s) | Run (s) | Result |
|---|---:|---:|---|
| SourceHygieneTest | 5.98 | 1.50 | 169/0 |
| CstDeriveGoldenTest | 25.25 | 27.90 | 458 MATCH; zero DRIFT/UNCOVERED/STALE |
| ManifoldSolverTest | 7.32 | 0.41 | all assertions; DL-435 49/0 |
| ExteriorIndexInvarianceTest | 25.28 | 1327.19 | 299/0 |
| PTGuidingMISPartitionTest | 24.57 | 40.90 | 185/0 |
| MediumInsideOutsideInvariantTest | 24.28 | 430.79 | 52/0 |
| OIDNAutoDeterminismTest | 24.69 | 130.62 | 163/0 |
| SMSMediumAnchorTest | 24.24 | 499.18 | 27/0 |
| SMSEmitterDirectionTest | 24.05 | 233.83 | 344/0 |
| SMSUniformDispersionTest | 24.02 | 411.27 | 210/0 |
| SMSUniformDispersionTest-shipped | 24.02 | 0.26 | 10/0 |
| SSSRadianceScalingTest | 24.31 | 73.49 | 576256/0 |
| DoubleSidedEmitterTest | 23.95 | 14.69 | 34/0 |
| FrameStoreTest | 2.90 | 0.75 | 123/0 |
| RasterizerDefaultsConsistencyTest | 6.85 | 0.29 | 164/0 |
| AgentEvalCheckTest | 37.78 | 49.93 | 2075/0 |
| TransparentShadowPartitionTest | 24.75 | 65.97 | 42/0 |
| OpenSheetIndexConventionTest | 23.91 | 42.64 | 24/0 |

Exterior retains the established Part C bands: flat slab SMS/VCM 0.996254 +/- 0.0118433, glass block 1.03898 +/- 0.011489. The existing non-gated photon nested-IOR residual remains 1.03978 +/- 0.00136612; this is not claimed fixed. The default dispersion target includes all 96 new attenuation checks. SF11's centroid shift is about -0.3125 pixels in both NM/HWSS and plane/indexedmesh; constant-index controls stay near zero. No established dispersion band was widened.

The earlier full SSSExteriorIndexInvariance510/0 remains applicable: it does not enable SMS, and its target/fixture sources are unchanged. The earlier full WeaveGapShadowTransmittance244/0 is historical; the earlier reuse claim was too broad because its SMS/split cases include a tinted refractor ball lens. Those cases require a fresh targeted run on the attenuation repair. Neither reuse nor the targeted weave run substitutes for the full SMS and exterior gates above. Exact normal records: `/tmp/rise-dl435-final-gate.json`; per-step logs use `/tmp/rise-dl435-final-<label>.log`.

### DL-435 final clean Xcode gate and integration candidate

Clean Deployment/RISE-GUI and Opto/RISE-GUI-Opto builds pass in 57.97s and 103.06s with zero compiler or linker warnings. Each build reports only the AppIntents metadata notice explicitly excluded by AGENTS.md. The checkout-only OIDN include/lib links still resolve to the same CPU-only Homebrew OIDN 2.5.0 used above; no dependency upgrade, project modification or warning suppression occurs. Actual GPU transitions remain untested. Records: `/tmp/rise-dl435-xcode-gate.json`.

The complete gate evidence and prospective DL-435 closure are prepared for fresh independent review. Integration is permitted only after a fresh zero-P1 verdict on the final committed tree, followed by a no-ff merge and targeted gate at the master merge commit. The fourteen prospective closures yield 284 main rows, 44 open and 240 closed; master remains at 115aee62e until integration. The merge commit must identify the exact reviewed tree and verdicts.

### Fresh review round2 findings and correction work

Exact reviewed HEAD: `650cca6055802f2aa06fb180dcfd36b0c6196e11`. Transport found native perfect-refractor reflection incorrectly multiplied by transmission tint, and bare SMS Fresnel ignoring native dielectric AR coatings (new DL-436). The API/doc reviewer independently found the stale material-law comment family. Test review found hardcoded /tmp paths on Windows and the new deterministic gate hidden in assert under NDEBUG. These are confirmed; corrections are in progress and round2 is not a zero-P1 verdict.

The refined source distinguishes transmission-only refractance, and advertises an optional SPF interface-law evaluation for coated dielectrics at the solved cosine/ordered eta/wavelength. Uncoated paths avoid virtual calls; AR uses the SPF's existing stack law and TIR classifier. No new generic absorbing-film model is introduced. Portable temp paths and an unconditional default deterministic gate address the test findings. Clean library and deterministic build pilots have zero warnings; expanded native-SPF/independent Airy/double-sided-mesh gate passes184/0.

A 32-spp n4 reflection-only upward-spot pilot is not a valid control for one sheet winding: all RGB/NM/HWSS rows are dark. The reversed winding is measurable, with quarter/white ratios1.00309/1.00043/1.00308 (render SD0.00472881/0.00244014/0.00504575). Its0.02 band exceeds three mean SDs. The dark rows expose a separate seed-coverage design question (DL-437); they are not used to claim a throughput fix. Pilot logs: `/tmp/rise-dl435-r2-interface-pilot.log`.

Coating render pilot refinement: the first directional fixture was entirely dark and invalid; the distant-point fixture with receiver albedo 0.5 initially failed the single-interface oracle (60/12), because it also admits floor/sheet feedback. Reducing only the receiver albedo to 0.0001 bounds the geometric-series feedback below 0.000101 relative (rho/(1-rho)). The corrected fixture passes 72/0 over twelve n4 pairs (RGB/NM/HWSS, Snell/uniform, both mesh windings): RGB ratios1.04015 vs independent Airy1.04016; NM/HWSS1.04166 vs1.04167; maximum render SD1.25981e-6 (mean SD6.29905e-7). The0.001 band resolves >1587 measured mean SDs and allows the finite-angle/feedback approximation. This is a control-scene correction, not a widened noisy band or an undisclosed transport repair. Pilot log: `/tmp/rise-dl435-r2-coating-pilot4.log`.

Round2 focused cost pilot (five trials x200,000 varying-position calls per case, same optimized make build): metadata remains SpecularInfo56 / ManifoldVertex336 bytes; 0 layers, NM: 10.71..11.33 ns/call; 0 layers, RGB: 14.06..15.00 ns/call; 1 layers, NM: 133.82..135.75 ns/call; 1 layers, RGB: 376.87..382.42 ns/call; 8 layers, NM: 558.47..573.01 ns/call; 8 layers, RGB: 1662.37..1709.14 ns/call. These are throughput-query costs, not render times; coated RGB runs three wavelength evaluations. The eight-layer cap is substantially more expensive than bare Fresnel and is used only on advertised coated vertices. Earlier pre-DL435 timing checkpoints remain historical, not a claim of a whole-render speed bound. Record: `/tmp/rise-dl435-r2-benchmark-final.log`.

### Round2 committed-state failure proofs

Repair commit `7bdc53cdd` is red-proved with the current tests against both committed pre-round2 source `650cca605` and committed master `115aee62e`: the ten fixed Library files (including DielectricSPF.cpp and the shared interfaces) are restored together, each library/test build succeeds with zero warnings, and each required regression run exits1. Pre-round2 unit81/103, reflection15/3, coating60/12; master unit52/132, original attenuation74/22, reflection17/1, coating60/12. Restoring HEAD yields full ManifoldSolver green (DL435184/0), attenuation96/0, reflection18/0, coating72/0. Each binary is run only after a checked successful warning-free build. Records: `/tmp/rise-dl435-r3-redproof.json` and `/tmp/rise-dl435-r3-proof-*.log`. Fresh sanitizer, integration and Xcode gates remain pending at this note.

Round2 fresh sanitizer gate passes: clean instrumented library125.17s, full ManifoldSolver184/0 (build68.64s/run0.52s), attenuation96/0 (38.09s), reflection18/0 (24.46s), coating72/0 (61.76s), render target build66.29s. All builds exit0 with zero warnings; no ASan/UBSan reports. UBSan halts on error; macOS ASan leak detection is disabled, and foreign libraries are not instrumented. Records: `/tmp/rise-dl435-r3-sanitizer.json`. A fresh normal rebuild and integration/Xcode gates follow.

Round2 self-audit before fresh review: the main failure surfaces are (1) event applicability drifting from native SPF reflection, pinned with real perfect-refractor RGB/NM/TIR controls and the reflection-only render; (2) coating incidence, orientation or wavelength being stale, pinned with independent Airy, native-SPF two-layer oblique/TIR, both-sided/wound mesh and photon reconstruction; (3) spectral metadata losing UV/child-Po or tau distance, pinned by retained context/distance controls; (4) silently accepting dark root-coverage rows, explicitly rejected and filed as DL-437; (5) interface/lifetime/test execution assumptions, addressed with the C++ rebuild disclosure, scoped native objects, portable temporary paths and an unconditional new deterministic gate. The existing final-root, nested-spectral-IOR and participating-medium limitations remain named rather than claimed solved.

The interface-law audit additionally confirms an existing RGB model limitation by code reading: native refracting metadata selects IOR GetValuesAt.v[0], while native RGB scatter uses three channel indices; ManifoldVertex stores only scalar interface indices. The coating callback uses the SPF's three film wavelengths against that existing scalar pair, not three refraction geometries. Filed as DL-438 and asked alongside DL-437 whether to defer or extend the SMS design. The constant-IOR RGB Airy controls do not claim to cover dispersive RGB geometry. No runtime magnitude or convergence result is claimed for this code-reading finding.

Sibling metadata-adapter audit: the native DielectricMaterial forwards metadata and SPF to the same DielectricSPF; mirrors/refractors forward to their own SPF. Composite and Coated adapters do not advertise this delta-specular metadata, so they do not accidentally forward a custom-law flag to an unsupported wrapper callback. Polished and SSS native specular metadata retain their neutral surface multipliers and default custom-law flag. This is a code-reading audit, not a new render-coverage claim.

Round2 fresh integration progress on repair commit `7bdc53cdd`: clean normal library61.37s, zero warnings; SourceHygiene169/0; CST458 MATCH/0 DRIFT/0 UNCOVERED/0 STALE; full Manifold184/0; full Exterior299/0 (1298.21s; flatslab0.996254 +/-0.0118433 and glassblock1.03898 +/-0.011489 at the existing bands); PTGuiding185/0; Medium52/0 (424.48s); OIDN163/0 (128.66s). All builds and runs exit0, with zero build warnings. Further integration targets and clean Xcode builds are still running; this is not a final or zero-P1 verdict. Records: `/tmp/rise-dl435-r3-final-gate.json`.

### Round2 completed normal and Xcode gate

Production repair commit `7bdc53cdd` passes all 37 fresh normal build/run steps after a clean library rebuild (61.37s). Every target was built individually with checked exit0 and zero compiler warnings before its sequential run with `RISE_MEDIA_PATH` set. The only subsequent test edit corrects the feedback-bound comment to0.000101; that comment was compiled by this normal gate.

| Target | Build (s) | Run (s) | Result |
|---|---:|---:|---|
| SourceHygieneTest | 5.82 | 1.49 | 169/0 |
| CstDeriveGoldenTest | 24.64 | 27.55 | 458 MATCH; zero DRIFT/UNCOVERED/STALE |
| ManifoldSolverTest | 7.31 | 0.39 | 184/0 plus legacy assertions |
| ExteriorIndexInvarianceTest | 24.56 | 1298.21 | 299/0 |
| PTGuidingMISPartitionTest | 23.69 | 40.25 | 185/0 |
| MediumInsideOutsideInvariantTest | 23.49 | 424.48 | 52/0 |
| OIDNAutoDeterminismTest | 24.05 | 128.66 | 163/0 |
| SMSMediumAnchorTest | 23.14 | 493.18 | 27/0 |
| SMSEmitterDirectionTest | 23.32 | 231.25 | 344/0 |
| SMSUniformDispersionTest | 23.41 | 417.03 | 300/0 |
| SSSRadianceScalingTest | 23.90 | 73.31 | 576256/0 |
| DoubleSidedEmitterTest | 23.36 | 14.60 | 34/0 |
| FrameStoreTest | 2.82 | 0.75 | 123/0 |
| RasterizerDefaultsConsistencyTest | 6.72 | 0.30 | 164/0 |
| AgentEvalCheckTest | 36.71 | 48.53 | 2075/0 |
| TransparentShadowPartitionTest | 24.21 | 65.59 | 42/0 |
| OpenSheetIndexConventionTest | 23.27 | 42.40 | 24/0 |

The shipped dispersion scenes additionally pass10/0 (0.25s). Existing exterior bands and the ungated photon residual reported above are unchanged. The expanded default dispersion includes attenuation96/0, reflection18/0 and coating72/0; its coating maximum mean SD is8.22e-7, below the retained0.001 band. All established bands remain unchanged. Records: `/tmp/rise-dl435-r3-final-gate.json`.

Fresh clean Xcode Deployment and Opto builds pass in57.07s and100.85s, with zero compiler/linker warnings. Each has only the AppIntents metadata notice excluded by AGENTS.md. The same CPU-only OIDN2.5.0 dependency and checkout-only links are used; actual GPU transitions remain unmeasured. Records: `/tmp/rise-dl435-r3-xcode-gate.json`. The fresh relevant weave SMS/split and portable temporary-directory checks are recorded below.

### Round2 weave and portable temporary-directory gates

Fresh WeaveGapShadowTransmittanceTest build exits0 with zero warnings (24.70s); `WEAVE_GAP_FILTER=sms,split` passes31/0 in182.24s. The three split SMS on/off ratios are0.98352 (perfect refractor),1.03394 (native dielectric) and0.99735 (perfect refractor HWSS), within their unchanged established bands. This fresh run covers the tinted ball-lens throughput paths missed by the earlier reuse note. Other non-SMS weave groups retain their earlier full244/0 evidence; the full non-SMS SSS exterior510/0 evidence also remains applicable. Records: `/tmp/rise-dl435-r3-extra-gate.json`.

With TMPDIR set to the worktree-local `.claude/test-tmp space` and inherited RISE_OPTIONS_FILE cleared, already checked normal binaries pass reflection18/0 (3.17s) and OIDN policy112/0 (0.12s); exit cleanup leaves zero temporary entries. This checks macOS custom-directory handling and spaces, not a Windows execution claim. Records: `/tmp/rise-dl435-r3-temp-gate.json` and driver log.

The complete current tree is ready for fresh independent review. DL-437 reflection seed coverage and DL-438 RGB channel-index geometry are pending user design dispositions; neither is silently treated as an approved deferral. The ledger recount is287 main rows,47 open and240 prospectively closed, unique IDs through438 (next439;404/405 remain unused). Master remains clean at115aee62e. No merge or push has occurred.

### Fresh review round3 findings and corrections

Exact reviewed HEAD `c40dab0e8ab956b61ca7ede08f8cc1f887288a74` is not a zero-P1 verdict. Transport confirms event/attenuation/coating/eta and emitter/Jacobian overlaps, but finds output-vector reuse retaining photon reconstruction solver state (DL-439). API/cost/docs confirms interface/rebuild/cost/ledger fidelity, but finds CPU Accurate OIDN writing public const auxiliary inputs (DL-440). Tests confirms red proofs, independent oracles, salts/bands and actual mesh coverage; it finds the older photon UV/Po context test inside assert (P2).

The reconstruction correction resets every output vertex before filling it; deterministic reuse checks include shorter/same/longer buffers, RGB/NM and both sides/windings of actual indexedmesh. The context test now returns explicit failure outside assert, with an isolation flag. OIDN Accurate uses reusable owned auxiliary buffers and refreshes their inputs even on cache hits; Fast retains shared input-only buffers. Const-input tests cover supported guide combinations (both, albedo-only, neither), mode/dimension transitions and repeated same-pointer calls with changed data, against fresh output. An initial normal-only fixture was rejected by OIDN as an unsupported feature combination and failed its lit-output guard; it is not accepted as a valid preservation control.

The correction adds no new Library file or optional generic attenuation model. CPU Accurate retains up to two image-sized auxiliary buffers and makes one input copy per supplied guide each call; no measured whole-render cost bound is claimed. DL-437/438 remain pending user design decisions. Committed failure proofs and fresh focused gates/review follow.

Round3 committed-state proofs: current tests fail on reviewed `c40dab0e8` (Manifold304/84; OIDN const-input54/10), and coherent committed master source/headers (Manifold160/228; OIDN const-input54/10). Every accepted baseline test build exits0 with zero warnings. The master manifold total includes the earlier attenuation/interface omissions; the pre-round3 baseline isolates the84 output-reuse failures. OIDN's ten input-preservation failures isolate mutation without relying on cache-inspection methods added in the batch. An initial master implementation-only attempt failed to compile against the new reconstruction declaration; it was restored, no stale binary was run, and the accepted proof restores compatible headers too. Current source is restored to HEAD; checked rebuilds and sanitizer/normal gates follow. Records: `/tmp/rise-dl435-r4-gate.json`.

Round3 fresh ASan/UBSan gate passes: clean instrumented library124.26s, zero warnings; full Manifold388/0 plus legacy checks (build67.05s/run0.53s), OIDN policy/const-input176/0 (build65.78s/run1.38s). No sanitizer report; UBSan halts on findings, ASan leak detection is disabled on macOS and foreign dependencies remain uninstrumented.

Self-audit before round4: (1) defaulting reused photon state must reset ordered indices, derivatives and alpha ownership without dropping material/context/event metadata; dirty output vectors and actual mesh side/winding controls pin this. (2) Accurate owned buffers must survive Fast/Accurate, dimensions, guide presence and cache-hit transitions; unchanged inputs, same-pointer changed-data and fresh-output equality pin this. (3) “zero-copy” and const-input claims must describe only applicable modes; source/header and OIDN doc family now disclose Accurate copies and retained auxiliary memory. (4) release-build context queries must actually execute outside assert; explicit return checks pin the call path. (5) prior full render evidence must not hide affected paths; all three production photon reconstruction sites instantiate empty vectors, so default resets preserve those paths, while changed OIDN/mesh/reuse paths run fresh.

### Round3 completed normal focused gate

Source/test repair tree `e59025720` passes the fresh clean normal build (60.35s), zero compiler warnings. Each target was individually built with checked exit0/warnings0 before its sequential run with RISE_MEDIA_PATH set. SourceHygiene169/0; CST458 MATCH/0 DRIFT/0 UNCOVERED/0 STALE; full Manifold388/0 plus legacy assertions (7.38s build/0.38s run); isolated photon context passes; full OIDN227/0 (24.17s build/130.56s run), retaining the same raw and denoised hashes across clock-perturbed replicates. SMS attenuation96/0 (6.19s), reflection18/0 (3.07s), coating72/0 (8.14s), shipped10/0 (0.25s); fresh render target build23.85s. Exterior unit-only125/0 (24.45s build/0.62s run). All accepted r4 proof/sanitizer/normal steps have the expected exit and zero build warnings. Records: `/tmp/rise-dl435-r4-gate.json`.

The prior complete r3 normal integration, SMS/split weave31/0 and portable temporary-path controls remain the broad evidence for unchanged production paths. The round3 corrections change only reused public photon output buffers and Accurate OIDN aux ownership; all three production photon callers instantiate empty vectors, and the explicit fresh/reused oracles check the reset. Full OIDN and all changed deterministic/render controls run fresh above. This is targeted relevant revalidation, not a claim that every long render was repeated after an equivalent default reset. Clean Xcode configurations are running next.

Round3 fresh clean Xcode Deployment/RISE-GUI57.82s and Opto/RISE-GUI-Opto102.61s both exit0 with zero compiler/linker warnings. Each emits only the AGENTS-excluded AppIntents metadata notice. The same checkout-only CPU OIDN2.5.0 dependency is retained, without project changes or suppressions; actual GPU execution remains unmeasured. Records: `/tmp/rise-dl435-r4-xcode-gate.json`. All required pre-review gates are complete. The candidate ledger has289 unique main rows,49 open and240 prospective closures, highest440/next441 (404/405 unused). DL-436/439/440 are implemented and gated but unstruck pending integration review/dispositions; DL-437/438 remain pending user design choices. Master remains clean at115aee62e; no merge or push. The next independent round must review this final committed tree and report any additional P1, without silently approving the pending design deferrals.
