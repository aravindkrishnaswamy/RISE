# DL-04: subsurface radiance convention and discriminating measurements

Base master: `df5e17f996967a5a7a5ed379bae4b6b3a2efe3ac`.
This records the source audit, measured probes, and the coarse convention gate
at test commit `25421dd6`. Production transport is unchanged. DL-04 remains
open: formal review, mutation discrimination and the final gate record are
pending. Defaults below are the current `25421dd6` snapshot, pending further
convergence decisions; they are not a finalized release contract.

## Complete event versus one boundary

For a path from exterior index n_e into n_s and back into the same exterior,
the two basic-radiance factors multiply to
`(n_e/n_s)^2 * (n_s/n_e)^2 = 1`. RISE samples that complete subsurface event
directly, preserving the exterior stack. It does not first sample a surface
transmission carrying one of those factors.

The old PBRT-v3/v4 multiply-versus-divide account is incorrect. In verified
PBRT-v4 revision `b4ce9687e6c695f5582997c61b0c66cf064bdb4a`, the
[ordinary dielectric transmission](https://github.com/mmp/pbrt-v4/blob/b4ce9687e6c695f5582997c61b0c66cf064bdb4a/src/pbrt/bxdfs.cpp#L98)
applies the reciprocal square, while
[NormalizedFresnelBxDF](https://github.com/mmp/pbrt-v4/blob/b4ce9687e6c695f5582997c61b0c66cf064bdb4a/src/pbrt/bxdfs.h#L1023)
applies the square on the BSSRDF side. The
[PBRT-v3 explanation](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/The_Path-Space_Measurement_Equation)
explicitly identifies its BSSRDF correction as the second refraction,
with the first handled by the surface BSDF. Adding only the adapter factor
to RISE would omit that caller context and introduce an unmatched multiplier.

## Reachability corrections

The shipped SubSurfaceScatteringMaterial, RandomWalkSSSMaterial and
DonnerJensenSkinBSSRDFMaterial all construct SubSurfaceScatteringSPF with
`bAbsorbBackFace=true`. Its membership-selected inside branch returns before
the fallback exit refraction and pop. Initial containment can be seeded,
but that does not make a parser-created SSS material emit the exit ray.
The original source document conflated initial membership with reachability.
Without membership the SPF takes its outside branch; it does not reach an
ordinary no-op pop either.

A standalone SPF constructed with the flag false can reach the fallback.
That is separate API behavior, not production BSSRDF coverage. Its current
pre-pop destination-IOR read is tracked separately as DL-51. The
camera in the main rendered comparison stays outside the SSS solid.

## Why the camera ratio is insufficient alone

Moving the camera across a fixed enclosing interface gives both materials
a common observer transform. Multiplying every SSS event by a wrong
constant can therefore leave the ratio-of-ratios unchanged. With a Fresnel
outer boundary, the reflected environment adds another term; it is not
purely a common multiplier.

The test retains the requested matched-material geometry and observer
comparison, using an explicitly ideal, non-reflecting IOR enclosure for
that wiring control. It also measures the no-enclosure absolute furnace.
The explicit slab boundary remains a dielectric with Fresnel reflection.
A configurable Fresnel enclosure is a diagnostic, not assumed equivalent
to the pure observer transform. All constituent means must be positive and
finite before division.

## Matched materials and controls

The fixture uses a broad closed slab with smooth normals, isotropic
scattering and matched neutral physical coefficients (`sigma_a=0`,
`sigma_s=2`, `g=0`, physical mfp 0.5). Zero absorption supplies the
primary conservative control: Burley's integrated profile albedo then
matches the conservative volume limit. Equal nonzero absorption/scattering
coefficients alone would not match Burley's empirical hemispherical
reflectance to explicit volume transport.

The matrix includes dielectric-plus-homogeneous-medium, diffusion SSS,
and random-walk SSS. Orthographic viewing keeps directions/framing fixed
between camera positions. An initial four-spp white Lambertian sphere supplies a diagnostic
lighting/capture reference of one. A non-unit IOR sweep and an absolute-energy guard
must reject either unmatched square multiplier; the camera ratio alone
must not be described as proving that property.

Finite profile support, slab size and walk/recursion truncation remain
limits of the measurement. The cap comparisons below show a substantial
RW change; the coarse tolerance is not a convergence certificate. Sample counts are perfect
squares, renders are sequential, denoising is disabled, filtering is box,
and output placeholders are relative linear EXR. Captured raw linear pixels
and alpha must be finite; each render is reseeded, without claiming
bitwise repeatability under worker-side random scheduling.

## Distinct confounds identified before measuring

- DL-49: exterior IOR is hardcoded as air in profile Fresnel and random-walk
  refraction/entry factors, while surface SPF reflection reads the stack.
  Changing the surrounding medium is different from moving only the camera.
- DL-48: the implemented Schlick transmission has cosine-hemisphere integral
  `20*(1-F0)/21`, while Sw uses `(41-20*F0)/42`. Those are different
  normalizations. This is not an eta-square omission. Independent exact
  arithmetic at eta=1.5 gives a normal conservative prediction of
  `1603/1675 = 0.957014925373` under an ideal unit-integral spatial profile. This
  is an analytical prediction, not a measured render; the exact arithmetic
  is retained with the evidence.
- DL-50: NM random-walk exit multiplies by survival transmittance after a sampled
  survival outcome; RGB divides by its event probability. Do not assume
  spectral agreement as an independent reference for this row.
- Random-walk exits use a diffuse angular approximation; ballistic and
  low-scatter transmission are not exact dielectric-volume equivalents.

These have separate ledger entries or documented model limitations;
none licenses a guessed constant repair in this row.

## Current convention-gate contract (25421dd6; review pending)

An ordinary invocation runs the full three-model, three-topology matrix,
with four trials by default. Before rendering, it runs four separately
seeded local IndependentSampler helper streams of 100,000 attempts each.
Each helper trial checks `0.9 < J < 1.1`, where
`J = mean_spatial_red / FtExit`, and checks every RGB channel of
`F0 + mean_full_event` within 20% of the unit conservative furnace.
Rejected helper samples contribute their physical zero; the denominator
is all attempts, not only successful samples.

The rendered air-furnace mean must be within 3% of one for explicit
volume and within 20% for diffusion and RW, channel by channel. The
observer ratio-of-ratios must be within 10% of one, a wiring check only.
These are consistency pins intended to distinguish an unmatched eta
square from the measured baseline. They do not assert exact energy
conservation, independently established QMC uncertainty, or closure of
the residual mechanisms below. The first unchanged-library gate at 256 spp passed; mutation rejection remains to be run.

Current defaults are 16x16, 1024 spp, four trials, seed base 1000,
material IOR 1.5, ellipsoid radii `(40,40,10)` centered at z=-10,
and volume/RW/path caps 256/512/1024. Ordinary gate mode permits IOR
1.5 or 2, requires at least 256 spp and four trials, curved R=40 and
an ideal enclosure. The camera remains outside the SSS solid; water
views place it inside or outside a separate IOR-1.33 enclosure.

`--probe` retains structural, finite-value and activity guards, but
applies no convention/energy bounds. `--air-only` and `--helper-only`
imply probe mode. The former omits water views; the latter loads only
diffusion air and performs no rasterization. `--helper-attempts` defaults
to 256 for these diagnostic helper/coverage calls; the ordinary gate's
independent prepass overrides it to 100,000. `--outer-fresnel`, `--flat`,
`--slab-radius`, `--samples`, `--trials`, `--seed`, `--ior` and the three
cap flags expose diagnostics. Sample counts must be perfect squares.
Run from the repository root with `RISE_MEDIA_PATH="$PWD/"`; owned scene
inputs under `rendered/sss_radiance_scaling/` are removed on return.

## Measured probe record

The following values come from `.claude/debt-DL04/logs/`. They were
measured on unchanged production transport by the earlier probe fixture;
their passing guard counts are not executions of the newly added energy
bounds. No current-default gate pass is claimed here. RGB was retained
in the logs; the neutral scenes' red-channel means are quoted below.
All these render groups use four trials and seed base 1000.

`curved-base-measure.log`: 1024 spp, IOR 1.5, R=40, caps
256/512/1024, ideal enclosure:

| Model | Air | Water camera inside | Water camera outside | Observer ratio / explicit observer ratio |
| --- | ---: | ---: | ---: | ---: |
| Explicit volume | 0.9953495254 | 1.768346136 | 0.9997097766 | 1 |
| Diffusion | 1.078322624 | 1.540974782 | 0.8830050964 | 0.986595165 |
| Random walk | 0.9764421176 | 1.387544233 | 0.7891255288 | 0.9940481122 |

Its terminal count is verbatim: `Guards passed: 90251 failed: 0. Provisional measurements complete; no physics acceptance band or eta closure claimed.`
The original `stdev`/`stderr` labels in that log describe repeat dispersion,
not independent integration uncertainty (see below).

Air-only sample/cap probes:

| Log | Spp | Volume/RW/path caps | IOR | Explicit | Diffusion | RW |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `air-base256.log` | 256 | 256/512/1024 | 1.5 | 0.9953150202 | 1.077335534 | 0.9781150275 |
| `air-highcaps.log` | 256 | 1024/2048/4096 | 1.5 | 0.9984468968 | 1.077336303 | 1.098107075 |
| `air-highcaps1024.log` | 1024 | 1024/2048/4096 | 1.5 | 0.9983269318 | 1.078324222 | 1.09768227 |
| `air-eta2.log` | 1024 | 256/512/1024 | 2 | 0.9814371556 | 1.033956666 | 0.8415164355 |

Each of those four logs ends verbatim:
`Guards passed: 29499 failed: 0. Provisional measurements complete; no physics acceptance band or eta closure claimed.`
Changing all three caps together does not isolate which cap causes each
shift. The RW shift is large, so the low-cap value near unity is not proof
of exact normalization; raising spp at the higher caps does not remove it.
The eta-2 run is an IOR sweep, not an eta-square code mutation.

Independent helper probes use local seeds 1000–1003, avoiding the fixed
pixel Sobol sequence. `full` below is the helper event before adding
surface `F0`; it is not the rendered furnace mean.

| Log | R / IOR | Attempts per trial | Four measured J values | Four measured full red values |
| --- | --- | ---: | --- | --- |
| `helper-independent.log` | 40 / 1.5 | 100000 | 0.9858711371, 0.9844916403, 0.9930006437, 0.9775698157 | 0.9047199685, 0.9035210941, 0.9103816862, 0.8955543034 |
| `helper-r20.log` | 20 / 1.5 | 1000000 | 0.9917207962, 0.9902040756, 0.9904943586, 0.9877099833 | 0.9093114065, 0.9076297311, 0.9085178144, 0.9056383627 |
| `helper-r80.log` | 80 / 1.5 | 1000000 | 0.9655905694, 0.9643886107, 0.9647241786, 0.960426414 | 0.8852703406, 0.8840727285, 0.8847940103, 0.8805264944 |
| `helper-eta2.log` | 40 / 2 | 1000000 | 0.9830108616, 0.98154723, 0.981414275, 0.9782654041 | 0.8010438175, 0.7997271832, 0.8000556125, 0.7971831266 |

The respective terminal counts, verbatim, are:

```text
Helper guards passed: 480406 failed: 0. Unconditional helper measurements complete; no energy acceptance band.
Helper guards passed: 4787419 failed: 0. Unconditional helper measurements complete; no energy acceptance band.
Helper guards passed: 4766717 failed: 0. Unconditional helper measurements complete; no energy acceptance band.
Helper guards passed: 4787391 failed: 0. Unconditional helper measurements complete; no energy acceptance band.
```

Independent spatial quadrature (`independent-quadrature.log`) at 200,000
steps gives J=1.00001599796, 1.00000103049 and 1.00000064454 for R=20,
40 and 80. The corresponding helper deficits grow with flattening; the
log's estimated disk-hole masses are 0.00924465427004, 0.0183757085347
and 0.0363039580927. This supports treating support loss separately from
an eta-square convention. At R=40, IOR 1.5, ideal sampled furnace arithmetic
gives 0.957015870352, versus 2.10328570829 or 0.447562609045 when multiplying
or dividing the complete subsurface contribution by eta squared. At IOR 2
the values are 0.926139012437, 3.37122271642 and 0.314868086443. These are
quadrature/arithmetic diagnostics, not executed production mutations.

## Planar probe origin (DL-52)

The first four-spp flat-box smoke returned diffusion air RGB mean
(0.04000010123, 0.04000011512, 0.04000005414), near the surface reflection
alone. This is a low-sample diagnostic, not an energy estimate. Source
inspection identifies a separate support omission: SampleEntryPoint
advances each probe from the exit tangent plane before finding its first
hit. Normal probes therefore skip the nearby coplanar face. The bottom is
within Burley's hardcoded effective range, but its distant profile value
does not replace the missing near-face contribution.

A matched ellipsoid with radii (40,40,10), centered at z=-10, activates
nearby sampling while retaining the box bounds. The first curved smoke
recorded 113 legacy z-only hits among 256 actual helper attempts in the air
case, and passed 24052 guards with zero failures. That z-only criterion
can include distant sidewalls and is not the current near-top test.
Current near-top coverage requires `entryPoint.z > -1`, distance from the
central hit below 5, and `entryGeomNormal.z > 0.9`. It remains a diagnostic:
curvature, finite support and small probe advances still need bounds.
The original flat failure stays recorded in DL-52; curved activity does
not close that defect or prove exact material equality.

The later `helper-flat.log` runs four 100,000-attempt independent trials.
Their valid counts are 75031, 75282, 74752 and 75061, but every near-top
count is zero. Measured J is 6.900921372e-07, 6.750339866e-07,
7.019267261e-07 and 6.582445719e-07. Its terminal line is verbatim:
`Helper guards passed: 382636 failed: 0. Unconditional helper measurements complete; no energy acceptance band.`
That successful diagnostic exit does not turn the flat support failure
into a passing energy test.

## Repeat dispersion is not QMC uncertainty

PathTracingPelRasterizer derives its Sobol scramble from pixel coordinates,
independently of the libc seed set before each render. Repeated renders
therefore reuse those QMC samples; incidental jitter and fallback random
choices can vary. The fixture labels repeat dispersion descriptively and
does not use it as a confidence interval for the integral. Sample-count,
geometry and cap comparisons supplement the absolute convention check.

## Recursive environment MIS (DL-53)

The rendered diffusion air mean exceeds the independently sampled helper
mean. Source inspection confirms an independent positive extra term:
complete diffusion/RW branches add MIS-weighted environment NEE, then pass
the global radiance map and positive cosine PDF into recursive RayCaster.
Its explicit-map escape branch returns raw radiance before the global-map
MIS block, for RGB/NM/HWSS. The angular contributions consequently have
weights `w_NEE + 1` instead of complementary weights. This establishes
the sign, not a numerical attribution of the observed difference.

The ordinary smooth SSS BSDF is zero; its SPF emits delta Fresnel reflection,
so an extra surface diffuse lobe is not the mechanism. The iterative PT
loop already applies the correct map-identity condition. DL-53 carries
the recursive sibling and its direct miss-ray regression recipe.

## Existing focused-test logs; final gates pending

The recorded `gate-BSSRDFEntryPointTest`, `gate-BSSRDFSamplingTest`,
`gate-RandomWalkSSSTest`, `gate-SobolDimensionBudgetTest`,
`gate-SPFPdfConsistencyTest` and `gate-SPFBSDFConsistencyTest` logs report
all tests passed; their companion `.exit` files each contain `0`. They
are coverage evidence for the existing suite, not executions of the new
convention bounds. SourceHygiene initially ended with
`(scanned 306 test files) 164 passed, 1 failed.` because the generated Xcode
app bundle under `.claude/debt-DL04/xcode-derived/` contained a copied
skill document that its repository scan treated as an unregistered source.
Removing that owned generated directory, with no production-source edit,
yielded `(scanned 306 test files) 165 passed, 0 failed.` in
`gate-SourceHygieneTest-clean.log` (exit zero).

The clean Xcode build succeeded. Its only `warning:` lines were the
repository-exempt missing local OIDN library path and AppIntents metadata
notice. No compiler warning was reported.

The first actual convention gate, `convention-baseline256.log`, used
unchanged production transport at test commit `25421dd6`, 256 spp and
four trials with the original 256/512/1024 caps. It ended verbatim:
`Guards passed: 572084 failed: 0. Complete-event convention checks complete; exact energy conservation is not asserted.`
This is baseline-green consistency evidence, not a red proof. Final review,
mutation discrimination and final-default gates remain pending.
