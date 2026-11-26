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
even when RGB parameters exist. RandomWalkSSSMaterial evaluates its IOR
painter at the constructor's existing dummy point for NM; this does not add
spatially varying coefficient support. (Superseded for the coefficients by
DL-374, below: as written here the NM query returned the RGB coefficient
snapshot, which made every spectral random-walk render achromatic.)
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

## DL-374 follow-up (`debt-dl374`, 2026-10-01): coefficients at the wavelength

`RandomWalkSSSMaterial::GetRandomWalkSSSParamsNM` used to return the
construction-time RGB `sigma_a`/`sigma_s` snapshot (only the IOR was evaluated
at lambda), and `RandomWalkSSS::SampleExit`'s NM mode collapsed that triple to
its Rec. 709 luminance, so every wavelength walked one grey medium and every
spectral integrator -- PT, PT `hwss TRUE`, BDPT and VCM measured, MLT by its shared BDPT generator -- rendered the
material achromatic. The NM query now evaluates the absorption and scattering
painters at lambda (`IScalarPainter::GetValueAtNM`, the query the diffusion
profiles' `EvaluateProfileNM` already makes) at the same snapshot point,
broadcast per `IMaterial`'s contract, and the walk reads a broadcast triple
exactly (a non-broadcast triple keeps the luminance fallback). Red-proof
`tests/RandomWalkSSSSpectralColourTest.cpp` (58/0; 26/32 with the two source
files reverted): the walk's NM weight at 650/550/450 nm equals the RGB walk's
R/G/B channel weight within |z| <= 0.4 (pre-fix every wavelength read 0.26,
z up to -187), and a chromatic white-furnace sphere (absorption 3.0/0.5/0.02)
renders B/R ~8.3-9.2 spectrally against RGB 5.6 (pre-fix 0.97-1.00). The
spectral render is not the RGB render: an RGB-authored coefficient is a
three-node piecewise-linear curve under spectral rendering
(`RGBScalarPainter`, nodes 450/550/650 nm), so green and blue agree within 5 %
while red reads 0.61-0.66x.  That gap is PREDICTED, not just attributed
(DL-374 review, 2026-10-01): integrating the RGB-render reflectance-vs-
sigma_a curve over this piecewise-linear sigma_a(lambda) against the env's
radiance spectrum, the CMFs and the XYZ->Rec709 matrix gives spectral
0.0632 / 0.3031 / 0.5721 (red 0.611x RGB); PT `hwss TRUE` renders
0.06321 / 0.30298 / 0.57396.  The red CMF's negative lobe sits where
this medium reflects most.  Non-HWSS PT/BDPT/VCM spectral read red ~6 %
above the prediction (0.067-0.069); a grey-medium control shows the same
+3.5 % red before and after the fix, so that offset is the non-HWSS
spectral path's wavelength-sampling residual, not DL-374.  Note also that
`RGBScalarPainter` places an RGB triple at 450/550/650 nm while
wavelength-varying painters use `ScalarPainterRGB::kChannelNM`
611/549/465 nm in RGB mode -- an older, separate convention. BDPT `hwss TRUE` rendered this material grey
(B/R 0.94) because its companions inherited the hero's walk weight (DL-357); since 2026-10-02 BDPT/VCM/MLT
terminate the companions at a random-walk jump and re-price a diffusion jump per companion.
