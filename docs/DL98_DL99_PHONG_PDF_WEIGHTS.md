# DL-98 / DL-99 — `IsotropicPhongSPF` / `AshikminShirleyAnisotropicPhongSPF`
# `Pdf`/`PdfNM` report the true generating density of `Scatter`+`RandomlySelect`

Scope: the same one-sentence bug pattern DL-67 Slice 0 fixed in
`SchlickSPF` (see [DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md)),
confirmed present in two more multi-lobe SPFs by that slice's own
sibling audit and closed here.

Status: **landed 2026-09-17** on branch `debt-phongpdf`.

---

## 1. `IsotropicPhongSPF` — the closed form

`IsotropicPhongSPF::Scatter`/`ScatterNM` draw the diffuse ray
(`kray = Rd`, direction-independent) and one (or per-channel, three)
specular ray(s) every call, subject only to a geometric-horizon accept
check, exactly like `SchlickSPF`. The specular kray is

```
kray(direction) = Rs * (N+2)/(N+1) * max(cos_o, 0),   cos_o = dot(direction, n)
```

— simpler than Schlick's Fresnel-from-half-vector, since `cos_o` is a
direct dot product with no reconstruction needed at the query direction
itself.

**`GeometricUtilities::Perturb`'s frame, in closed form.** `Scatter`
calls `Perturb(reflected, down, around)` with
`down = acos(pow(xi, 1/(N+1)))`, `around = 2*pi*b`. `Perturb` builds
`uvw.CreateFromU(reflected)` (so `U = normalize(reflected)`), applies a Y-
then-X-rotation in that local frame, and transforms back. Working
through the two rotation matrices (verified numerically to 0 error
against the library's own `Perturb` over 200000 random draws — see the
fix commit's diagnostic) gives, for ANY `N`:

```
direction = cos(down)*U + sin(down)*sin(around)*V - sin(down)*cos(around)*W
```

with `U,V,W = uvw.u(),uvw.v(),uvw.w()`. This is the identity the whole
fix is built on: it lets `Pdf()` recover a per-channel lane's sibling
direction from a query `wo` WITHOUT inverting the sampler at all.
Writing `ctI = dot(woNorm, U)` (= cos(down) for the hypothesis "wo is
lane i's own draw") and `sy = dot(woNorm,V)`, `sz = dot(woNorm,W)`, the
azimuth `around` is recoverable directly (it is geometric, not
tied to `N`: `Perturb`'s azimuth warp doesn't depend on the exponent at
all), and the OTHER lanes' polar angle scales as a pure power law:

```
cos(down_j) = ctJ = ctI ^ ((N_i+1)/(N_j+1))
cos_o_j = ctJ*nu + (sinDownJ/sinDownI)*(sy*nv + sz*nw)   [nu,nv,nw = dot(n,U/V/W)]
```

No sampler inversion, no transcendentals beyond one `pow` per sibling.

**`C_D` and `q_i`.** Same DL-67 Slice-0 aggregate construction,
`f(wo) = C_D*p_D(wo)*1{above horizon} + sum_i q_i(wo)*p_i(wo)`, with
`C_D` by a 16x16 deterministic stratified replay of the shared
`(xi,b)` square (`PhongDiffuseSelectCoefficient`) and `q_i` exact via
the closed form above (`PhongSpecularDensity`). `A_D` (the diffuse ray's
own geometric-horizon survival probability) uses the same Malley-disk
closed form, `(1+dot(n,geomN))/2`.

**A bug found and fixed mid-session, worth recording as a lesson.** The
first version of `PhongSpecularDensity` computed lane i's own realized
weight as `w_i = Rs_i*(N_i+2)/(N_i+1)*max(ctI,0)` — using `ctI =
dot(wo,U) = cos(down)`, the density variable — instead of `cos_o =
dot(wo,n)`, the SHADING-NORMAL cosine `Scatter`'s kray actually uses.
These are different quantities (`U = reflected`, not `n`) that happen to
coincide only at normal incidence. The bug passed a naive smoke check
(mass close to 1 on several configs, since a convex combination of two
individually-normalized densities is automatically mass-1 regardless of
whether the weights are right) but failed decisively once tested against
adversarial configs (low `N`, `Rs` dominant, tilted normal): mass as low
as 0.76 against the real sampler. Fixed by computing `cos_o = ctI*nu +
sy*nv + sz*nw` (the same U/V/W-projection identity used for the sibling
reconstruction, at `ctJ=ctI, scale=1` — i.e. lane i is its own `j==i`
case).

---

## 2. `AshikminShirleyAnisotropicPhongSPF` — two differences from Phong

Same "draw every lobe, pick one by REALIZED weight" shape, with two
wrinkles Phong/Schlick don't have.

### 2a. The specular kray has a much simpler closed form than expected

`GenerateSpecularRay` samples a half-vector `h` via an anisotropic
quadrant-warped `(phi via x, theta via y given phi)` scheme, reflects to
get `k2`, and sets

```
specular.kray = specFactor * cos_o,   specFactor = inv_actual_density * brdf
```

where `brdf` is `AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors`'s
specular output, `rhoSconst*(pow(hdotn,exponent)/den)*fresnel`, and
`inv_actual_density = 4*hdotk / (factor1*factor2)` is the reciprocal of
the sampler's OWN half-vector-to-solid-angle pdf. Substituting
`factor1=sqrt((NU+1)(NV+1))/(2*pi)`, `factor2=pow(hdotn,exponent)`,
`rhoSconst=sqrt((NU+1)(NV+1))/(8*pi)`, `den = hdotk*max(ndotk1,ndotk2)`:
the `pow(hdotn,exponent)` terms cancel exactly, `rhoSconst/factor1 =
1/4`, and the `hdotk` in the numerator cancels the `hdotk` inside `den`,
leaving

```
kray(direction) = fresnel(Rs, hdotk) * max(cos_o, 0) / max(cos_i, cos_o)
```

— independent of NU, NV, and the sampled half-vector's azimuth or polar
angle beyond `hdotk` and `cos_o`. Verified against the real
`GenerateSpecularRay` + `ComputeDiffuseSpecularFactors` construction to
1e-10 over 200000 random draws before any source was edited. This means
the KRAY at a query direction never needs sampler inversion at all — only
the "what would the OTHER per-channel lanes have drawn" bookkeeping does
(same anisotropic-quadrant inversion as `SchlickInvertPhi`, adapted for
the `atan(phi_root*tan(...))` warp: `AshikminInvertPhi`).

**One inversion bug found and fixed via the same numerical-verification
discipline.** The forward sampler's `y -> cos_theta` warp is
`cos_theta = pow(y, e_fwd)`, `e_fwd = 1/(cos_phi^2*NU + sin_phi^2*NV +
1)`. The DENSITY exponent used elsewhere in this same file,
`exponent_i = NU*hu^2 + NV*hv^2` (with `hu,hv` the h-vector's tangent
components, `sin_theta_h_sq` factored out), is related by
`e_fwd = 1/(exponent_i+1)` — NOT `exponent_i = 1/e_fwd`. Inverting the
theta warp at a query direction therefore needs `y = pow(hn,
exponent_i+1.0)`, and the first version of this fix used
`pow(hn, 1.0/exponent_i)` instead. Static review would not have caught
this (both look like a plausible "the inverse power" formula); a
20000-trial round-trip check (draw a real `(x,y)`, compute the forward
direction, recover `(x,y)` from it via the candidate inversion, compare)
found EVERY trial mismatched under the wrong formula and zero mismatches
under the right one — see the fix commit for the harness.

### 2b. The diffuse lobe's realized weight is ALSO direction-dependent

Unlike Schlick/Phong's constant diffuse kray, `Scatter`'s diffuse ray
here is

```
diffuse.kray = Rd*(1-Rs)*(28/23)*fromK1(cos_o)*fromK2(cos_i)
```

— a function of the diffuse ray's OWN sampled direction (`cos_o`), not a
material constant. This has two consequences, one straightforward and
one that took a second round to get right.

**Straightforward: `C_D(wo)`'s coefficient `wD` is evaluated exactly at
the query `wo`** (no averaging needed — `wo` IS the diffuse's own
hypothesized draw when we're weighting `p_D(wo)`), then `C_D` itself is
still a quadrature over the INDEPENDENT specular draw, same shape as
Phong/Schlick's.

**Not straightforward, and wrong in the first version of this fix: `q_i`
(the specular lobe's own selection coefficient) is NOT
`w_i(wo)/(w_i(wo)+wD(wo))` with `wD` evaluated at the SAME `wo` as the
specular query.** The diffuse candidate competing against a hypothesized
specular draw is a DIFFERENT ray, independently sampled, with its own
direction and hence its own weight — `wD` there must be an expectation
over the diffuse ray's OWN random draw, not the specular's. Using
`wD(wo)` directly (mirroring Schlick's `aD`-weighted formula, which is
only valid there because Schlick's diffuse weight is a material
constant) integrated to 0.92-1.05 mass against the real sampler on
EVERY ordinary test configuration — small individually, but a real,
structural defect, not noise. The fix adds a SECOND, independent
16x16 quadrature (`AshikminSpecularSelectCoefficient`) over the
diffuse's own `(px,py)` unit square (`CreateDiffuseVector`'s own
inverse-CDF: `cost=sqrt(1-py)`, `sint=sqrt(py)`, phi=`2*pi*px`),
averaging `w_i/(w_i+wD(diffuse draw))` (or `w_i/wSTotal` /
`1` when the diffuse draw is geometrically rejected, mirroring
`RandomlySelect`'s branches) over that grid.

**Cost of getting a two-sided-random-weight aggregate right: TWO
independent 16x16 quadratures per `Pdf()` call** (one for `C_D`'s
specular-side expectation, one per specular lane for its own
diffuse-side expectation) — roughly 6-9x SchlickSPF's single-quadrature
cost per lane. See §4.

---

## 3. Sibling audit (repeats DL-67 Slice 0 §7's exercise, scoped to the
   two rows this doc closes)

One-sentence bug pattern: *`Pdf()` reports a density other than the one
`Scatter()`+`RandomlySelect()` actually generate from.*

| SPF | verdict |
|---|---|
| `IsotropicPhongSPF` | **fixed this slice** (exact specular weight+density, branch parity, tilted-normal handling) |
| `AshikminShirleyAnisotropicPhongSPF` | **fixed this slice** (all three named factor errors, plus the exact-at-query-wo reconstruction and the two-sided diffuse/specular weight-averaging design) |
| `WardIsotropicGaussianSPF` | reconfirmed immune, and NOT coincidentally in the same way DL-67 found for Ward: `d.kray = pDiffuse->GetColor(ri)`, `s.kray = pSpecular->GetColor(ri)` — BOTH lobes' krays are direction-independent, so `MaxValue(Rd)`/`MaxValue(Rs)` raw-albedo weighting IS the exact realized selection weight. (Its branch-parity `alpha` averaging is a separate, un-investigated issue per DL-67's own note — out of this pattern's scope.) |
| `WardAnisotropicEllipticalGaussianSPF` | same, reconfirmed: `d.kray = pDiffuse->GetColor(ri)`, `s.kray = pSpecular->GetColor(ri)` |
| `CompositeSPF` | reconfirmed out of scope: `Pdf()` is a documented hard-coded 50/50 `top`/`bottom` placeholder (`// Equal weighting is the best approximation for this material` — not an attempt at `RandomlySelect` parity at all) |

No new sibling instances of the pattern found. `IsotropicPhongSPF`'s
per-channel loop and `AshikminShirleyAnisotropicPhongSPF`'s per-channel
loop were both independently checked against DL-101's "reused
`ScatteredRay` across lanes" pattern and found CLEAN: Phong's
`GenerateSpecularRay` unconditionally writes a fresh direction every
call (no early-return path that could leave a stale direction); Ashikmin's
`GenerateSpecularRay` returns `bool` and the caller only reads/uses its
output (and calls `AddScatteredRay`) inside the `if (...)` — a rejected
lane contributes NOTHING to the container, not a stale entry with a
freshly-computed weight. DL-100's "unflipped-frame back-face lobe loss"
pattern also does not reproduce: both files already sample around
`myonb`/the ray-facing-flipped normal throughout (visible in their own
extensive prior-fix comments).

**~~One accepted, documented approximation, not filed as a new debt.~~
STRUCK 2026-09-17 (review P1-1): the claim below was wrong on both
counts — the approximation was neither small nor "the same class" as
Slice 0's, and it is now FIXED rather than accepted.**

~~`AshikminShirleyAnisotropicPhongSPF::Pdf`'s `wD` uses
`MaxValue(Rd)*(1-MaxValue(Rs))` as a stand-in for the exact
`MaxValue(Rd*(1-Rs))` ... the SAME class of approximation `SchlickSPF`'s
DL-67 Slice-0 fix already accepted ...~~

Two things were wrong with that.

1. **Slice 0's simplification is PROVABLY EXACT; this one was not even
   close.** Schlick's is
   `MaxValue(rho+(1-rho)*F) == MaxValue(rho)+(1-MaxValue(rho))*F`, an
   identity for a scalar `F in [0,1]` because `x -> x+(1-x)F` is
   monotone in `x`. Ashikmin's `Rd*(1-Rs)` is a product of two
   *independently varying* RISEPels, and `max` does not commute past it:
   with `Rd=(.9,.1,.1)`, `Rs=(.1,.9,.1)` the approximation reads
   `0.9*(1-0.9) = 0.09` against the true
   `max(.81,.01,.09) = 0.81` — a factor of **9**; at
   `Rd=(.95,.05,.05)`, `Rs=(.05,.95,.05)` a factor of **19**. Calling
   the two "the same class" was the error that let a 19x weight error
   stand as documented-and-accepted.

2. **Nothing was testing it.** Every configuration in
   `tests/AshikminShirleySPFPdfConsistencyTest.cpp` used GREY
   reflectances, on which the two expressions are the same number. Gate
   1 could not have caught it either even with a chromatic row: moving
   mass between two lobes that each integrate to the same total leaves
   the total unchanged, and the measured `|intPdf - emitted|` on the
   three worst chromatic configurations is 0.00054-0.00467 against a
   0.01 band. Only the total-variation gate sees it.

**Fixed.** `Pdf` now forms the RISEPel product first and reduces once —
`wDBase = MaxValue(Rd * (1 - Rs))`, one extra RISEPel multiply — which
is the exact scalar `RandomlySelect` reads, because `Scatter` itself
builds `diffuse.kray = Rd*(1-Rs)*(28/23)*fromK1*fromK2` as a RISEPel and
hands THAT to `MaxValue`. The remaining per-direction factors
(`diffuseNorm`, `fromK1`, `fromK2`) are channel-independent scalars and
factor cleanly out of the reduction, so the split into a direction-
independent `wDBase` and a per-direction tail is itself exact. `PdfNM`
has a single wavelength lane and so was never affected; its `wDBase` is
just `rd*(1-rho)`, now read RAW to match `ScatterNM` — the earlier
`fabs()` was a guard the sampler itself does not apply, so on a negative
reflectance the density described a distribution nothing draws from.

Measured on the five new chromatic rows (total variation against 600k
real `Scatter`+`RandomlySelect` draws, threshold 0.012):

| row | TVD before | TVD after |
|---|---|---|
| `chroma .9/.1 vs .1/.9 Nu20Nv80` | 0.25208 | 0.00850 |
| `chroma .95/.05 Nu20Nv80` | 0.29464 | 0.00853 |
| `chroma .9/.1 Nu2Nv2 tilt20` | 0.07588 | 0.00766 |
| `chroma .9/.1 th=70 az30 Nu5Nv5` | 0.11131 | 0.00803 |
| `chroma backface .95/.05` | 0.29467 | 0.00831 |

Every grey row's `intPdf` / `emitted` / `TVD` is unchanged to the
printed digit, as the algebra requires.

---

## 4. Cost

Microbenchmark, 200000 `Pdf()` calls, one shading point (measured on a
machine also running several other concurrent debt-cleanup builds this
session — treat as order-of-magnitude, not precise):

| | pre-fix | fixed (kPhongQuadN/kAshQuadN=16) |
|---|---|---|
| `IsotropicPhongSPF::Pdf`, single lobe | ~17-20 ns (trivial closed form) | ~660-1500 ns |
| `IsotropicPhongSPF::Pdf`, per-channel (3 lanes) | ~17-20 ns | ~1840-5900 ns |
| `AshikminShirleyAnisotropicPhongSPF::Pdf`, single lobe | ~a few tens of ns (mirror-direction-only formula) | ~1100-13000 ns |
| `AshikminShirleyAnisotropicPhongSPF::Pdf`, per-channel (3 lanes) | ~a few tens of ns | ~3100-32000 ns |

The wide range on the "fixed" column reflects real measurement noise
from concurrent machine load (this session ran alongside several other
worktrees' builds), not a change in the algorithm; the SHAPE is stable
across repeats (per-channel costs ~3x single-lobe within one run, and
Ashikmin costs ~2-9x Phong's, consistent with running two independent
16x16 quadratures per lane instead of one).

**`kAshQuadN`/`kPhongQuadN` = 16 was measured, not assumed.** A trial at
`kAshQuadN=8` (the same order-of-magnitude speedup Schlick's own kSpecQuadN
decision considered) regressed `AshikminShirleySPFPdfConsistencyTest` from
38/0 to 38/7 failures — specifically the four `Nu2Nv2` (low, equal
exponent) tilted-normal adversarial rows, the same regime DL-67 Slice 0's
own `kSpecQuadN` decision found most sensitive to quadrature resolution.
16 is kept.

**Whole-render visible effect**, `oidn_denoise FALSE`, `pixel_filter box`,
EXR `Rec709RGB_Linear`, single un-averaged run per side (RISE seeds from
an unsynchronized libc `rand()`, so this is a directional measurement,
not a converged one):

- `scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene` (one
  `isotropic_phong_material` sphere, `N=60`), PT 32spp at 160x160: frame
  mean moved +0.06% (dominated by the other 8 non-Phong objects at low
  spp); the phong sphere's own screen block moved **+7.65%**
  (0.6874 -> 0.7400 mean luminance in an 8x8 block decomposition).
- `scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene` (path
  guiding and adaptive sampling disabled for this comparison;
  `ashikminshirley_anisotropicphong_material` panels), PT 32spp at
  160x120: frame mean moved +0.04% (large emitter-dominated scene); per-
  block deltas on the Ashikmin panels' own screen regions ranged from
  **-28% to +43%**, consistent with the pre-fix mass error's own
  magnitude (up to 43% off on the adversarial red-proof configs).

---

## 5. Gates

`tests/IsotropicPhongSPFPdfConsistencyTest.cpp` (46 checks: 12 original
configs mirrored from Schlick's own review set, plus 4 adversarial
low-N/high-Rs/tilted-normal rows added after the first 12 turned out not
to discriminate the bug on gate 1 alone — see the test file's own header)
and `tests/AshikminShirleySPFPdfConsistencyTest.cpp` (38 checks: 9
ordinary configs varying incidence AND azimuth relative to the object's
tangent frame, a per-channel Nu/Nv row, 2 tilted-normal rows, 4
adversarial low-equal-exponent/high-Rs/tilted rows) both gate `Checks: N
Failures: 0` at HEAD. Full red/green tables and the captured red-proof
output are in the two fix commit messages (`7703785f`, `644a056b`).

Broader regression suites gated clean at HEAD: `SPFPdfConsistencyTest`,
`SPFBSDFConsistencyTest`, `PTGuidingMISPartitionTest` (63/0),
`PathValueOpsTest`, `LayeredWhiteFurnaceTest` (57/0 configs),
`MISWeightsTest` (59/0), `EnvLightBalanceTest` (116/0),
`BDPTStrategyBalanceTest` (66/0), `CstDeriveGoldenTest` (452 MATCH / 0
DRIFT), `SourceHygieneTest` (165/0). Clean rebuild, zero compiler
warnings, both files.
