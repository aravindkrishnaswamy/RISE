# Extended SMS Phase 2 validation

Phase 2 is in progress on `sms-ext-phase2`, based on master
`08552560b54b517b7a8a0696eb9d317953b26bbe`. It is not reviewed or merged.
No ledger row closes on this checkpoint.

The internal opt-in path samples one channel and one native R/T walk per
original trial. Point and spot lights use root-level estimator A, independent
conditional retries and the survival-weighted reciprocal tail. Failed
proposals remain in the original trial average. Reference deposits divide
only by the selected channel probability, emitter selection probability and
original trial count; event and caster probabilities are inside root
rediscovery. The dedicated `SMSShaderOp` remains legacy.

## Isolated evidence

`SMSExtendedReferenceTest --synthetic-only` passes **33/0** in
`.claude/logs/sms-phase2-synthetic-powered.log`. Each row uses four independent
discovery/retry salts. Ordinary laws and uncapped rare-root controls use
2,000,000 original trials per salt; the rare-root roulette control uses
100,000,000. The rare probabilities are 1/3 and 1/512, plus zero trials.
Increasing power retained the original three-sample-SD and 6% accuracy
checks; exploratory smaller runs were underpowered and are not red proofs
of a source defect. The weighted tail has unbounded cost and large outliers;
per-salt logarithmic retry histograms, tail/stopping counts, maximum reciprocal estimate and
maximum normalized deposit are printed. No speedup is claimed. Historical logs label the maximum reciprocal estimate
`maxK`; for roulette it is a weighted estimate, not a retry count. The test
now names it `maxReciprocal`. For the rare-root roulette mixture, all four
salts put the median retry count in [2,3], the 90th percentile in [4,7],
the 99th in [8,15] and the 99.9th in [128,255]. These histogram quantiles
combine both roots; they are not conditional rare-root quantiles. Roulette
stops affect 0.463–0.467% of completed loops. The maximum weighted
reciprocal estimate ranges from 1.11 to 1.77 million and the largest
normalized single deposit from 0.499 to 0.859. Derived values are recorded
in `.claude/logs/sms-phase2/synthetic-tail-summary.json`.

Geometry passes **3240/0** in
`.claude/logs/sms-phase2-geometry-final-run.log`: native R/T walks, R–T and
R–T–R, closed nested exits, start-inside, TIR, both indexed-mesh windings and
incidence sides, transformed objects, sphere/plane controls and central
finite-difference constraint Jacobians. Two distinct roots on separate
patches of one mesh remain distinct at scales 0.01, 1 and 100. Additional
very thin native patches consistently return zero proposals; this is not
positive root coverage or a mathematical root-isolation certificate.

Root comparison uses geometry, native UV/object context, event order,
object/material identities, evaluated interface indices and starting
membership. It caps the numerical comparison at sqrt(machine epsilon)
times endpoint distance and rejects large final inverse-Jacobian
corrections. That correction is a numerical diagnostic, **not a certified
nonlinear error bound**. Reference Newton polishing bypasses the legacy
fixed displacement dead zone and compares true surface points, undoing the
object's authored launch offset only for geometry. Native material contexts
retain their intersection UV, object position and raster coordinates.

## Production point/spot controls

`--production-only` passes **176/0** in
`.claude/logs/sms-phase2-production-depth-run.log`. It traces real RGB/NM PT
camera paths to a Lambertian receiver below an open double-sided indexed
mesh, both windings, with a pure mirror or index-1.5 refractor. Four salted
runs of 16,384 original samples use native emitter/BSDF domains, NM at 450
and 650 nm, and channel-separated RGB means and SDs.

The upward spot illuminates only by reflection. Its analytic virtual-image
reference is native BSDF times native Fresnel and emitted radiance divided
by squared virtual-image distance. The point fixture additionally includes
unoccluded direct light. Reference SD is zero. Receiver depth is one to
match this analytic transport order; an exploratory depth-eight run also
included repeated floor/mirror interreflection and was not a matched
reference. Every channel passes three measured SDs plus ordinary summation
roundoff. Observable compensated summation preserves the test's roundoff
bound under make/Opto reassociation.

## Unsupported-caster coupling

An index painter whose values happen to equal one but do not certify
position independence is outside the initial proposal domain. The prepared
caster data rejects its anchor switches together, even when a remote
supported mirror exists. This is anchor ineligibility: PT retains its light
and SMS is absent. It differs from the adopted composite policy, which
selects the existing solver and suppression for the entire scene.

The actual PT control passes **24/0** in
`.claude/logs/sms-phase2-coupling-green-run.log`. Four salts in each of the
two scenes are bit-identical between plain PT and extended mode, each
reading 1.41471060526. Committed checkpoint `f925c29c4` reproduces **14/10**:
PT stays lit at that value while extended mode reads zero. Evidence is
`.claude/logs/sms-phase2/caster-coupling--unsupported-only-red.log`.

