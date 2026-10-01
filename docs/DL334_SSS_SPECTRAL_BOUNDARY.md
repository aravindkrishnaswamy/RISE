# DL-334: SSS boundaries at the transported wavelength

Spectral reflection already queried `GetValueAtNM`; diffusion transmission,
normalized entry `Sw`, and random-walk material selection could instead use the
RGB scalar. A measured curve with a matched spectral exterior could consequently
reflect zero and transmit zero at the same physical interface. The new public
regression first failed against original production (2016 checks, 568 failures;
worst partition error 1 at 380 nm). Sellmeier BK7 and measured curves exercise
this independently of the production Fresnel helpers.

Profiles now expose wavelength boundary queries with constant-profile defaults.
Burley and Donner-Jensen override both index and transmission. NM entry adapters,
shared vertex evaluation, diffusion sampling entry/exit, and PT/BDPT eye/light
coins use these queries. NM random-walk consumers prefer the material NM query
even when RGB parameters exist. RandomWalkSSSMaterial keeps its existing
coefficient snapshot and evaluates its IOR painter at the constructor's existing
dummy point for NM; this does not add spatially varying coefficient support.
Donner-Jensen spectral table construction also evaluates both layer indices at
its table wavelength; RGB tables keep their existing scalar policy.

The geometric containment seeder accepts an optional wavelength and passes it
to all six existing parity probes. PT/BDPT NM camera/light seeds, spectral photon
seeds, and legacy spectral rasterizer seeds pass the transported wavelength.
RGB photon, SMS, AOV, and Pel callers retain the default RGB query. HWSS cameras
compare every enclosing interface against the hero stack, including dispersive
outer media hidden by a constant inner medium. If any index differs, active lanes
trace independent NM paths before transport. The legacy rasterizer does the same
with CastRayNM and retains its original active-lane denominator.

At a HWSS SPF/SSS fallback, lane stacks are reconstructed with the same seeder
at the incoming segment midpoint, starting from the existing environment index.
The ray intersection is the nearest accepted hit: the open segment from its
origin to that boundary has no intervening accepted surface. Its midpoint lies
in the incident physical region for entry, exit, nested boundaries, and grazing
incidence. Public nested sphere crossings check that reconstruction agrees with
the transitioned incoming medium; grazing entry checks an exterior midpoint.
This uses the existing seeder's closed-volume/topology/tolerance conventions;
it does not infer a companion index by scaling the hero scalar. Exactly tangent,
open sheets and overlapping solids retain the pre-existing seeder limitations.

RGB policy is unchanged. ScalarPainterRGB channel anchors remain 611, 549, 465
nm; parser single-scalar views anchor all RGB slots at green 549 nm while keeping
full spectral evaluation. Raw programmatic per-channel painters keep their prior
v[0] scalar-profile convention. This change repairs NM without expanding the RGB
profile API to three distinct boundaries. Constant index and exact RGB Fresnel
controls pin that policy. The virtual interface additions require consumers to
be rebuilt; clean original/candidate library and exact test relinks verify this.

The exact DL-306 boundary law and its hemisphere normalization are unchanged.
The independent test integrates scalar polarized Fresnel numerically and uses
Snell reciprocity below unity, with matched, immersed and TIR angles. DL-04
complete-event radiance eta squared is unchanged: nested public SPF entry/exit
returns to environment 1.17 with net eta-square product 1. No BSDF density,
strategy admission, recurrence, or MIS weight arithmetic changes.

The public furnace uses conservative diffusion and random-walk slabs in a flat
spectral environment. Air, constant ambient, measured dispersive ambient, and
a constant inner medium inside a dispersive outer sphere run at four wavelengths
through real IntegrateRayNM/HWSS. Lambertian and matched-index refractor controls
pin the broadened containment path. Each lane gets 60000 paths. The independent
reference is environment radiance times camera index squared; the mean band is
0.015 + six measured standard errors, with standard error strictly below 0.02.
All four HWSS lanes are active. Earlier planar enclosure fixtures failed the
precision cap through trapped diffusion/TIR heavy tails; raw failed logs are
retained outside the repository. Concentric enclosures let exiting rays escape,
without loosening that cap. Deterministic TIR coverage remains in the partition
suite. This furnace is bounded stochastic evidence, not proof for all geometry.

Affected regression coverage includes diffusion/RW sampling, exterior invariance,
SSS radiance, matched grazing, shared BDPT vertex evaluation, VCM recurrence,
MLT HWSS normalization, photon wavelength measure, containment/stack regression,
legacy spectral final gather, and full golden/source hygiene suites. The new
furnace directly exercises PT NM/HWSS; legacy rasterizer and photon changes have
source audits plus existing affected suites, not a new camera-inside full image
or photon-distribution oracle. World-position graded-index machinery has its
existing scalar field contract; this patch does not introduce dispersive graded
fields. Independent reviews assess these explicit coverage limits.
