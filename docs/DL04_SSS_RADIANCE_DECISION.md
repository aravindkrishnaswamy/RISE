# DL-04: subsurface radiance convention and discriminating measurements

Base master: `df5e17f996967a5a7a5ed379bae4b6b3a2efe3ac`.
This is the pre-measurement design and source audit. No radiance convention
has been changed, and the row is not yet closed.

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

The test will retain the requested two-material geometry and observer
comparison, using an explicitly ideal, non-reflecting IOR enclosure for
that wiring control. It will also measure the no-enclosure absolute furnace.
The explicit slab boundary remains a dielectric with Fresnel reflection.
A configurable Fresnel enclosure is a diagnostic, not assumed equivalent
to the pure observer transform. All constituent means must be positive and
finite before division.

## Matched materials and controls

Use a broad closed slab with smooth normals, isotropic scattering and
matched neutral physical coefficients. Set absorption to zero for the
primary conservative control: Burley's integrated profile albedo then
matches the conservative volume limit. Equal nonzero absorption/scattering
coefficients alone would not match Burley's empirical hemispherical
reflectance to explicit volume transport.

The matrix includes dielectric-plus-homogeneous-medium, diffusion SSS,
and random-walk SSS. Orthographic viewing keeps directions/framing fixed
between camera positions. An initial white Lambertian control validates
lighting and capture. A non-unit IOR sweep and an absolute-energy guard
must reject either unmatched square multiplier; the camera ratio alone
must not be described as proving that property.

Finite profile support, slab size and walk/recursion truncation require
convergence checks before assigning a tolerance. Sample counts are perfect
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

These require separate ledger entries or documented model limitations;
none licenses a guessed constant repair in this row. Measurements and the
final gate/review record will replace the pending conclusions below.

## Verification status

The fresh base make build succeeded without compiler warnings. The new
measurement fixture is being authored and must be committed before its
first execution. A baseline-green observable will be called a consistency
pin, never an unfixed red proof. No transport fix is assumed in advance.
