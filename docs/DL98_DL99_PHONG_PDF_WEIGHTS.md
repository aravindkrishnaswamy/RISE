# DL-98 / DL-99 — `IsotropicPhongSPF` / `AshikminShirleyAnisotropicPhongSPF`
# `Pdf`/`PdfNM` report the true generating density of `Scatter`+`RandomlySelect`

Scope: the same one-sentence bug pattern DL-67 Slice 0 fixed in
`SchlickSPF` (see [DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md)),
confirmed present in two more multi-lobe SPFs by that slice's own
sibling audit and closed here.

Status: **landed 2026-09-17** on branch `debt-phongpdf`, then revised the
same day by an external review pass whose three P1 findings are folded in
here: the diffuse selection weight's chromatic reduction order (§3), the
DL-100 sibling verdict (§3), and the two chi2 sub-tests that were still
skipped above comments describing the just-fixed bug (§5).  §4, §6 and
§7 replace numbers this document quoted from single un-averaged runs.

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
freshly-computed weight. ~~DL-100's "unflipped-frame back-face lobe loss"
pattern also does not reproduce: both files already sample around
`myonb`/the ray-facing-flipped normal throughout (visible in their own
extensive prior-fix comments).~~

**STRUCK 2026-09-17 (review P1-3): that verdict was FALSE for
`AshikminShirleyAnisotropicPhongSPF`.**  The audit checked which frame
the lobes are SAMPLED around (`myonb` — correct in both files) and
stopped there.  It did not check which frame the realized krays and the
stored pdf are MEASURED in, and in Ashikmin those were three different
raw `ri.onb.w()` reads: `cos_o` (the specular kray's cosine),
`cos_o_diff` (the diffuse pdf and Schlick `fromK1`) and `cos_i`
(`fromK2`).  On a back-face hit they all come out with the wrong sign,
so the diffuse lobe's stored pdf and both transmission factors collapse
to exactly 0 while the specular kray goes NEGATIVE.  Measured over
600 000 real `Scatter` calls at a back-face hit, pre-fix:

| row | emitted mass | rays with negative `MaxValue(kray)` (min) |
|---|---|---|
| `backface th=10 Nu20 Nv80` RGB | 0.00047 | 599720 / 600000 (−0.3000) |
| `backface th=45 Nu20 Nv80` RGB | 0.03601 | 578394 / 600000 (−0.3015) |
| `backface th=70 az30 Nu5 Nv5` RGB | 0.37102 | 377389 / 600000 (−0.1416) |
| `backface tilt20 th=45 az60` RGB | 0.03839 | 594711 / 600000 (−0.3015) |
| `backface per-channel Nu/Nv` RGB | 0.01754 | 0 (the per-channel branch writes one channel and leaves the others at 0, so `MaxValue` clamps the sign away) |

against a `Pdf()` that integrated to 0.997-0.999 the whole time — which
is what identifies it as a SAMPLER defect, not a density one.  A
negative selection weight is worse than a lost sample: it is not a
probability mass, it reverses the CDF ordering inside
`ScatteredRayContainer::RandomlySelect`, and
`RandomlySelectNonDiffuse` (`SMSPhotonMap.cpp`,
`CausticPelPhotonTracer.cpp`, `CausticSpectralPhotonTracer.cpp`) returns
such a ray unconditionally, with no weight test at all.  Fixed by taking
all three cosines against `myonb.w()`; front-face behaviour is
bit-for-bit unchanged (no flip is performed there, so the two frames are
the same vector) and every pre-existing row is identical to the printed
digit.  `IsotropicPhongSPF` really is clean on this pattern — it binds
`n` once and uses it for the lobe, the kray cosine and the pdf alike.

The audit was then re-run properly, over every file in
`src/Library/Materials` containing a `FlipW`, asking the CONSUMED-field
question ("is a kray or pdf cosine taken against the raw `ri.onb.w()` in
a function that samples around a FlipW'd copy?"):

| SPF | verdict |
|---|---|
| `SchlickSPF` | refuted — kray is `rho+(1-rho)*fresnel`, no cosine at all; both accept-checks already use `myonb.w()`.  Its half-vector frame IS the raw `ri.onb`, but deliberately and documented; the back-face specular loss that causes is the separate, still-open DL-100. |
| `Ward{Isotropic,AnisotropicElliptical}...SPF` | refuted — krays are pure `GetColor(ri)`, no cosine; the pdf cosine already uses `myonb.w()`. |
| `CookTorranceSPF`, `GGXSPF`, `CoatedSPF`, `FabricSPF`, `WeaveSPF`, `SheenSPF`, `PerfectReflectorSPF` | refuted — each binds `n = myonb.w()` (or an equivalent `nEff`) and uses only that. |
| `LambertianSPF`, `OrenNayarSPF` | refuted — their two raw `ri.onb.w()` pdf reads are inside `fabs()`, which CLAUDE.md's own DL-70 immunity list already names. |

No new rows opened.

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

**RE-MEASURED 2026-09-17 (review P2-1/P2-4).**  The table that stood here
before quoted ranges up to 20x wide ("~1100-13000 ns") taken on a machine
running several concurrent worktree builds; it is replaced.  The
protocol now is: three separately linked binaries against three library
states, run ALTERNATELY in the same session, each reporting the best of
15 timed blocks of 50 000 `Pdf()` calls on pre-generated directions
(so no RNG in the timed loop), with an UNTOUCHED reference workload
(`IsotropicPhongSPF::Pdf`, single lobe) measured inside every process so
machine drift can be divided out.  n = 5 alternating rounds.

| `Pdf()` configuration | pre-slice `a4495f94` | shipped (16x16 grids) | ratio |
|---|---|---|---|
| `IsotropicPhongSPF::Pdf`, single lobe | 18.8 ns | 486 ns | 26x |
| `AshikminShirleyAnisotropicPhongSPF::Pdf`, single lobe | 22.0 ns | 2576 ns | 117x |
| `AshikminShirleyAnisotropicPhongSPF::Pdf`, per-channel (3 lanes) | 22.0 ns | 7651 ns | 348x |
| `AshikminShirleyAnisotropicPhongSPF::PdfNM` | 23.5 ns | 2594 ns | 110x |

(The pre-slice column is stable to +/-0.2 ns across runs; the shipped
column's own spread over n=12 is +/-30 ns on the Phong reference,
+/-230 ns on Ashikmin single-lobe and +/-732 ns on per-channel.)

### 4a. The `kAshQuadN` hoist was implemented, measured, and NOT kept

The review's P2-4 asked for the lane-independent 16x16 diffuse draw grid
inside `AshikminSpecularSelectCoefficient` to be hoisted out of the
per-lane loop: it depends only on `wDBase`, `cos_i` and `geomN`, all
fixed for a whole `Pdf()` call, while the per-lane inputs enter as three
scalars.  In the 3-lane per-channel case that is 512 of roughly 1536
grid cells per call, ~33% of the arithmetic, provably redundant.

It was implemented exactly that way (a once-per-call
`AshikminDiffuseDrawTable` of 256 `wD` values, with -1 marking a cell
Scatter's geomN gate would have rejected, shared by every lane) and
measured against the un-hoisted build under the protocol above,
ratio-normalised against the in-process reference to remove drift, n=12:

| configuration | un-hoisted (Ash/ref) | hoisted (Ash/ref) | delta | t |
|---|---|---|---|---|
| `Pdf`, single lobe | 5.288 +/- 0.175 | 5.464 +/- 0.306 | +3.31% | +1.72 |
| `Pdf`, per-channel (3 lanes) | 15.727 +/- 1.067 | 16.213 +/- 2.070 | +3.09% | +0.72 |
| `PdfNM` | 5.327 +/- 0.451 | 5.323 +/- 0.227 | −0.07% | −0.03 |

i.e. no win anywhere, and if anything marginally negative.  It was
therefore reverted rather than shipped: it adds a 2 KB stack buffer and
a second data structure to a construction whose exactness is the whole
point, for nothing measurable.

The likely reason, stated as an inference rather than a measurement: the
specular-side grid is much CHEAPER per cell than it looks, because its
only transcendental is `pow(1 - cost*0.5, 5.0)` — a compile-time
integral exponent that `-ffast-math` folds into multiplies — while
`AshikminDiffuseSelectCoefficient`'s own grid evaluates a genuine
`pow(y, expo)` with a runtime exponent per cell.  Materialising 256
doubles then costs about what recomputing them costs.  Whoever wants to
move this number should attack the DIFFUSE grid (or `kAshQuadN` itself,
see §6), not this one.

## 5. Gates

`tests/IsotropicPhongSPFPdfConsistencyTest.cpp` (46 checks: 12 original
configs mirrored from Schlick's own review set, plus 4 adversarial
low-N/high-Rs/tilted-normal rows added after the first 12 turned out not
to discriminate the bug on gate 1 alone — see the test file's own header)
and `tests/AshikminShirleySPFPdfConsistencyTest.cpp` (99 checks: 9
ordinary configs varying incidence AND azimuth relative to the object's
tangent frame, a per-channel Nu/Nv row, 2 tilted-normal rows, 4
adversarial low-equal-exponent/high-Rs/tilted rows, 5 BACK-FACE rows and
5 CHROMATIC-reflectance rows added by the 2026-09-17 review pass, each
row now carrying three gates rather than two) both gate `Checks: N
Failures: 0` at HEAD. Full red/green tables and the captured red-proof
output are in the fix commit messages.

**Gate 1's domain is the FULL SPHERE, not the hemisphere** (review
P2-3).  `IsotropicPhongSPF::Scatter` has no `dot(dir,n) >= 0`
accept-check at all — only the geomN gate — and `Perturb` around
`reflected` reaches `down = pi/2`, so at low exponent and grazing
incidence a real fraction of emitted specular rays point BELOW the
shading horizon; their kray is `r_max(cos_o,0) = 0`, so they never win a
two-ray `RandomlySelect`, but when the diffuse ray has been dropped by
the geomN gate they are the container's only occupant and the
`freeidx==1` short-circuit returns them regardless.  `Pdf()` prices
exactly that event, so the old hemisphere-only quadrature was measuring
its own domain error.  On `N1 rd.05 rs.95 tilt30 th45`: 0.870% of
emitted directions are below the horizon, the hemisphere integral read
0.99180 against a measured emission probability of 0.99884, and the full
sphere reads 1.00047.  The same error was inflating gate 2 there — TVD
0.01196 of a 0.012 threshold, 99.7% of the band ON AN ARTIFACT; it is
0.00763 with the domain corrected, and the worst TVD over the whole file
drops to 0.00855, 71% of the band.  The threshold needed no retune once
the artifact was gone.  `AshikminShirleyAnisotropicPhongSPF` carries the
same domain as a guard: its own sampler CANNOT emit below the shading
hemisphere (`GenerateSpecularRay` rejects `Dot(k2, onb.w()) < 0`) and
its `Pdf` early-outs at `cosO <= 0`, so every row reads
`belowZ=0.00000` and no figure moved.

**Gate 3 (new): no scattered ray may carry a negative selection weight.**
Every ray in the container is inspected for the exact quantity
`RandomlySelect` reads.  This is the gate that the DL-100 sibling defect
(§3) needed; a one-sided "does the density integrate to the emission
probability" check could not see it, because the density was right and
the sampler was wrong.

**What each gate can and cannot see.**  Worth recording, because the
P1-1 chromatic defect was invisible to one of them by construction:
gate 1 moves mass between the lobes' totals only if the TOTAL changes,
so a wrong split between two lobes that each integrate correctly leaves
it at 0.0005-0.0047 against a 0.01 band while the real total variation
is 0.25-0.29.  A normalisation gate is necessary and nowhere near
sufficient; the histogram gate is what does the work.

Broader regression suites gated clean at HEAD: see the merge commit /
the slice report for the current counters.  Clean rebuild, zero compiler
warnings.

---

## 6. The quadrature's own systematic bias (review P2-3)

`C_D` and `q_i` are DETERMINISTIC stratified quadratures over the
sampler's own unit square, not Monte-Carlo estimates, so their error is
a systematic bias, not noise — it does not shrink with the test's draw
count and it is the same sign on every evaluation of a given
configuration.  It is disclosed here for the same reason DL-67 Slice 0
§4c/§4f disclosed its own.

Measured end to end by raising the grid and re-running the gates
(nothing else changed):

| | worst gate-1 `abs(intPdf - emitted)` | at |
|---|---|---|
| `kPhongQuadN` = 16 (shipped) | 0.00207 | `th=45 rd.05 rs.9 N5` |
| `kPhongQuadN` = 64 | 0.00038 | `tilt 55 deg th=30` |
| `kAshQuadN` = 16 (shipped) | 0.00785 | `Nu2Nv2 rd.05 rs.95 tilt20 th45` (NM) |
| `kAshQuadN` = 32 | 0.00279 | `Nu2Nv2 rd.05 rs.95 tilt20 th60` |

On the single most biased Phong row, `th=45 rd.05 rs.9 N5`, the integral
moves 0.99793 -> 0.99984 between grid 16 and grid 64: the shipped grid
is 0.191 pp low there.  Since that configuration's diffuse half is the
one `C_D` scales and the specular half is exact at the query direction,
dividing that deficit by the row's own `C_D` puts the bias ON `C_D`
itself at roughly 1.7-1.9% (a derived figure, not a direct
measurement — `C_D` is not exposed).

**Both grids stay at 16, deliberately.**  Raising `kAshQuadN` to 32
costs 3.9x (single-lobe `Pdf` 2576 ns -> ~10000 ns; per-channel 7651 ns
-> ~27500 ns) to move a residual that is already inside its band (Phong: 4.8x inside, worst 0.00207 vs kMassTol 0.01; Ashikmin: only 1.27x inside, worst 0.00785 with 0.00215 headroom, and a SYSTEMATIC quadrature bias that does not average away -- review correction 2026-09-17);
`kPhongQuadN` = 64 costs 13.5x (486 ns -> ~6520 ns).  What this bias
DOES mean for anyone reading a gate number: the gate-1 residuals in
these two suites are NOT all Monte-Carlo noise, so tightening
`kMassTol` below roughly 0.01 would start gating the quadrature's
resolution rather than the density's correctness.

---

## 7. The render movement is NOT evidence of improvement (review P2-2)

**DL-103 has two more instances, and they are these two SPFs.**  That
row records that un-guided default PT's escape-side MIS partner is
`effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf`
(`PathTracingIntegrator.cpp:3585`, stored via `misBsdfPdf` at `:3900`) —
the SELECTED LOBE's own density — while `LightSampler`'s NEE arms weight
against the material's AGGREGATE `pMaterial->Pdf(...)`
(`LightSampler.cpp:2600`, `:2763`, and the `PdfNM` twins).  At a
single-lobe SPF those coincide; at any multi-emit SPF they are different
quantities and `w_bsdf + w_nee != 1`.  The row was filed with
`SchlickSPF` as its example and its "check every other multi-emit SPF"
list did not name Phong or Ashikmin.  It should:
`IsotropicPhongSPF` and `AshikminShirleyAnisotropicPhongSPF` are exact
instances — both emit a diffuse and one-or-three specular lanes per
`Scatter()` call with per-lobe pdfs that differ from their aggregate,
which is the entire subject of this document.

The consequence for §7a below is blunt: **at a Phong or Ashikmin vertex
the NEE/escape MIS pair does not partition to one either before or after
this slice.**  A rendered mean therefore moving toward or away from
anything is not evidence that the render got closer to ground truth; the
evidence for THIS slice is the sampler-vs-density gates in §5, which
compare `Pdf()` against the sampler's own 600 000-draw histogram and are
independent of how any integrator then weights it.  The render numbers
below are reported as a magnitude check — "is this change visible at
all, and where" — and nothing more.

### 7a. Re-measured render deltas (review P2-1)

The figures that stood here before ("+7.65%" on the Phong sphere's
block, "−28% to +43%" on the Ashikmin regions) came from SINGLE
un-averaged 32-spp runs, which at those regions' brightness is mostly
Monte-Carlo noise.  Re-measured at 1024 spp with n = 6 independent
renders per build (renders are not deterministic run to run — RISE seeds
from an unsynchronised libc `rand()`), `oidn_denoise FALSE`,
`pixel_filter box`, EXR `Rec709RGB_Linear`, pre-slice `a4495f94` vs this
branch:

**`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene`**, one
`isotropic_phong_material` sphere of 9 objects, 160x160:

| region | pre-slice | this branch | delta | t |
|---|---|---|---|---|
| frame mean luminance | 0.996758 +/- 0.000121 | 0.997091 +/- 0.000297 | +0.033% +/- 0.013% | +2.54 |
| the phong sphere itself (disc r=21 px, 1387 px, located by projecting its own world position through the scene camera) | 0.821338 +/- 0.001213 | 0.823676 +/- 0.000317 | +0.285% +/- 0.062% | +4.57 |
| its 8x8 screen block (row 3, col 5) | 0.853583 | 0.861933 | +0.978% +/- 0.164% | +5.96 |

That block is also the largest-moving of the 64, and 3 of 64 move at
`abs(t) >= 3` — so the effect is real, localised on the material that
changed, and about 8x smaller than the number this document used to
quote.

**`scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene`**, path
guiding and adaptive sampling disabled for the comparison, 160x120:

| region | pre-slice | this branch | delta | t |
|---|---|---|---|---|
| frame mean luminance | 10.964884 +/- 0.004299 | 10.966679 +/- 0.003392 | +0.016% +/- 0.020% | +0.80 |
| gold-ellipsoid footprint (x 18-31, y 72-96) | 0.257538 +/- 0.004417 | 0.254618 +/- 0.004514 | −1.134% +/- 1.001% | −1.13 |
| largest-moving 8x8 block of 64 | 0.565213 | 0.581249 | +2.837% +/- 1.469% | +1.93 |

**Nothing in this scene moves significantly** — 0 of 64 blocks reach
`abs(t) >= 3`.  Two things about the fixture explain that, and both
correct the earlier account:

1. There are no "Ashikmin panels".  `gold_mat` is bound by exactly ONE
   object, `gold_ellipsoid` (`ellipsoid_geometry`, radii 0.225 / 0.425 /
   0.175).
2. It is a ~11 x 21 px object at the LEFT EDGE of a 160x120 frame,
   about 1.2% of the pixels, in a dim region (block mean ~0.27) of a
   frame whose mean is 10.96.  Its screen position was pinned
   empirically, not by trusting a projection: re-rendering with the
   ellipsoid's radii raised to 2.0 lights up blocks (rows 4-6, cols 0-1)
   by +147% / +118% / +74%, which is where it is.

So `pt_jewel_vault` is a poor instrument for this material at this
camera, and "−28% to +43% per-block" was single-run noise in dim blocks,
not a measured effect.  A directed fixture (an Ashikmin sphere placed on
screen, with chromatic Rd/Rs so the P1-1 defect is in play) would be the
right way to put a render number on the Ashikmin half; none exists yet.


### Whole-render cost (review round 2, 2026-09-17)

The per-call figures above are not the render-level number. On the Phong-sphere fixture used for the render A/B (1024 spp, 160x160, box filter, no OIDN; ONE of nine objects is `isotropic_phong_material`) the reviewer measured pre-slice 14.45 +/- 0.15 s (n=5) vs this branch 16.10 +/- 0.29 s (n=5): **+11.4 %** wall clock, every branch sample above every pre-slice sample. For calibration, DL-67 Slice 0 reported +2.5 % for one `schlick_material` of 15 objects and +26 % when every surface is Schlick. Under BDPT (master's DL-69 queries the aggregate `Pdf` for both `pdfFwd` and `pdfRev`) an Ashikmin vertex costs about 2 x 2.6 us; that combination is unmeasured -- the shipped exposure is five scenes, and in `bdpt_jewel_vault` a single gold ellipsoid.