Committed reference checkpoint `60a461692` fails **3226/14** in the current
geometry suite and **132/44** in the production point/spot suite. These
measure the earlier coarse root comparison, unresolved-context checks,
reference geometry/refinement and reversed spot direction, not the absence
of a newly introduced helper API. Evidence and checked build exits are in
`.claude/logs/sms-phase2/proof-results.json`.

Committed master `08552560b` fails **126/50** in the same production
native-API control. This rebuild uses master sources and dependent headers
coherently; the missing helper tests are not counted as failures. The
master red log is `.claude/logs/sms-phase2/native-master--production-only-red.log`.

The restored committed-proof runs pass synthetic 33/0, geometry 3240/0,
delta queries 176/0, production 176/0 and unsupported casters 24/0, with
zero compiler diagnostics. These are the `0b3caba51` checkpoint counts; a
later test adds a worker-configuration check and expands the caster cases.

The inherited-CSG extension uses closed double-sided meshes in both
windings, with and without a remote supported mirror. Although each clear
operand can be sampled, the effective CSG surface cannot supply uniform
proposal mass. The expanded committed test checkpoint `6d2c9891d` fails
77/20; rejecting such effective casters passes 97/0 in
`.claude/logs/sms-phase2/inherited-csg-green.log`. All four salts per case
preserve the positive PT control bit for bit. The formal restored-source
proof for this extension remains pending.

The spot-through-slab fixture compares shared pinhole camera renders with
matching native BDPT transport, both mesh windings and cones 30/45, 44/45
and 80/85. It uses four independent salts and channel-separated image
means, with the reference's own SD. An initial missing-shader setup and an
orthographic setup without BDPT's light-to-camera strategy were aborted;
those are invalid references, not source red proofs or passing renders.
The pinhole run completed the four salts for the front winding's three
cone settings. Cones 30/45 and 44/45 pass all RGB comparisons. Cone 80/85
fails red: extended 0.62896691331016075, SD 0.00019614427847902438, versus
BDPT 0.63006257853459957, reference SD 0.00008359126631271482. The
relative difference is −0.1739%, or −5.138 combined sample SDs. Green and
blue pass. Bands have not been widened.

At that checkpoint this triggered the user's unexpected-bias stop rule. Attribution remains
unresolved: it may involve estimator acceptance or the reference's camera
footprint/transport domain. No outside-contract remedy is implemented and
no Phase 2 merge or fresh review is claimed. The remaining reversed-winding
matrix was cancelled after the completed comparison; the process exited
143. That cancellation is an aborted measurement, not a zero trial or a
passing full slab suite. The source is unchanged from the tested production
checkpoint apart from the separately measured CSG gate; the newer weave
test and diagnostic labels await a checked build and execution. Raw
evidence is `.claude/logs/sms-phase2/slab-pinhole-run.log`, with derived
channel statistics in `.claude/logs/sms-phase2/slab-stop-results.json`.

Full regression/build gates, off-mode A/B cost measurements, the expanded
weave-gap coupling control and independent reviews remain pending.

## User ruling: resume after the wide-cone measurement

The user accepted this discrepancy as variance and explicitly authorized
continuing the full task. The wide-cone fixture retains the original
three-SD comparison in its output and is a reported measurement rather
than an accuracy assertion. Narrow-cone assertions remain unchanged. The
wide-cone result does not establish a DL-420 closure. The complete matrix
will be rerun after the remaining caster gate fixes.

## Unsupported pass-through sibling

A clear native glass pane behind an unsupported weave gap exposed the
same shadow/proposal coupling defect. Committed checkpoint `f44caa909`
passes 125/20: ordinary PT is positive while extended mode returns zero.
The prepared rejection now includes `HasDeltaPassThrough()` as well as
`CouldLightPassThrough()`. Restored source `72f784927` passes 145/0 in
`.claude/logs/sms-phase2/weave-green-run.log`, preserving each salted PT
result bit for bit across both pane windings and the remote-mirror control.
The coherent restored-source proofs reproduce 105/40 on `6d2c9891d`,
125/20 on `f44caa909`, and 101/44 on committed master `08552560b`.
Master's native production point/spot control is 127/50 with the added
worker check; this is the same 50 physical/accuracy failures as the earlier
126/50 checkpoint. Build exits are checked and compiler diagnostics are
zero. Restored candidate results follow after rebuilding HEAD. Evidence is
`.claude/logs/sms-phase2/caster-proofs/proof-results.json`.

## Neutral providers and forwarded composite walkers

