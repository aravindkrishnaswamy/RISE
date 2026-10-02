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
adjustment and existing explicit value-salt contract. Final rows are
DL-390 and DL-391 only.


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


## Final clean build and serial integration gate

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
