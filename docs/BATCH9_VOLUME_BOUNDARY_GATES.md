# DL-474: certified single-boundary volume normalization

VCM's 3-D merge ball previously divided by its full volume even where an
opaque sphere or the containing medium box removed part of that volume.
This slice divides the kernel by the available fraction phi and multiplies
the volume MIS factor by phi. The query radius, thinning probability, surface
normalization and surface recurrence identity remain unchanged.

## Scope and support

A scene object manager is borrowed for the duration of each frame/pass.
AABB and signed lower distances can rule out a cut; pricing a cut requires
an exact BoxGeometry or SphereGeometry distance under the object's similarity
transform certification. That certification includes the existing 1e-12
similarity tolerance; no stronger numerical-exactness claim is made here.
For one box face, phi = 1/2 + 3a/4 - a^3/4, where a is clearance/radius.
For an opaque external sphere (or another explicit-medium sphere), phi is
one minus the exact two-sphere lens volume divided by the ball volume.
Both fractions lie in [1/2,1], bounding the kernel correction by two.

Multiple cuts, box corners, an enclosing sphere medium, uncertified transforms
and unknown geometry retain the old kernel. Zero signed distance is uncertified.
The medium cookie and opaque-segment filters on candidate vertices remain.
The same geometric helper is used by RGB/NM/HWSS conversion and evaluation;
measured render evidence below is RGB. DL-474 remains PARTIAL.

## Self-review and cost

The risks checked were treating a lower bound as an exact distance, applying
phi twice, forgetting the q eta volume MIS factor at a recurrence/endpoint,
changing the surface eta relation, and retaining a scene pointer past its pass.
Only certified primitive cuts are priced; local normalization copies change
only the volume kernel and its MIS factor. Light/eye conversion and all volume
connection/merge endpoint factors use the same helper. The rasterizer supplies
the manager after each of its six volume-radius updates.

Certification scans world-visible objects for each relevant medium query;
AABB distance avoids expensive primitive work for remote objects. This is
O(number of objects), adds no sampling, and has no speedup claim. Large scenes
may need a proximity acceleration structure (P2), outside this correctness slice.

## Retained residual pin

VCMVolumeBoundaryRadiusTest checks the sphere lens independently, near/contact
cap ranges, all six box faces, unchanged factors away from a cut, and the
multiple-face fallback. Default: 16/0. --strict-corner-pin remains 16/1 (rc1).
The changed integrator TU and this test pass ASan/UBSan; other linked library
objects are not instrumented, so this is not a full-library sanitizer claim.

## Render evidence

The same red/green harness forces one thread, reserve count zero, K=1 and
64 eye-footprints, and checks those options before rendering. It compares the
black-sphere fog box at VCM512/PT512, salts 46900..46963. The unchanged paired
mean-difference band is three sample standard errors; no band was widened.

| Quantity | Master kernel | Corrected kernel |
|---|---:|---:|
| VCM mean / sample SD, n64 | .0357843 / .00177262 | .0370652 / .00184252 |
| PT mean / sample SD, n64 | .0368868 / .0000760915 | .0368868 / .0000760915 |
| VCM/PT, paired-ratio sample SD | .970113 / .0480166 | 1.00484 / .0499017 |
| Paired difference mean / sample SD | -.00110242 / .00177116 | +.000178481 / .00184058 |
| Three standard errors | .000664184 | .000690218 |
| Result | 1/1, rc1 | 2/0, rc0 |

An initial default-footprint run was not red; it is not counted as red proof.
A subsequently scheduled large-radius run used a stale executable and was
explicitly discarded. The valid red above came from a completed named build
whose executable asserts K=1 and footprint=64. Its return code was checked.

Related named builds return rc0. Existing gates: VCMRecurrenceTest103/0,
VCMLightPostPassTest72/0, VCMEyePostPassTest38/0 and VCMSpectralRecurrenceTest38/0.
VCMVolumeMergeTest A: VCM/PT .993360, ratio sample SD .0354785, n8, 2/0.
E: q=.01/q=1 VCM .939128, ratio sample SD .202783, n16, 2/0; its existing .2
band remains. That broad E check does not establish removal of all thinning bias.

VCMVolumeNoBoundaryHashTest uses global fog with no surface and a forced .05
surface merge radius, so volume merging remains active (about 35k stored medium
vertices per pass). Four ValueSalt values 474900..474903 give exact RGBA hashes
before and after: 6239592327718455421, 10361286704715844264,
13304065442282509844, 736822152037334529. Mean .355471623683, sample SD
.00150520762611, n4. The baseline links the same test and current remaining
library objects with the master-5bd23470d VCMIntegrator TU. The metadata header
layout is shared; the old TU uses the original kernel. This is a same-harness
kernel comparison, not a claim that the entire executable is an old-master build.

Additional strict E-PT check: q=.01 VCM512/PT512, n16 distinct salts,
ratio .951280, paired-ratio sample SD .222365. VCM mean .139086, sample SD
.032492; PT mean .146210, sample SD .000218493. Paired difference -.00712340,
sample SD .0325077, within unchanged 3SE .0243807: 2/0, rc0. This is a noisy
agreement gate, not proof that the unsupported floor/corner bias is gone.
All final named builds return rc0 with no warnings. Fresh review is required
on the committed state before merge.