The same caster-set audit covers legacy metadata-only providers. A native
clear-transmission metadata delegate with neutral optional hints and no
selected-domain provider fails 173/20 on committed `298bf64ea`: PT reads
1.41471060526 while extended mode reads zero. Adding a forwarding composite
SPF extension gives 185/32 on committed `f62eae39a`; its remote closed
double-sided composite should preserve the positive legacy mirror control.
The old prepared policy misses its material type and changes the image.

Prepared data now certifies material graphs only for exact audited native
types, recursing through native wrappers. An unknown provider makes
extended anchors ineligible; no arbitrary metadata sample certifies
absence of clear transmission. Queries likewise decline unknown native
subclasses. A forwarded `CompositeSPF` identifies the real walker from
static material data and selects the adopted scene-wide legacy policy,
with the first composite object's name. No virtual API or public metadata
layout is added. Corrected source `7d973fd7f` passes 217/0 in
`.claude/logs/sms-phase2/provider-guard-green-run.log`. Formal coherent
proofs reproduce 185/32 on `f62eae39a` and 165/52 on master; restored
candidate passes 217/0, geometry 3241/0, delta queries 177/0 and production
177/0. The production proof records 81,740,328 bytes peak process memory
footprint under `/usr/bin/time -l`; that includes rendering/library/test
state and is not a per-worker scratch bound. Arm64 record diagnostics are
config 112, root 184, reference counters 320 and domain vertex 1584 bytes.
Public legacy vertex/index records retain their Phase 1 layouts. Checked
library/test builds have zero compiler diagnostics. Evidence is in
`.claude/logs/sms-phase2/provider-proofs/proof-results.json` and its logs.

## Signed RGB emission self-audit

The committed signed-emission checkpoint `e4ecbb2f1` is **149 passed,
28 failed** in `.claude/logs/sms-phase2/signed-checkpoint-red.log`.
Native point and spot lights retain authored `color 1 -0.5 0.2`;
estimator A had discarded the negative green reflection. The fixtures
use actual double-sided meshes, both windings, mirror/glass casters,
query and PT production entry points, four salts and 16,384 samples
per salt. The reference uses native signed emission with zero reference
SD and the original three-SD plus floating-point-roundoff band.

Reference deposits now retain finite signed physical contributions. Both
RGB entry points accept any finite nonzero component; NM uses the same
nonzero validity rule. Legacy paths and their existing clamps are unchanged.
The default-source A/B and clean builds recorded at `4e211d99f` preceded
this correction. Its regression gate was stopped during
`ExteriorIndexInvarianceTest` and is recorded as aborted, not a pass.
A fresh final-source gate and committed-source red/green proof follow.

The coherent committed-source proof is complete in
`.claude/logs/sms-phase2/signed-proofs/proof-results.json`: checkpoint
`e4ecbb2f1` is 149/28 and native master `08552560b` is 111/66 in
`--signed-only`. The latter combines legacy coverage/accuracy failures and
is not a count of signed-clamp defects. Restored `89a9ab0a6` passes signed
177/0, unsupported 217/0, geometry 3241/0, delta 177/0, production 177/0
and synthetic 35/0. All library/test build exits are checked with zero
compiler diagnostics. Production's `/usr/bin/time -l` peak memory
footprint is 82,379,280 bytes for the whole test process, not a worker
scratch bound. Pre-fix cost, sanitizer and aborted full-gate logs are
preserved under `.claude/logs/sms-phase2/checkpoint-4e211d99f/`; the
fresh final-source pipeline is running.

## Corrected-source off-mode cost and output gate

Actual interleaved library/test builds compare master `08552560b` with
`96b73057ddeea4e7068b3a198bacc4139cb7d17d`, four salted trials per fixture, identical
worker policy and sequential renders. All 16 paired output hashes and
channel means are identical. Time includes the same native test-render
work on each side; means and sample SDs are seconds.

| Fixture | Master mean ± SD | Candidate mean ± SD | Paired change % mean ± SD |
|---|---:|---:|---:|
| RGB k1 | 0.961168094 ± 0.006179403 | 0.942920094 ± 0.002791386 | -1.895499 ± 0.690842 |
| RGB k2 | 0.734318333 ± 0.003011683 | 0.726885292 ± 0.008980231 | -1.013908 ± 0.946574 |
| NM | 1.710615302 ± 0.005328788 | 1.715625479 ± 0.002314363 | 0.293572 ± 0.321692 |
| HWSS | 5.743522740 ± 0.012806160 | 5.740002062 ± 0.018625509 | -0.061426 ± 0.147112 |

Evidence: `.claude/logs/sms-phase2/phase2-interleaved-cost.json` and
each `phase2-interleaved-{A,B}{0..3}.log`, with checked build exits in
`cost-driver.log`. No whole-render speedup is inferred from these timings.
